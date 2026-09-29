#!/usr/bin/env python3
"""Regression tests for backpainless.

Suites:
    bruteforce  small random and hand-written CNFs (<= 14 variables), every model enumerated, checked in many
                configurations (threads, -bb-chunk, -bb-no-flip, sharing strategies)
    sharing     medium planted 3-SAT instances solved by a parallel portfolio with frequent sharing rounds. Checks
                that clauses are really exchanged (Sharer stats) and that the backbone matches an independent oracle
                (the standalone CaDiCaL binary, one SAT call per candidate literal)
    robustness  output format, -no-backbone, timeout, repeated runs with many threads (interrupt races),
                -bb-share-units validation and, with the debug binary, which workers export backbone units
    mpi         distributed runs with mpirun (not part of 'all', needs -dist support on the machine)

Usage:
    python3 tests/run_tests.py                     # all suites except mpi
    python3 tests/run_tests.py bruteforce sharing  # selected suites
    python3 tests/run_tests.py --quick             # fewer instances
Exit code 0 when every test passes.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # no __pycache__ in scripts/
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from check_backbone import brute_force_backbone  # noqa: E402

CADICAL = os.path.join(ROOT, "solvers", "cadical", "build", "cadical")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
RUN_TIMEOUT = 120  # seconds, a run taking longer is reported as a hang

# ---------------------------------------------------------------------------------------------------------------------
# Formulas


def write_cnf(path, nb_vars, clauses):
    with open(path, "w") as f:
        f.write(f"p cnf {nb_vars} {len(clauses)}\n")
        for cls in clauses:
            f.write(" ".join(map(str, cls)) + " 0\n")


def random_kcnf(rng, nb_vars, nb_clauses, k):
    return [[v * rng.choice((1, -1)) for v in rng.sample(range(1, nb_vars + 1), min(k, nb_vars))]
            for _ in range(nb_clauses)]


def planted_kcnf(rng, nb_vars, nb_clauses, k):
    """Random k-CNF satisfied by a hidden model: always SAT, with a large backbone above the threshold."""
    model = [rng.choice((1, -1)) for _ in range(nb_vars)]
    clauses = []
    while len(clauses) < nb_clauses:
        cls = [v * rng.choice((1, -1)) for v in rng.sample(range(1, nb_vars + 1), min(k, nb_vars))]
        if any(lit * model[abs(lit) - 1] > 0 for lit in cls):
            clauses.append(cls)
    return clauses


def handwritten_formulas():
    """Edge cases: (name, nb_vars, clauses)."""
    return [
        ("units_only", 4, [[1], [-2], [3]]),
        ("unused_vars", 8, [[1, 2], [-1, 2], [3, -4]]),                  # var 2 in backbone, 5..8 free
        ("tautology", 3, [[1, -1], [2, 3], [-2]]),
        ("duplicate_lits", 3, [[1, 1, 2], [-2, -2], [3, 3]]),
        ("empty_clause", 3, [[1, 2], []]),
        ("contradicting_units", 2, [[1], [-1]]),
        ("equivalence_chain", 6, [[-1, 2], [-2, 3], [-3, 4], [-4, 5], [-5, 6], [-6, 1], [1, 6]]),  # all true
        ("xor_no_backbone", 3, [[1, 2], [-1, -2], [2, 3], [-2, -3]]),     # 1 == 3 != 2, empty backbone
        ("pigeonhole_3_2", 6, [[1, 2], [3, 4], [5, 6], [-1, -3], [-1, -5], [-3, -5], [-2, -4], [-2, -6], [-4, -6]]),
        ("all_clauses_2vars", 2, [[1, 2], [1, -2], [-1, 2]]),             # backbone {1, 2}
    ]


def random_small_formulas(rng, count):
    """(name, nb_vars, clauses) covering SAT with/without backbone and UNSAT."""
    formulas = []
    for i in range(count):
        n = rng.randint(3, 14)
        kind = rng.random()
        if kind < 0.35:
            k = 3
            m = int(n * rng.uniform(2.0, 6.0))
            clauses = random_kcnf(rng, n, m, k)
            name = f"rand3_{i}"
        elif kind < 0.55:
            m = int(n * rng.uniform(0.5, 1.5))
            clauses = random_kcnf(rng, n, m, 2)
            name = f"rand2_{i}"
        elif kind < 0.85:
            m = int(n * rng.uniform(3.0, 8.0))
            clauses = planted_kcnf(rng, n, m, 3)
            name = f"planted_{i}"
        else:
            # mixed clause lengths, including units
            clauses = [[v * rng.choice((1, -1)) for v in rng.sample(range(1, n + 1), rng.randint(1, min(4, n)))]
                       for _ in range(int(n * rng.uniform(1.0, 4.0)))]
            name = f"mixed_{i}"
        formulas.append((name, n, clauses))
    return formulas


# ---------------------------------------------------------------------------------------------------------------------
# Running backpainless and parsing its output


class Run:
    def __init__(self, cmd, returncode, stdout, elapsed):
        self.cmd = cmd
        self.returncode = returncode
        self.stdout = ANSI.sub("", stdout)
        self.elapsed = elapsed
        self.status = None
        self.backbone = []
        self.has_terminator = False
        for line in self.stdout.splitlines():
            if line.startswith("s "):
                self.status = line[2:].strip()
            elif line.startswith("b "):
                lit = int(line.split()[1])
                if lit == 0:
                    self.has_terminator = True
                else:
                    self.backbone.append(lit)
        self.backbone.sort(key=abs)

    def sharing_stats(self):
        """[(rounds, receivedCls, sharedCls)] for each strategy printed by the sharers."""
        rounds = [int(x) for x in re.findall(r"Sharer \d+: executionTime: [\d.]+, rounds: (\d+)", self.stdout)]
        stats = [(int(r), int(s)) for r, s in re.findall(r"receivedCls (\d+), sharedCls (\d+)", self.stdout)]
        return rounds, stats


def run_backpainless(binary, cnf, args, timeout=RUN_TIMEOUT, launcher=()):
    cmd = [*launcher, binary, cnf, *args]
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return Run(cmd, proc.returncode, proc.stdout, time.monotonic() - start)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        return Run(cmd, "HANG", out, time.monotonic() - start)


EXIT_CODES = {"SATISFIABLE": 10, "UNSATISFIABLE": 20, "UNKNOWN": 0}


def check_run(run, expected_sat, expected_backbone):
    """Returns an error message, or None when the run is correct."""
    if run.returncode == "HANG":
        return f"no answer after {RUN_TIMEOUT}s"
    expected_status = "SATISFIABLE" if expected_sat else "UNSATISFIABLE"
    if run.status != expected_status:
        return f"status {run.status!r} (exit {run.returncode}), expected {expected_status!r}"
    if run.returncode != EXIT_CODES[expected_status]:
        return f"exit code {run.returncode}, expected {EXIT_CODES[expected_status]}"
    if expected_sat:
        if not run.has_terminator:
            return "missing 'b 0' terminator"
        if run.backbone != expected_backbone:
            missing = sorted(set(expected_backbone) - set(run.backbone), key=abs)
            extra = sorted(set(run.backbone) - set(expected_backbone), key=abs)
            return f"wrong backbone: missing {missing}, extra {extra}"
    bad = [line for line in run.stdout.splitlines() if line and not line.startswith(("c", "s ", "b "))]
    if bad:
        return f"stdout line not starting with 'c': {bad[0]!r}"
    return None


# ---------------------------------------------------------------------------------------------------------------------
# Independent oracle for medium instances: standalone CaDiCaL, no backpainless code involved


def cadical_solve(nb_vars, clauses, extra, workdir):
    fd, path = tempfile.mkstemp(suffix=".cnf", dir=workdir)
    os.close(fd)
    write_cnf(path, nb_vars, clauses + extra)
    out = subprocess.run([CADICAL, "-q", path], capture_output=True, text=True).stdout
    os.unlink(path)
    if "s UNSATISFIABLE" in out:
        return None
    if "s SATISFIABLE" not in out:
        raise RuntimeError("standalone CaDiCaL gave no answer")
    model = {}
    for line in out.splitlines():
        if line.startswith("v "):
            for tok in line.split()[1:]:
                lit = int(tok)
                if lit:
                    model[abs(lit)] = lit
    return model


def oracle_backbone(nb_vars, clauses, workdir):
    """(is_sat, backbone): a literal l of a model is in the backbone iff F and -l is UNSAT."""
    model = cadical_solve(nb_vars, clauses, [], workdir)
    if model is None:
        return False, []
    candidates = [model.get(v, v) for v in range(1, nb_vars + 1)]
    backbone = []
    while candidates:
        lit = candidates.pop()
        other = cadical_solve(nb_vars, clauses, [[-lit]], workdir)
        if other is None:
            backbone.append(lit)
        else:
            candidates = [c for c in candidates if other.get(abs(c), abs(c)) == c]
    return True, sorted(backbone, key=abs)


# ---------------------------------------------------------------------------------------------------------------------
# Suites


class Report:
    def __init__(self):
        self.passed = 0
        self.failures = []

    def record(self, name, error, run=None):
        if error is None:
            self.passed += 1
        else:
            cmd = " ".join(os.path.relpath(c, ROOT) if os.path.isabs(c) else c for c in run.cmd) if run else ""
            self.failures.append(f"{name}: {error}\n      {cmd}")
            print(f"  FAIL {name}: {error}", flush=True)


BRUTEFORCE_CONFIGS = [
    ["-c=1"],
    ["-c=1", "-bb-chunk=1"],
    ["-c=1", "-bb-chunk=10"],
    ["-c=1", "-bb-no-flip"],
    ["-c=1", "-bb-chunk=1", "-bb-no-flip"],
    ["-c=4"],
    ["-c=4", "-shr-strat=2"],
    ["-c=8", "-shr-strat=3", "-bb-chunk=10"],
    ["-c=8", "-shr-sleep=1000", "-init-sleep=100"],
    ["-c=4", "-bb-share-units=0"],
    ["-c=8", "-bb-share-units=10", "-bb-chunk=1", "-shr-sleep=1000", "-init-sleep=100"],
]


def suite_bruteforce(opts, report, workdir):
    rng = random.Random(opts.seed)
    formulas = handwritten_formulas() + random_small_formulas(rng, 25 if opts.quick else 120)
    jobs = []
    for name, n, clauses in formulas:
        path = os.path.join(workdir, f"{name}.cnf")
        write_cnf(path, n, clauses)
        is_sat, backbone = brute_force_backbone(n, [c for c in clauses])
        for cfg in BRUTEFORCE_CONFIGS:
            jobs.append((f"{name} {' '.join(cfg)}", path, cfg, is_sat, backbone))

    nb_sat = sum(1 for _, n, c in formulas if brute_force_backbone(n, c)[0])
    print(f"[bruteforce] {len(formulas)} formulas ({nb_sat} SAT), {len(BRUTEFORCE_CONFIGS)} configurations, "
          f"{len(jobs)} runs", flush=True)

    def work(job):
        name, path, cfg, is_sat, backbone = job
        run = run_backpainless(opts.binary, path, cfg)
        return name, check_run(run, is_sat, backbone), run

    with ThreadPoolExecutor(opts.jobs) as pool:
        for name, error, run in pool.map(work, jobs):
            report.record(name, error, run)


# (vars, clause/var ratio, seed): each takes ~0.1s to ~2s with 8 threads, long enough for many sharing rounds
SHARING_INSTANCES = [(150, 5.0, 2), (200, 4.8, 3), (250, 4.6, 4)]
SHARING_CONFIGS = [
    ["-c=8", "-shr-strat=1"],
    ["-c=8", "-shr-strat=2"],
    ["-c=8", "-shr-strat=3"],
    ["-c=8", "-shr-strat=1", "-one-sharer"],
    ["-c=8", "-shr-strat=1", "-bb-chunk=1"],
    ["-c=8", "-shr-strat=1", "-bb-chunk=10", "-bb-no-flip"],
    ["-c=14", "-shr-strat=3"],
    ["-c=8", "-shr-strat=1", "-bb-share-units=0"],
    ["-c=8", "-shr-strat=2", "-bb-share-units=10", "-bb-chunk=1"],
    ["-c=8", "-shr-strat=3", "-bb-share-units=011", "-bb-chunk=10"],
]
SHARING_FAST = ["-shr-sleep=10000", "-init-sleep=1000"]


def suite_sharing(opts, report, workdir):
    if not os.access(CADICAL, os.X_OK):
        print(f"[sharing] SKIPPED: {CADICAL} not built (run make)")
        return
    instances = SHARING_INSTANCES[:2] if opts.quick else SHARING_INSTANCES
    print(f"[sharing] {len(instances)} planted 3-SAT instances x {len(SHARING_CONFIGS)} configurations", flush=True)
    for n, ratio, seed in instances:
        rng = random.Random(seed)
        clauses = planted_kcnf(rng, n, int(n * ratio), 3)
        name = f"planted_n{n}_r{ratio}_s{seed}"
        path = os.path.join(workdir, f"{name}.cnf")
        write_cnf(path, n, clauses)
        is_sat, backbone = oracle_backbone(n, clauses, workdir)
        print(f"  {name}: oracle backbone size {len(backbone)}", flush=True)

        exchanged_somewhere = False
        for cfg in SHARING_CONFIGS:
            run = run_backpainless(opts.binary, path, cfg + SHARING_FAST)
            test = f"{name} {' '.join(cfg)}"
            error = check_run(run, is_sat, backbone)
            rounds, stats = run.sharing_stats()
            if error is None and not stats:
                error = "no sharing statistics printed (was a sharing strategy instantiated?)"
            shared = sum(s for _, s in stats)
            received = sum(r for r, _ in stats)
            if shared > 0 and received > 0:
                exchanged_somewhere = True
            print(f"    {' '.join(cfg):45s} {run.elapsed:6.2f}s  rounds {sum(rounds):4d}  "
                  f"received {received:7d}  shared {shared:7d}  {'OK' if error is None else 'FAIL'}", flush=True)
            report.record(test, error, run)
        # a single run may end before the first round, but at least one per instance must exchange clauses
        report.record(f"{name} clauses were exchanged",
                      None if exchanged_somewhere else "no configuration shared any clause")


def suite_robustness(opts, report, workdir):
    print("[robustness]", flush=True)
    demo = os.path.join(ROOT, "examples", "backbone_demo.cnf")
    with open(demo) as f:
        demo_clauses = []
        nb_vars = 0
        for line in f:
            if line.startswith("p"):
                nb_vars = int(line.split()[2])
            elif line.strip() and not line.startswith("c"):
                demo_clauses.append([int(t) for t in line.split() if t != "0"])
    demo_sat, demo_bb = brute_force_backbone(nb_vars, demo_clauses)

    # repeated parallel runs: the first worker to finish interrupts the others, races show up as wrong/missing answers
    repeats = 10 if opts.quick else 40
    for i in range(repeats):
        run = run_backpainless(opts.binary, demo, ["-c=14", "-shr-sleep=1000", "-init-sleep=100"])
        report.record(f"demo repeat {i} -c=14", check_run(run, demo_sat, demo_bb), run)

    # -no-backbone: only the status line outside comments
    run = run_backpainless(opts.binary, demo, ["-c=4", "-no-backbone"])
    error = None
    if run.status != "SATISFIABLE" or run.returncode != 10:
        error = f"status {run.status!r}, exit {run.returncode}"
    elif any(line.startswith("b ") for line in run.stdout.splitlines()):
        error = "'b' lines printed with -no-backbone"
    report.record("-no-backbone", error, run)

    # -bb-share-units: an invalid mask is rejected before solving
    for mask in ("", "2", "1a"):
        run = run_backpainless(opts.binary, demo, [f"-bb-share-units={mask}"])
        error = None
        if run.returncode != PERR_ARGS_ERROR or run.status is not None:
            error = f"exit {run.returncode}, status {run.status!r}, expected exit {PERR_ARGS_ERROR} and no answer"
        report.record(f"-bb-share-units={mask!r} rejected", error, run)

    suite_share_units_mask(opts, report, workdir)

    # timeout on a hard instance: s UNKNOWN, exit 0, in about t seconds
    rng = random.Random(1)
    hard = os.path.join(workdir, "hard.cnf")
    write_cnf(hard, 800, planted_kcnf(rng, 800, int(800 * 4.6), 3))
    run = run_backpainless(opts.binary, hard, ["-c=4", "-t=2"], timeout=30)
    error = None
    if run.returncode == "HANG":
        error = "timeout not honoured"
    elif run.status != "UNKNOWN" or run.returncode != 0:
        error = f"status {run.status!r}, exit {run.returncode}, expected UNKNOWN / 0"
    elif run.elapsed > 10:
        error = f"took {run.elapsed:.1f}s with -t=2"
    report.record("timeout -t=2", error, run)
    print(f"  {report.passed} passed so far", flush=True)


PERR_ARGS_ERROR = 250  # -6 in utils/ErrorCodes.hpp, as a process exit code
DEBUG_BINARY = os.path.join(ROOT, "backpainlessd")
EXPORT_LOG = re.compile(r"CadiBack (\d+) exported backbone literal (-?\d+)")


def suite_share_units_mask(opts, report, workdir):
    """With the debug binary (-v=2 logs each exported backbone unit): only the workers selected by the mask export."""
    if not os.access(DEBUG_BINARY, os.X_OK):
        print(f"  -bb-share-units mask check SKIPPED: {DEBUG_BINARY} not built (run make debug)")
        return
    n, ratio, seed = SHARING_INSTANCES[1]
    clauses = planted_kcnf(random.Random(seed), n, int(n * ratio), 3)
    path = os.path.join(workdir, "share_units.cnf")
    write_cnf(path, n, clauses)
    nb_solvers = 8
    for mask in ("1", "0", "10", "01", "001"):
        run = run_backpainless(DEBUG_BINARY, path, [f"-c={nb_solvers}", "-v=2", f"-bb-share-units={mask}",
                                                   "-bb-chunk=1", *SHARING_FAST])
        exporters = {int(solver) for solver, _ in EXPORT_LOG.findall(run.stdout)}
        allowed = {i for i in range(nb_solvers) if mask[i % len(mask)] == "1"}
        lits = {int(lit) for _, lit in EXPORT_LOG.findall(run.stdout)}
        error = None
        if run.status != "SATISFIABLE":
            error = f"status {run.status!r}, exit {run.returncode}"
        elif not exporters <= allowed:
            error = f"workers {sorted(exporters - allowed)} exported although the mask disables them"
        elif allowed and not exporters:
            error = "no worker exported a backbone unit"
        elif not lits <= set(run.backbone):
            error = f"exported literals {sorted(lits - set(run.backbone), key=abs)} are not in the backbone"
        print(f"  -bb-share-units={mask:4s} exporters {sorted(exporters)}  {'OK' if error is None else 'FAIL'}",
              flush=True)
        report.record(f"-bb-share-units={mask} honoured", error, run)


def suite_mpi(opts, report, workdir):
    mpirun = shutil.which("mpirun")
    if not mpirun or not os.access(CADICAL, os.X_OK):
        print("[mpi] SKIPPED: mpirun or standalone CaDiCaL not found")
        return
    print("[mpi]", flush=True)
    n, ratio, seed = SHARING_INSTANCES[1]
    clauses = planted_kcnf(random.Random(seed), n, int(n * ratio), 3)
    path = os.path.join(workdir, "mpi.cnf")
    write_cnf(path, n, clauses)
    is_sat, backbone = oracle_backbone(n, clauses, workdir)
    for extra in (["-gshr-strat=1"], ["-gshr-strat=2"], ["-gshr-strat=3"], ["-gshr-strat=1", "-bb-share-units=10"]):
        run = run_backpainless(opts.binary, path, ["-c=2", "-dist", *extra, *SHARING_FAST],
                               launcher=(mpirun, "--oversubscribe", "-np", "3"))
        error = check_run(run, is_sat, backbone)
        print(f"  {' '.join(extra)}: {run.elapsed:.2f}s {'OK' if error is None else 'FAIL'}", flush=True)
        report.record(f"mpi -np 3 {' '.join(extra)}", error, run)


SUITES = {"bruteforce": suite_bruteforce, "sharing": suite_sharing, "robustness": suite_robustness, "mpi": suite_mpi}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("suites", nargs="*", choices=[*SUITES, "all"], default="all")
    parser.add_argument("--binary", default=os.path.join(ROOT, "backpainless"))
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--quick", action="store_true", help="fewer instances")
    parser.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 4),
                        help="parallel backpainless processes in the bruteforce suite")
    opts = parser.parse_args()

    if not os.access(opts.binary, os.X_OK):
        sys.exit(f"{opts.binary} not found, run make first")
    selected = [s for s in SUITES if s in opts.suites or ("all" in opts.suites and s != "mpi")]

    report = Report()
    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="backpainless-tests-") as workdir:
        for suite in selected:
            SUITES[suite](opts, report, workdir)

    print(f"\n{report.passed} passed, {len(report.failures)} failed in {time.monotonic() - start:.1f}s")
    for failure in report.failures:
        print(f"  {failure}")
    sys.exit(1 if report.failures else 0)


if __name__ == "__main__":
    main()
