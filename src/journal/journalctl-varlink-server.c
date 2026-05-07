/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-event.h"
#include "sd-id128.h"
#include "sd-journal.h"
#include "sd-varlink.h"

#include "alloc-util.h"
#include "journal-internal.h"
#include "journalctl.h"
#include "journalctl-filter.h"
#include "journalctl-varlink-server.h"
#include "json-util.h"
#include "logs-show.h"
#include "output-mode.h"
#include "runtime-scope.h"
#include "strv.h"
#include "unit-name.h"          /* IWYU pragma: keep */
#include "user-util.h"

typedef struct GetEntriesParameters {
        char **units;
        char **user_units;
        const char *namespace;
        const char *invocation_id;
        uid_t uid;
        int priority;
        int follow;
        uint64_t limit;
} GetEntriesParameters;

static void get_entries_parameters_done(GetEntriesParameters *p) {
        assert(p);

        p->units = strv_free(p->units);
        p->user_units = strv_free(p->user_units);
}

typedef struct FollowRequest {
        sd_varlink *link; /* owned: takes a ref so the connection survives the method callback returning */
        sd_journal *journal;
        sd_event_source *event_source;
} FollowRequest;

static FollowRequest* follow_request_free(FollowRequest *f) {
        if (!f)
                return NULL;

        sd_event_source_disable_unref(f->event_source);
        sd_journal_close(f->journal);
        sd_varlink_unref(f->link);
        return mfree(f);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(FollowRequest*, follow_request_free);

/* Emit one journal entry on the varlink connection. The two callsites differ in which sd-varlink helper
 * applies: the snapshot path uses sd_varlink_set_sentinel() and must call sd_varlink_replybo() (the framework
 * converts the replies into "more" notifications and replaces the final one with the sentinel); the follow
 * path doesn't use a sentinel and must call sd_varlink_notifybo() directly. */
static int emit_one_entry(sd_varlink *link, sd_journal *j, bool follow) {
        _cleanup_(sd_json_variant_unrefp) sd_json_variant *entry = NULL;
        int r;

        r = journal_entry_to_json(j, OUTPUT_SHOW_ALL, /* output_fields= */ NULL, &entry);
        if (r < 0)
                return r;
        if (r == 0)
                return 0; /* corrupted entry; skip */

        if (follow)
                return sd_varlink_notifybo(link, SD_JSON_BUILD_PAIR_VARIANT("entry", entry));
        return sd_varlink_replybo(link, SD_JSON_BUILD_PAIR_VARIANT("entry", entry));
}

static int on_journal_event(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        FollowRequest *f = ASSERT_PTR(userdata);
        int r;

        assert(s);

        r = sd_journal_process(f->journal);
        if (r < 0) {
                log_debug_errno(r, "Failed to process journal events: %m");
                return sd_varlink_error_errno(f->link, r);
        }

        for (;;) {
                r = sd_journal_next(f->journal);
                if (r < 0) {
                        log_debug_errno(r, "Failed to iterate journal: %m");
                        return sd_varlink_error_errno(f->link, r);
                }
                if (r == 0)
                        return 0;

                r = emit_one_entry(f->link, f->journal, /* follow= */ true);
                if (r < 0) {
                        log_debug_errno(r, "Failed to emit journal entry: %m");
                        return r;
                }
        }
}

int vl_method_get_entries(sd_varlink *link, sd_json_variant *parameters, sd_varlink_method_flags_t flags, void *userdata) {

        static const sd_json_dispatch_field dispatch_table[] = {
                { "units",        SD_JSON_VARIANT_ARRAY,         sd_json_dispatch_strv,         offsetof(GetEntriesParameters, units),         0 },
                { "uid",          _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uid_gid,      offsetof(GetEntriesParameters, uid),           0 },
                { "userUnits",    SD_JSON_VARIANT_ARRAY,         sd_json_dispatch_strv,         offsetof(GetEntriesParameters, user_units),    0 },
                { "namespace",    SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(GetEntriesParameters, namespace),     0 },
                { "priority",     _SD_JSON_VARIANT_TYPE_INVALID, json_dispatch_log_level,       offsetof(GetEntriesParameters, priority),      0 },
                { "invocationId", SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(GetEntriesParameters, invocation_id), 0 },
                { "follow",       SD_JSON_VARIANT_BOOLEAN,       sd_json_dispatch_tristate,     offsetof(GetEntriesParameters, follow),        0 },
                { "limit",        _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,       offsetof(GetEntriesParameters, limit),         0 },
                {}
        };

        _cleanup_(get_entries_parameters_done) GetEntriesParameters p = {
                .uid = UID_INVALID,
                .priority = -1,
                .follow = -1,
        };
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        int r;

        assert(link);
        assert(FLAGS_SET(flags, SD_VARLINK_METHOD_MORE));

        r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
        if (r != 0)
                return r;

        if (arg_varlink_runtime_scope == RUNTIME_SCOPE_SYSTEM && p.user_units && !uid_is_valid(p.uid))
                return sd_varlink_error_invalid_parameter_name(link, "uid");

        sd_id128_t invocation_id = SD_ID128_NULL;
        if (p.invocation_id) {
                r = sd_id128_from_string(p.invocation_id, &invocation_id);
                if (r < 0)
                        return sd_varlink_error_invalid_parameter_name(link, "invocationId");
        }

        bool follow = p.follow > 0;

        /* FIXME: this restriction should be removed eventually */
        if (p.limit > 10000)
                return sd_varlink_error_invalid_parameter_name(link, "limit");

        /* systemd ships with sensible defaults for the system/user services and the socket permissions so we
         * do not need to do extra sd_varlink_get_peer_uid() or policykit checks here. Note that we must NOT
         * pass SD_JOURNAL_ASSUME_IMMUTABLE in follow mode: that flag tells sd-journal it may ignore entries
         * appended after open(), which would silently break follow mode (no new entries would ever fire). */
        uint32_t open_flags = SD_JOURNAL_LOCAL_ONLY | (follow ? 0 : SD_JOURNAL_ASSUME_IMMUTABLE);
        r = sd_journal_open_namespace(&j, p.namespace, open_flags);
        if (r < 0)
                return r;

        r = journal_add_unit_matches(j, MATCH_UNIT_ALL, /* mangle_flags= */ 0, p.units, p.uid, p.user_units);
        if (r == -ENODATA)
                return sd_varlink_error(link, "io.systemd.JournalAccess.NoMatches", NULL);
        if (r < 0)
                return r;

        if (!sd_id128_is_null(invocation_id)) {
                /* add_matches_for_invocation_id() builds an OR-group (_SYSTEMD_INVOCATION_ID OR
                 * OBJECT_SYSTEMD_INVOCATION_ID OR INVOCATION_ID OR USER_INVOCATION_ID); terminate it with
                 * a conjunction so it AND's with whatever filter terms come next. */
                r = add_matches_for_invocation_id(j, invocation_id);
                if (r < 0)
                        return r;

                r = sd_journal_add_conjunction(j);
                if (r < 0)
                        return r;
        }

        if (p.priority >= 0) {
                for (int i = 0; i <= p.priority; i++) {
                        r = journal_add_matchf(j, "PRIORITY=%d", i);
                        if (r < 0)
                                return r;
                }

                r = sd_journal_add_conjunction(j);
                if (r < 0)
                        return r;
        }

        if (follow) {
                /* In follow mode we'd rather not miss any entries for a freshly-started invocation, so seek
                 * to head when the caller scoped the request via invocationId. Without that scoping, fall
                 * back to "tail then follow" semantics so we don't drown the caller in historical data. */
                if (!sd_id128_is_null(invocation_id))
                        r = sd_journal_seek_head(j);
                else
                        r = sd_journal_seek_tail(j);
                if (r < 0)
                        return r;

                /* Drain whatever is already available. */
                for (;;) {
                        r = sd_journal_next(j);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        r = emit_one_entry(link, j, /* follow= */ true);
                        if (r < 0)
                                return r;
                }

                /* Hand the journal off to an event-driven follower attached to this varlink connection. */
                int journal_fd = sd_journal_get_fd(j);
                if (journal_fd < 0)
                        return journal_fd;

                _cleanup_(follow_request_freep) FollowRequest *f = new(FollowRequest, 1);
                if (!f)
                        return -ENOMEM;
                *f = (FollowRequest) {
                        /* Take an explicit ref so the connection (and our state) survive the callback
                         * returning 0 without enqueuing a reply. sd-varlink rejects that pattern unless an
                         * extra ref has been stashed somewhere by the callback. The matching unref is in
                         * follow_request_free(). */
                        .link = sd_varlink_ref(link),
                        .journal = TAKE_PTR(j),
                };

                sd_event *event = sd_varlink_get_event(link);
                if (!event)
                        return -EINVAL;

                r = sd_event_add_io(event, &f->event_source, journal_fd, EPOLLIN, on_journal_event, f);
                if (r < 0)
                        return r;

                (void) sd_event_source_set_description(f->event_source, "varlink-journal-follow");

                /* Hand ownership of FollowRequest to the connection's userdata; the disconnect callback
                 * will free it (which also drops the link ref), at which point the connection actually
                 * tears down. */
                sd_varlink_set_userdata(link, TAKE_PTR(f));
                return 0;
        }

        /* Non-follow: existing snapshot behavior, "journalctl -n $limit". */
        r = sd_journal_seek_tail(j);
        if (r < 0)
                return r;

        uint64_t n = p.limit == 0 ? 100 : p.limit;

        r = sd_journal_previous_skip(j, n + 1);
        if (r < 0)
                return r;

        r = sd_varlink_set_sentinel(link, "io.systemd.JournalAccess.NoEntries");
        if (r < 0)
                return r;

        for (uint64_t i = 0; i < n; i++) {
                r = sd_journal_next(j);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = emit_one_entry(link, j, /* follow= */ false);
                if (r < 0)
                        return r;
        }

        return 0;
}

void vl_get_entries_disconnect(sd_varlink_server *server, sd_varlink *link, void *userdata) {
        assert(link);

        FollowRequest *f = sd_varlink_get_userdata(link);
        if (!f)
                return;

        /* The link is being torn down. Drop our state: follow_request_free() disables the event source
         * (so the io callback cannot fire after this point) and drops the connection ref we took when
         * the call entered follow mode. */
        sd_varlink_set_userdata(link, NULL);
        follow_request_free(f);
}
