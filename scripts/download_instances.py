#!/usr/bin/env python3
"""Download benchmark instances from the Global Benchmark Database (benchmark-database.de).

Usage:
    python3 scripts/download_instances.py                   # the example and main lists, into instances/
    python3 scripts/download_instances.py example           # the 20 instances for quick runs
    python3 scripts/download_instances.py main --dry-run    # print what would be downloaded
    python3 scripts/download_instances.py all --limit 30    # 30 instances that CaDiCaL solves fastest
    python3 scripts/download_instances.py --select          # rebuild main and all from the competition data

The lists live in scripts/benchmarks/ (one instance per line, the GBD hash in the first column):
    example.tsv           (example) 20 instances for quick runs and first tests of a configuration
    sc-cadical-1800.tsv   (all) main track instances of the SAT Competitions 2022-2024 that CaDiCaL solved in
                          at most 1800 s, with a verified result (SAT only by default: an UNSAT formula has no backbone)
    sc-cadical-100.tsv    (main) 100 of them, not in example.tsv, spread over the families, the years and the CaDiCaL
                          times (as many in each of 0-10, 10-60, 60-300 and 300-1800 s): the benchmark set the
                          conclusions are drawn from

The files are saved as <hash>-<name>.cnf.xz, the name given by GBD, and existing files are skipped.
The SAT Competition 2025 published no per-instance results, so the selection uses 2022-2024, the last three
competitions with detailed results. CaDiCaL is the entry of its author in 2023 and 2024; 2022 only has modified
versions and uses the best ranked one (CaDiCaL-watchsat-lto).
"""

import argparse
import csv
import io
import os
import sqlite3
import sys
import tempfile
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LISTS = os.path.join(ROOT, "scripts", "benchmarks")
GBD_FILE = "https://benchmark-database.de/file/{}"
GBD_META = "https://benchmark-database.de/getdatabase/meta"
LIST_FILES = {"example": "example", "main": "sc-cadical-100", "all": "sc-cadical-1800"}
TIME_BUCKETS = [10, 60, 300]

# Per competition: detailed results of the main track (hash x solver -> seconds, 10000 = timeout), the CaDiCaL column,
# the column of the verified result, and a metadata file giving the name and family of each instance (None: the name
# is in the results and the family comes from the GBD metadata).
COMPETITIONS = {
    2024: {
        "results": ("https://satcompetition.github.io/2024/downloads/detailed_results.zip", "detailed_main.csv"),
        "cadical": "CaDiCaL",
        "result": "vresult",
        "meta": ("https://satcompetition.github.io/2024/downloads/meta.csv", None),
    },
    2023: {
        "results": ("https://satcompetition.github.io/2023/downloads/sc2023-detailed-results.zip",
                    "results_main_detailed.csv"),
        "cadical": "CaDiCaL_vivinst",
        "result": "vresult",
        "meta": ("https://satcompetition.github.io/2023/downloads/sc2023-detailed-results.zip", "bench_meta.csv"),
    },
    2022: {
        "results": ("https://satcompetition.github.io/2022/downloads/sc2022-detailed-results.zip", "main-seq.csv"),
        "cadical": "CaDiCaL-watchsat-lto",
        "result": "verified-result",
        "meta": None,
    },
}

SELECT_COLUMNS = ["hash", "file", "year", "family", "result", "cadical_seconds"]


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "backpainless-download"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def read_csv(url, member, cache):
    if url not in cache:
        cache[url] = fetch(url)
    data = cache[url]
    if member is not None:
        data = zipfile.ZipFile(io.BytesIO(data)).read(member)
    text = data.decode()
    first = text.split("\n", 1)[0]
    # the 2024 metadata is separated by spaces
    if "," not in first and "\t" not in first:
        text = "\n".join(",".join(line.split()) for line in text.splitlines())
    return list(csv.DictReader(io.StringIO(text), dialect="excel-tab" if "\t" in first else "excel"))


def gbd_families():
    with tempfile.NamedTemporaryFile(suffix=".db") as f:
        f.write(fetch(GBD_META))
        f.flush()
        with sqlite3.connect(f.name) as db:
            return dict(db.execute("SELECT hash, family FROM features"))


def select(max_time, results, years):
    cache = {}
    chosen = {}
    families = None
    for year in sorted(years, reverse=True):
        comp = COMPETITIONS[year]
        if comp["meta"] is None:
            families = families or gbd_families()
        rows = read_csv(*comp["results"], cache)
        if comp["meta"] is not None:
            meta = {r["hash"]: r for r in read_csv(*comp["meta"], cache)}
        else:
            meta = {r["hash"]: {"filename": r["benchmark"], "family": families.get(r["hash"], "")} for r in rows}
        for row in rows:
            res = row[comp["result"]]
            time = row[comp["cadical"]]
            if res not in results or not time or float(time) > max_time:
                continue
            h = row["hash"]
            # an instance used in several competitions keeps the most recent time
            if h in chosen:
                continue
            m = meta.get(h, {})
            name = m.get("filename", "")
            chosen[h] = {"hash": h, "file": f"{h}-{name}" if name else "", "year": year,
                         "family": m.get("family", ""), "result": res, "cadical_seconds": f"{float(time):.1f}"}
    rows = sorted(chosen.values(), key=lambda r: float(r["cadical_seconds"]))
    print(f"{len(rows)} instances (CaDiCaL <= {max_time:g} s, {'/'.join(sorted(results))}, "
          f"{', '.join(map(str, sorted(years)))})", file=sys.stderr)
    return rows


