#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Flag pointer parameters that are dereferenced without a NULL check or assertion.

This is the tree-sitter companion to coccinelle/check-pointer-deref.cocci. The
coccinelle check only covers the prefix `*param` dereference, because its
flow-sensitive engine suffers a CTL state-explosion on the ubiquitous `param->`
dereferences. This script uses a cheaper flow-insensitive set-difference (a
parameter is "guarded" if it is asserted or NULL-checked *anywhere* in the
function) and additionally covers `param->field` and `param[i]`.

In systemd style, non-optional pointer parameters should be guarded with an
assert() at the top of the function.

Usage:
  tools/check-pointer-deref.py src/basic [src/shared ...]

Exits non-zero (and prints one line per finding) when unguarded dereferences are
found, so it can be wired up as a meson test.
"""
import sys
import glob
import os
import re

from tree_sitter import Language, Parser
import tree_sitter_c

LANG = Language(tree_sitter_c.language())
parser = Parser(LANG)

ASSERT_MACROS = {
    "assert", "assert_se", "assert_raw", "assert_return",
    "ASSERT_PTR",
    "POINTER_MAY_BE_NULL",
}
# NULL-safe inspection helpers, mostly matched by suffix so new ones are covered
# with no per-name maintenance. By systemd convention these all tolerate a NULL
# argument: *_is_set() returns false, *isempty()/*is_empty() returns true, the
# collection accessors *_length()/*_size() return 0, and pidref_get_uid() returns
# -ESRCH. So passing the param to any of them means the author is treating it as
# optional -- e.g. strv_reverse() does "n = strv_length(l); if (n <= 1) return l;"
# which makes l[i] unreachable for NULL. pidref_get_uid is a hardcoded special-case
# (no usable suffix); add further such NULL-safe accessors here as needed.
GUARD_PREDICATE_RE = re.compile(r"(isempty|is_empty|_is_set|is_valid|_length|_size|pidref_get_uid)$")

def txt(n):
    return n.text.decode("utf8", "replace")

def walk(n):
    yield n
    for c in n.children:
        yield from walk(c)

def param_names(func_decl):
    """Yield identifier names of pointer parameters of a function_declarator."""
    plist = func_decl.child_by_field_name("parameters")
    if not plist:
        return
    for pd in plist.children:
        if pd.type != "parameter_declaration":
            continue
        d = pd.child_by_field_name("declarator")
        # Must be a pointer_declarator (one or more '*'); drill to the inner identifier.
        if not d or d.type != "pointer_declarator":
            continue
        cur = d
        while cur and cur.type in ("pointer_declarator", "array_declarator", "function_declarator"):
            cur = cur.child_by_field_name("declarator")
        if cur and cur.type == "identifier":
            yield txt(cur)

def is_ident(n, name):
    return n.type == "identifier" and txt(n) == name

def in_assert_call(idn):
    """True if identifier idn sits inside an assert-family / NULL-safe-helper call."""
    a = idn.parent
    while a is not None:
        if a.type == "call_expression":
            fn = a.child_by_field_name("function")
            fname = txt(fn) if fn and fn.type == "identifier" else ""
            if fname in ASSERT_MACROS or GUARD_PREDICATE_RE.search(fname):
                return True
        a = a.parent
    return False

def is_truthiness_or_null_guard(idn):
    """True if this occurrence of the param is a nullness/truthiness *test* of it
    (p, !p, p == NULL, p != NULL, p && ..., p ? ...), NOT a dereference like p->x."""
    par = idn.parent
    if par is None:
        return False
    # p == NULL / NULL == p (and !=). Compare by node .id; py-tree-sitter makes a
    # fresh wrapper per access, so `is` identity never matches.
    if par.type == "binary_expression":
        op = par.child_by_field_name("operator")
        left = par.child_by_field_name("left")
        right = par.child_by_field_name("right")
        if op and txt(op) in ("==", "!="):
            if (left and left.id == idn.id and right and txt(right) == "NULL") or \
               (right and right.id == idn.id and left and txt(left) == "NULL"):
                return True
        if op and txt(op) in ("&&", "||"):  # p && p->x  (short-circuit guard)
            return True
    # !p
    if par.type == "unary_expression" and par.children and txt(par.children[0]) == "!":
        return True
    # condition of if/while/do  -> bare `if (p)`
    if par.type == "parenthesized_expression":
        gp = par.parent
        if gp and gp.type in ("if_statement", "while_statement", "do_statement"):
            return True
    # condition of ternary  -> `p ? p->x : ...`
    if par.type == "conditional_expression":
        cond = par.child_by_field_name("condition")
        if cond and cond.id == idn.id:
            return True
    return False

def is_guarded(body, p):
    for n in walk(body):
        if is_ident(n, p) and (in_assert_call(n) or is_truthiness_or_null_guard(n)):
            return True
    return False

def derefs(body, p):
    """Yield (line, kind) for each *p / p->f / p[i] dereference of p."""
    for n in walk(body):
        if n.type == "field_expression":
            arg = n.child_by_field_name("argument")
            op = n.child_by_field_name("operator")
            if arg and is_ident(arg, p) and op and txt(op) == "->":
                yield (n.start_point[0] + 1, "->")
        elif n.type == "subscript_expression":
            arg = n.child_by_field_name("argument")
            if arg and is_ident(arg, p):
                yield (n.start_point[0] + 1, "[]")
        elif n.type == "pointer_expression":
            op = n.child_by_field_name("operator")
            arg = n.child_by_field_name("argument")
            if op and txt(op) == "*" and arg and is_ident(arg, p):
                yield (n.start_point[0] + 1, "*")

def check_file(path):
    src = open(path, "rb").read()
    tree = parser.parse(src)
    findings = []
    for n in walk(tree.root_node):
        if n.type != "function_definition":
            continue
        fd = n.child_by_field_name("declarator")
        body = n.child_by_field_name("body")
        # Pointer-returning functions (char* foo(...)) wrap the function_declarator
        # in one or more pointer_declarator layers; drill through them.
        while fd and fd.type == "pointer_declarator":
            fd = fd.child_by_field_name("declarator")
        if not fd or not body or fd.type != "function_declarator":
            continue
        name_node = fd.child_by_field_name("declarator")
        fname = txt(name_node) if name_node and name_node.type == "identifier" else "?"
        for p in param_names(fd):
            if is_guarded(body, p):
                continue
            ds = list(derefs(body, p))
            if ds:
                line, kind = ds[0]
                findings.append((path, line, fname, p, kind))
    return findings

def main():
    targets = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            targets += glob.glob(os.path.join(a, "**", "*.c"), recursive=True)
        else:
            targets.append(a)

    total = 0
    for f in sorted(targets):
        for (path, line, fn, p, kind) in check_file(f):
            print(f"{path}:{line}: {fn}(): '{p}' dereferenced via {kind} without assert/guard")
            total += 1

    if total > 0:
        print("")
        print("Pointer-deref check failed. For each flagged dereference, either:")
        print("  - Add assert(param)/ASSERT_PTR(param) at the top of the function (if the parameter must not be NULL)")
        print("  - Add an if (param) guard before the dereference (if NULL is valid)")
        print("  - Add POINTER_MAY_BE_NULL(param) if NULL is okay for param")
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
