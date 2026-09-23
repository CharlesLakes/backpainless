#!/usr/bin/env python3
"""Brute-force backbone checker for small CNF formulas.

Usage:
    python3 scripts/check_backbone.py <formula.cnf>                 # print the backbone
    python3 scripts/check_backbone.py <formula.cnf> <backpainless.log>  # compare with a backpainless output

backpainless prints the status of the formula (`s SATISFIABLE` / `s UNSATISFIABLE`) and, when it is satisfiable,
one `b <lit>` line per backbone literal terminated by `b 0` (CadiBack format).
Enumerates all 2^n assignments, so only use it with small formulas (default limit: 22 variables).
"""

import sys

MAX_VARS = 22


def parse_cnf(path):
    nb_vars = 0
    clauses = []
    current = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] in "c%":
                continue
            if line[0] == "p":
                nb_vars = int(line.split()[2])
                continue
            for tok in line.split():
                lit = int(tok)
                if lit == 0:
                    clauses.append(current)
                    current = []
                else:
                    current.append(lit)
                    nb_vars = max(nb_vars, abs(lit))
    if current:
        clauses.append(current)
    return nb_vars, clauses


def brute_force_backbone(nb_vars, clauses):
    """Returns (is_sat, backbone) where backbone is the sorted list of literals true in every model."""
    if nb_vars > MAX_VARS:
        sys.exit(f"Formula has {nb_vars} variables, brute force is limited to {MAX_VARS}")

    # Clause as (positive mask, negative mask) over bit (var - 1)
    masks = []
    for cls in clauses:
        pos = neg = 0
        for lit in cls:
            if lit > 0:
                pos |= 1 << (lit - 1)
            else:
                neg |= 1 << (-lit - 1)
        masks.append((pos, neg))

    full = (1 << nb_vars) - 1
    always_true = full   # variables true in every model found so far
    always_false = full  # variables false in every model found so far
    is_sat = False

    for assignment in range(1 << nb_vars):
        if all((assignment & pos) or (~assignment & neg) for pos, neg in masks):
            is_sat = True
            always_true &= assignment
            always_false &= ~assignment & full
            if not always_true and not always_false:
                break

    if not is_sat:
        return False, []

    backbone = []
    for var in range(1, nb_vars + 1):
        bit = 1 << (var - 1)
        if always_true & bit:
            backbone.append(var)
        elif always_false & bit:
            backbone.append(-var)
    return True, backbone


def parse_painless_output(path):
    status = None
    lits = []
    with open(path) as f:
        for line in f:
            if line.startswith("s "):
                status = line[2:].strip()
            elif line.startswith("b "):
                lit = int(line.split()[1])
                if lit != 0:
                    lits.append(lit)
    return status, sorted(lits, key=abs)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)

    nb_vars, clauses = parse_cnf(sys.argv[1])
    is_sat, backbone = brute_force_backbone(nb_vars, clauses)

    print(f"c formula: {'SATISFIABLE' if is_sat else 'UNSATISFIABLE'}, {nb_vars} variables, {len(clauses)} clauses")
    print(f"c backbone size: {len(backbone)}")
    for lit in backbone:
        print(f"b {lit}")
    print("b 0")

    if len(sys.argv) == 3:
        status, found = parse_painless_output(sys.argv[2])
        expected_status = "SATISFIABLE" if is_sat else "UNSATISFIABLE"
        if status == expected_status and found == backbone:
            print("c OK: backpainless backbone matches")
        else:
            print(f"c MISMATCH: expected '{expected_status}' {backbone}, backpainless gave '{status}' {found}")
            sys.exit(1)


if __name__ == "__main__":
    main()
