/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Detect pointer parameters that are dereferenced via param->field without a
 * NULL check or assertion. Companion to check-pointer-deref.cocci, which checks
 * *param prefix dereferences.
 *
 * Usage:
 *   spatch --sp-file coccinelle/check-pointer-deref-2.cocci --dir src/boot/
 *
 * This is a context-mode rule (flags, does not auto-fix). Each flagged
 * dereference should be reviewed: if the parameter is never NULL, add
 * assert(param) at the top. If it can legitimately be NULL, add an if() guard
 * or POINTER_MAY_BE_NULL(param).
 *
 * "->" is ubiquitous, so the flow-sensitive "... when != <guard> ..." approach
 * suffers a CTL state-explosion on large, dereference-dense functions (minutes
 * per file). Instead this uses a flow-INsensitive set-difference: rule @guarded@
 * records every parameter guarded *anywhere* in the function, and rule @arrow@
 * flags ->dereferences of the rest. The trade-off is that a guard placed *after*
 * a dereference, or on only some branches, still counts as guarding the
 * parameter -- acceptable because systemd asserts pointers unconditionally at
 * the top of the function.
 */

/* Record the declaration position of every pointer parameter that is guarded
 * (asserted / null-checked) anywhere in the function body.
 *
 * Performance exclusions (fn != {...}): a few huge, ->-dense functions make the
 * matcher blow up (minutes each). They all assert their pointers, so nothing is
 * missed. Add a name here if a new function times out:
 *   - produce_plot_as_svg()             src/analyze/analyze-plot.c
 *   - exec_context_deserialize()        src/core/execute-serialize.c
 *   - exec_cgroup_context_deserialize() src/core/execute-serialize.c */
@guarded exists@
identifier fn != { produce_plot_as_svg, exec_context_deserialize, exec_cgroup_context_deserialize };
identifier param;
identifier is_set =~ "_is_set$";
type T;
position p;
@@

fn(..., T *param@p, ...) {
  ...
  \( assert(param)
  \| assert(param != NULL)
  \| assert_se(param)
  \| assert_se(param != NULL)
  \| assert_raw(param)
  \| assert_raw(param != NULL)
  \| assert_return(param, ...)
  \| ASSERT_PTR(param)
  \| POINTER_MAY_BE_NULL(param)
  \| assert(is_set(param))
  \| assert_return(is_set(param), ...)
  \| is_set(param)
  \| param == NULL
  \| param != NULL
  \| !param
  \)
  ...
}

/* Flag ->dereferences of parameters not recorded as guarded above. */
@arrow exists@
identifier fn != { produce_plot_as_svg, exec_context_deserialize, exec_cgroup_context_deserialize };
identifier param;
identifier fld;
type T;
position q != guarded.p;
@@

fn(..., T *param@q, ...) {
  ...
* param->fld
  ...
}
