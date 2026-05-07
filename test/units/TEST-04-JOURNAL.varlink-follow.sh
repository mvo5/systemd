#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Exercises io.systemd.JournalAccess.GetEntries(follow=true, invocationId=...) in
# combination with io.systemd.Unit.StartTransient: start a transient service via
# varlink, then tail its journal output (stdout + stderr + lifecycle records) over
# a single follow call until the unit's terminal record arrives, and verify that
# all expected entries showed up in the stream.
set -eux
set -o pipefail

JOURNAL_SOCKET="/run/systemd/io.systemd.JournalAccess"
MANAGER_SOCKET="/run/systemd/io.systemd.Manager"

# SD_MESSAGE_UNIT_PROCESS_EXIT — emitted by PID1 with INVOCATION_ID=... when the
# main exec process of a unit exits. Used as the terminal record for the follow
# loop, so the client can detect "the process ended" purely from the journal feed.
TERMINAL_MESSAGE_ID="98e322203f7a4ed290d09fe03c09fe15"

UNIT="varlink-follow-transient-$RANDOM.service"
LOG_FILE="$(mktemp)"
FOLLOW_PID=""

cleanup() {
    if [[ -n "$FOLLOW_PID" ]]; then
        kill "$FOLLOW_PID" 2>/dev/null || true
        wait "$FOLLOW_PID" 2>/dev/null || true
    fi
    systemctl stop "$UNIT" 2>/dev/null || true
    systemctl reset-failed "$UNIT" 2>/dev/null || true
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

# Start a Type=simple transient unit that produces a few lines on both stdout and
# stderr with a small delay in between, then exits. Type=simple ensures the unit
# becomes active (and is assigned an InvocationID) the moment the process forks,
# so we don't need RemainAfterExit to keep the unit around long enough to query.
varlinkctl call "$MANAGER_SOCKET" io.systemd.Unit.StartTransient "$(cat <<EOF
{
  "context": {
    "ID": "$UNIT",
    "Service": {
      "ExecStart": [
        {"path": "/bin/sh", "arguments": ["/bin/sh", "-c", "echo stdout-line-1; echo stderr-line-1 >&2; sleep 1; echo stdout-line-2; echo stderr-line-2 >&2; sleep 1"]}
      ]
    }
  }
}
EOF
)" >/dev/null

# StartTransient returns when the start job is queued; the InvocationID is
# assigned a moment later when the unit actually starts. Poll for it.
INVOCATION_ID=""
for _ in $(seq 1 100); do
    INVOCATION_ID="$(systemctl show -P InvocationID "$UNIT" 2>/dev/null)"
    if [[ -n "$INVOCATION_ID" && "$INVOCATION_ID" != "00000000000000000000000000000000" ]]; then
        break
    fi
    sleep 0.1
done
test -n "$INVOCATION_ID"
test "$INVOCATION_ID" != "00000000000000000000000000000000"

# Start the follower in the background. follow=true + invocationId scopes the
# stream to this one run and seeks from the head, so we won't miss entries that
# were already produced before this call attached.
varlinkctl call --more "$JOURNAL_SOCKET" io.systemd.JournalAccess.GetEntries \
    "{\"follow\":true,\"invocationId\":\"$INVOCATION_ID\"}" >"$LOG_FILE" 2>&1 &
FOLLOW_PID=$!

# Wait until the terminal lifecycle record arrives in the stream. This proves the
# client can detect "process ended" purely from the journal feed.
if ! timeout 30 bash -c "until grep \"$TERMINAL_MESSAGE_ID\" \"$LOG_FILE\" >/dev/null; do sleep 0.2; done"; then
    echo "=== timeout waiting for terminal record; dumping LOG_FILE ==="
    wc -l "$LOG_FILE"
    head -c 8192 "$LOG_FILE" || true
    echo "=== unit state ==="
    systemctl status "$UNIT" --no-pager || true
    journalctl --no-pager -u "$UNIT" -o short-monotonic | tail -50 || true
    exit 1
fi

# Close the follow connection.
kill "$FOLLOW_PID"
wait "$FOLLOW_PID" 2>/dev/null || true
FOLLOW_PID=""

# All four lines from the script must have made it through the live stream.
grep stdout-line-1 "$LOG_FILE" >/dev/null
grep stdout-line-2 "$LOG_FILE" >/dev/null
grep stderr-line-1 "$LOG_FILE" >/dev/null
grep stderr-line-2 "$LOG_FILE" >/dev/null

# Cross-check: stdout entries must have _TRANSPORT=stdout (i.e. they really came
# from the service's stdout stream as captured by journald, not a stray match).
jq --seq --slurp -e \
    '[.[] | select(.entry.MESSAGE == "stdout-line-1") | .entry._TRANSPORT] | any(. == "stdout")' \
    <"$LOG_FILE" >/dev/null

# And the terminal record must be scoped to *our* invocation.
jq --seq --slurp -e \
    --arg id "$INVOCATION_ID" \
    "[.[] | select(.entry.MESSAGE_ID == \"$TERMINAL_MESSAGE_ID\") | .entry.INVOCATION_ID] | any(ascii_downcase == (\$id | ascii_downcase))" \
    <"$LOG_FILE" >/dev/null

# Sanity check: a follow call against a non-existent invocation ID should not produce
# any entries and should still terminate cleanly when the client closes the connection.
EMPTY_LOG="$(mktemp)"
varlinkctl call --more "$JOURNAL_SOCKET" io.systemd.JournalAccess.GetEntries \
    '{"follow":true,"invocationId":"deadbeefdeadbeefdeadbeefdeadbeef"}' >"$EMPTY_LOG" 2>&1 &
EMPTY_PID=$!
sleep 1
kill "$EMPTY_PID"
wait "$EMPTY_PID" 2>/dev/null || true
test ! -s "$EMPTY_LOG" || ! grep entry "$EMPTY_LOG" >/dev/null
rm -f "$EMPTY_LOG"

# Reject malformed invocation IDs.
(! varlinkctl call --more "$JOURNAL_SOCKET" io.systemd.JournalAccess.GetEntries \
    '{"follow":true,"invocationId":"not-a-uuid"}')

echo "TEST-04-JOURNAL.varlink-follow: OK"