def bucket(row):
    return sum(float(row["cadical_seconds"]) > b for b in TIME_BUCKETS)


def spread(rows, count, excluded):
    """Greedy pick of count rows: the time bucket with the fewest picks, then the least picked family and year (the
    hash, which is random, breaks ties), so the list is deterministic."""
    left = [r for r in rows if r["hash"] not in excluded]
    picked = []
    per_bucket, per_family, per_year = {}, {}, {}
    while left and len(picked) < count:
        open_buckets = {bucket(r) for r in left}
        b = min(open_buckets, key=lambda b: (per_bucket.get(b, 0), b))
        r = min((r for r in left if bucket(r) == b),
                key=lambda r: (per_family.get(r["family"], 0), per_year.get(r["year"], 0), r["hash"]))
        left.remove(r)
        picked.append(r)
        per_bucket[b] = per_bucket.get(b, 0) + 1
        per_family[r["family"]] = per_family.get(r["family"], 0) + 1
        per_year[r["year"]] = per_year.get(r["year"], 0) + 1
    return sorted(picked, key=lambda r: float(r["cadical_seconds"]))


def write_list(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, SELECT_COLUMNS, delimiter="\t", lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def read_list(name):
    path = os.path.join(LISTS, f"{LIST_FILES.get(name, name)}.tsv")
    with open(path) as f:
        return list(csv.DictReader(f, delimiter="\t"))


def download(row, dest, dry_run):
    h = row["hash"]
    existing = [f for f in os.listdir(dest) if f.startswith(h) and not f.endswith(".part")]
    if existing:
        return f"skip {existing[0]}"
    if dry_run:
        return f"would download {row.get('file') or h}"
    req = urllib.request.Request(GBD_FILE.format(h), headers={"User-Agent": "backpainless-download"})
    with urllib.request.urlopen(req, timeout=600) as r:
        disposition = r.headers.get("Content-Disposition", "")
        name = disposition.split("filename=", 1)[1].strip('"; ') if "filename=" in disposition else row.get("file")
        name = os.path.basename(name or f"{h}.cnf.xz")
        tmp = os.path.join(dest, name + ".part")
        try:
            with open(tmp, "wb") as out:
                while chunk := r.read(1 << 20):
                    out.write(chunk)
        except BaseException:
            os.remove(tmp)
            raise
    os.replace(tmp, os.path.join(dest, name))
    return f"ok   {name}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lists", nargs="*", choices=sorted(LIST_FILES),
                        help="lists to download (default: example and main)")
    parser.add_argument("--dest", default=os.path.join(ROOT, "instances"), help="directory (default: instances/)")
    parser.add_argument("--limit", type=int, help="only the N instances of main or all that CaDiCaL solves fastest")
    parser.add_argument("--max-time", type=float, help="only instances that CaDiCaL solves within this time (s)")
    parser.add_argument("--dry-run", action="store_true", help="print the instances without downloading them")
    parser.add_argument("-j", "--jobs", type=int, default=4, help="parallel downloads (default: 4)")
    parser.add_argument("--select", action="store_true",
                        help="rebuild the main and all lists of scripts/benchmarks/ from the competition results and exit")
    parser.add_argument("--select-count", type=int, default=100, help="size of the main list (default: 100)")
    parser.add_argument("--select-time", type=float, default=1800, help="time limit of --select (default: 1800 s)")
    parser.add_argument("--select-unsat", action="store_true", help="--select also keeps UNSAT instances")
    parser.add_argument("--select-years", type=int, nargs="+", default=sorted(COMPETITIONS),
                        choices=sorted(COMPETITIONS), help="competitions used by --select")
    opts = parser.parse_args()

    if opts.select:
        rows = select(opts.select_time, {"sat", "unsat"} if opts.select_unsat else {"sat"}, opts.select_years)
        picked = spread(rows, opts.select_count, {r["hash"] for r in read_list("example")})
        for name, part in [("all", rows), ("main", picked)]:
            path = os.path.join(LISTS, LIST_FILES[name] + ".tsv")
            write_list(path, part)
            print(f"written {os.path.relpath(path, ROOT)} ({len(part)} instances)", file=sys.stderr)
        return 0

    rows = []
    for name in dict.fromkeys(opts.lists or ["example", "main"]):
        part = read_list(LIST_FILES[name])
        if name != "example":
            if opts.max_time is not None:
                part = [r for r in part if float(r["cadical_seconds"]) <= opts.max_time]
            part = part[:opts.limit] if opts.limit is not None else part
        rows += part
    rows = list({r["hash"]: r for r in rows}.values())

    os.makedirs(opts.dest, exist_ok=True)
    failed = 0
    with ThreadPoolExecutor(max(1, opts.jobs)) as pool:
        futures = [(r, pool.submit(download, r, opts.dest, opts.dry_run)) for r in rows]
        for i, (r, fut) in enumerate(futures, 1):
            try:
                msg = fut.result()
            except Exception as e:
                failed += 1
                msg = f"FAIL {r['hash']}: {e}"
            print(f"[{i}/{len(rows)}] {msg}", flush=True)
    print(f"{len(rows) - failed}/{len(rows)} instances in {opts.dest}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
