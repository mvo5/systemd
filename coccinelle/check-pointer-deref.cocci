/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Detect pointer parameters that are dereferenced via *param (prefix
 * dereference) without a prior NULL check or assertion. In systemd style,
 * non-optional pointer parameters should have an assert() at the top of the
 * function.
 *
 * param->field dereferences are checked separately, in
 * check-pointer-deref-2.cocci: "->" is ubiquitous and the flow-sensitive scan
 * used here suffers a CTL state-explosion on large, dereference-dense functions,
 * so that check uses a cheaper flow-insensitive approach. Prefix dereferences
 * are rare, so the flow-sensitive scan below stays cheap.
 *
 * Usage:
 *   spatch --sp-file coccinelle/check-pointer-deref.cocci --dir src/boot/
 *
 * Note: this is a context-mode rule (flags, does not auto-fix). Each flagged
 * dereference should be reviewed: if the parameter is never NULL, add
 * assert(param) at the top. If it can legitimately be NULL, add an if() guard.
 */
@@
identifier fn, param;
identifier is_set =~ "_is_set$";
type T;
position p;
@@

fn(..., T *param, ...) {
  ... when != assert(param)
      when != assert(param != NULL)
      when != assert_se(param)
      when != assert_se(param != NULL)
      when != assert_raw(param)
      when != assert_raw(param != NULL)
      when != assert_return(param, ...)
      when != ASSERT_PTR(param)
      when != POINTER_MAY_BE_NULL(param)
      /* Any foo_is_set(param) guard implies param != NULL, since all *_is_set()
       * helpers in systemd return false for NULL input. Note the is_set regex
       * in identifier. */
      when != assert(is_set(param))
      when != assert_return(is_set(param), ...)
      when != \( is_set(param) \)
      when != \( param == NULL \| param != NULL \| !param \)
* *param@p
  ...
}
