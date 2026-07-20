#!/usr/bin/env python3
# dev_scripts/paper/eml_dag_stats.py
"""Tree-vs-DAG statistics for the EML macro chain.

Mirrors the emit primitives of the reference compiler (eml_compiler_v4.py,
SI Sect. 2.1) and zelph's stdlib/eml.zph rules. "Tree size" equals
Mathematica's LeafCount, which counts heads as well as atoms (hence
LeafCount(ln x) = 7 despite 4 terminal symbols). "Distinct DAG nodes" counts
distinct subexpressions -- exactly the number of graph nodes zelph
materializes, since all terms are hash-consed.
"""


def EML(a, b):
    return ("eml", a, b)


def ex(z):
    return EML(z, "1")


def lg(z):
    return EML("1", ex(EML("1", z)))


ZERO = lg("1")


def sub(a, b):
    return EML(lg(a), ex(b))


def neg(z):
    return sub(ZERO, z)


def add(a, b):
    return sub(a, neg(b))


def inv(z):
    return ex(neg(lg(z)))


def mul(a, b):
    return ex(add(lg(a), lg(b)))


def div(a, b):
    return mul(a, inv(b))


def terminals(t):
    return 1 if isinstance(t, str) else terminals(t[1]) + terminals(t[2])


def tree_size(t):  # == Mathematica LeafCount (heads count too)
    return 1 if isinstance(t, str) else 1 + tree_size(t[1]) + tree_size(t[2])


def dag_nodes(t):
    seen = set()

    def walk(u):
        if u in seen:
            return
        seen.add(u)
        if not isinstance(u, str):
            walk(u[1])
            walk(u[2])

    walk(t)
    return len(seen)


CASES = [
    ("exp(x)", ex("x")),
    ("ln(x)", lg("x")),
    ("x - y", sub("x", "y")),
    ("-x", neg("x")),
    ("x + y", add("x", "y")),
    ("1/x", inv("x")),
    ("x * y", mul("x", "y")),
    ("x / y", div("x", "y")),
]


def main():
    # Cross-checks against the paper's SI (Sect. 2.4):
    assert tree_size(lg("x")) == 7, "SI cross-check failed: LeafCount(ln x)"
    assert tree_size(sub("x", "y")) == 11, "SI cross-check failed: LeafCount(x - y)"
    print(
        "| expression | terminal symbols | tree size (LeafCount) | distinct DAG nodes |"
    )
    print("|---|---|---|---|")
    for name, t in CASES:
        print(f"| {name} | {terminals(t)} | {tree_size(t)} | {dag_nodes(t)} |")


if __name__ == "__main__":
    main()
