# Distributed benchmarks on a Slurm cluster

These scripts run BackPainLess portfolios on a Slurm cluster with MPI (`-dist`). Each **worker** is one MPI rank
running one solver thread (`-c=1`) on its own **4 CPUs** (`--cpus-per-task=4`) and **15 GB** of memory. The spare CPUs
go to the sharing threads and MPI. The workers exchange learnt clauses, backbone units and the candidate board (backbone and free
variables) through the global sharing strategy, whether or not they run on the same node.

| File | Purpose |
|---|---|
| `submit.sh` | Submits the configurations you choose (e.g. `maxsat-24`), one job each, grouped in a campaign. |
| `bench.sbatch` | One job: one configuration, running every (instance, repetition) one after the other. |
| `collect.sh` | Summary of a campaign or of some experiment directories. |

The instructions assume what most research clusters provide: a home or project directory **shared by the login and
compute nodes**, no root access, and no particular module system.

## 1. Requirements

On the login node:

- GCC with C++20 (GCC ≥ 10), `make`.
- Open MPI with `mpic++` and `mpirun`, built with Slurm support. Check with `ompi_info | grep -i slurm`.
- Boost **headers** (only header-only parts are used: `lockfree`, `intrusive_ptr`, `compute/detail/lru_cache`).
- zlib headers (`zlib.h`), needed by CryptoMiniSat.

If the cluster uses environment modules, load the compiler and MPI first (e.g. `module load gcc openmpi`), and
uncomment the `module load` line in `bench.sbatch`. That way the jobs use the same MPI as the build.

### Missing Boost or zlib, without root

Install them in your home directory, which is shared with the nodes:

```bash
mkdir -p ~/opt && cd ~/opt

# Boost: headers only, nothing to compile
wget https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
tar xzf boost_1_86_0.tar.gz boost_1_86_0/boost && rm boost_1_86_0.tar.gz

# zlib: only if /usr/include/zlib.h does not exist; static, so the nodes need nothing at run time
wget https://zlib.net/fossils/zlib-1.3.1.tar.gz
tar xzf zlib-1.3.1.tar.gz && cd zlib-1.3.1
./configure --prefix=$HOME/opt/zlib --static && make -j4 && make install
cd ~/opt && rm -rf zlib-1.3.1 zlib-1.3.1.tar.gz
```

If the login node has no internet access, download the archives on your machine and copy them with `scp`.

## 2. Build

Clone the repository **in the shared directory**, so that every compute node sees the binary at the same path:

```bash
cd ~/repo     # any shared directory
git clone git@github.com:CharlesLakes/backpainless.git
cd backpainless

# only if Boost / zlib were installed in ~/opt (these variables must be set in the shell that runs make)
export CPLUS_INCLUDE_PATH=$HOME/opt/boost_1_86_0:$HOME/opt/zlib/include
export C_INCLUDE_PATH=$HOME/opt/zlib/include
export LIBRARY_PATH=$HOME/opt/zlib/lib

make -j4      # CaDiCaL, CryptoMiniSat, then backpainless (./backpainless and ./backpainlessd)
```

Use `make` and not `make release` on a fresh clone: `make` also builds CryptoMiniSat.

### Sanity checks

```bash
ldd ./backpainless | grep "not found"           # must print nothing
srun -p <partition> -A <account> -n1 -c4 ./backpainless examples/backbone_demo.cnf -c=1 | grep "^s "
                                                 # a compute node runs the binary: "s SATISFIABLE"
```

The binary is generic x86-64 (no `-march=native`), so building on the login node is fine even if its CPU differs
from the compute nodes.

## 3. Instances

Put the instances in `instances/` at the repository root (`INSTANCES=<dir>` selects another directory). Accepted:
`*.cnf`, `*.cnf.xz`, `*.cnf.gz`, `*.cnf.bz2`, compressed files are decompressed by the job. Only MPI rank 0 reads the
instance and broadcasts it, so the other nodes never touch the file.

For a first test, a small instance with a non-empty backbone is the most useful: e.g. SATLIB `uf250-06.cnf` (backbone
of 10 literals, a few seconds). `ais12.cnf` is a good speed test, but its backbone is empty (`b 0` only).

## 4. Configurations

A **configuration** is a portfolio of workers, named `<experiment>-<workers>`. The experiment determines the worker
types and the number determines the total number of workers. The types are assigned round robin (`c, x, m, c, x, m,
...`), so the workers are split among the types as evenly as possible:

| Experiment | Worker types | 8 workers | 16 workers | 24 workers |
|---|---|---|---|---|
| `cadiback` | CadiBack (`c`) | 8 c | 16 c | 24 c |
| `xor` | CadiBack, DiverseBackboneSearch (`c`, `x`) | 4 c, 4 x | 8 c, 8 x | 12 c, 12 x |
| `maxsat` | CadiBack, DiverseBackboneSearch, MaxSAT (`c`, `x`, `m`) | 3 c, 3 x, 2 m | 6 c, 5 x, 5 m | 8 c, 8 x, 8 m |
| any other name | the types given by `MIX`, e.g. `MIX=ccxm mymix-16` | | | |

The `x` workers are configured for a long exploration: 1000 XOR rounds, and a round is never abandoned
(`XOR_ROUNDS`, `XOR_PATIENCE`).

### What the workers of a configuration share

A configuration is executed as **one MPI run**: all its workers run at the same time and cooperate, whether or not
they are on the same node.

| Information | `c` (CadiBack) | `x` (XOR) | `m` (MaxSAT) |
|---|---|---|---|
| Learnt clauses: exported | yes | no, by default (`-bb-xor-export=0`) | yes |
| Learnt clauses: imported | yes | yes | yes |
| Backbone literals (unit clauses) | yes | yes | yes |
| Candidate board: backbone and free variables | yes | yes | yes |

The candidate board is what carries the non-backbone information. A worker that finds a model publishes the free
variables it proves, and every other worker drops them from its candidates. With `-dist` the board is merged across
all MPI ranks at every round of the global sharing. The `x` workers mostly contribute through the board, since the
XOR exploration yields models that are very different from each other. They do not export their learnt clauses by
default, because CryptoMiniSat gives no glue and these clauses slowed down the CadiBack workers in earlier
measurements.

## 5. Running a configuration

Every command runs **from the repository root** on the login node. The following example runs `maxsat-24`: 8
CadiBack, 8 XOR and 8 MaxSAT workers, 96 CPUs in total.

1. Inspect the plan. Nothing is submitted; the plan shows the number of runs, the computed time limit and the number
   of CPUs requested:

   ```bash
   mkdir -p runs
   bash scripts/slurm/submit.sh maxsat-24
   ```

2. Submit it. The output gives the name of the campaign (the date and time by default, or `CAMPAIGN`):

   ```bash
   bash scripts/slurm/submit.sh --submit maxsat-24
   ```

3. Follow the job:

   ```bash
   squeue -u $USER
   tail -f runs/<campaign>/maxsat-24/slurm-*.out
   ```

4. Read the results once it has finished (see section 7):

   ```bash
   bash scripts/slurm/collect.sh runs/<campaign>
   cat runs/<campaign>/maxsat-24/workers.tsv                 # type and options of every worker
   grep -E '^(s |b )' runs/<campaign>/maxsat-24/logs/0.log   # backbone found by the first run
   ```

By default every instance of `instances/` is run once, with a timeout of 900 s per run. For a first test, reduce the
timeout:

```bash
TIMEOUT=300 bash scripts/slurm/submit.sh --submit maxsat-24
```

### Several configurations at the same time

Several configurations can be given to one command. Each one becomes a **separate job**, and the jobs run at the same
time as soon as Slurm has the CPUs:

```bash
bash scripts/slurm/submit.sh --submit cadiback-24 xor-24
```

- The configurations are independent: workers of different configurations never share anything, so each job measures
  its own portfolio and the results can be compared.
- Running together requires the CPUs of all of them, e.g. 2 × 96 = 192 CPUs for the command above. If the partition
  does not have them free, Slurm starts what fits and keeps the other jobs pending (reason `Resources`) until CPUs are
  released. Nothing has to be resubmitted.
- Jobs running at the same time may share a node and its memory bandwidth, which adds some noise to the times.
  `SERIAL=1` runs the jobs one after the other (same job name and `--dependency=singleton`) for cleaner timings.
- Inside a job, the runs (instance, repetition) are executed one after the other, each one using all the workers.

### A study

A study compares configurations, e.g. the three experiments with 8, 16 and 24 workers. Use the same `CAMPAIGN` so
that `collect.sh` puts them in one table; the configurations can be submitted at once or in steps:

```bash
export CAMPAIGN=study1
bash scripts/slurm/submit.sh --submit cadiback-8 cadiback-16 cadiback-24
bash scripts/slurm/submit.sh --submit xor-8 xor-16 xor-24
bash scripts/slurm/submit.sh --submit maxsat-8 maxsat-16 maxsat-24
bash scripts/slurm/collect.sh runs/study1
```

With 4 CPUs per worker, 24 workers need 96 CPUs. Slurm places the workers on the nodes that are not excluded
(`EXCLUDE`, `node906,node907` by default) wherever there are free CPUs and memory. On the `solver` partition this
leaves the 4-CPU nodes node910-927: one worker per node, so at most 18 workers in total (18 × 4 = 72 CPUs). A larger
configuration is refused by the plan; `EXCLUDE=` allows the excluded nodes again.

### Options

Set the variables in front of the command. They apply to every configuration of that command, and are recorded in
`config.env` of each one:

```bash
TIMEOUT=300 MEM_PER_WORKER=8G XOR_ROUNDS=200 bash scripts/slurm/submit.sh --submit xor-16
```

| Variable | Default | Meaning |
|---|---|---|
| `CAMPAIGN` | date and time | name of the campaign, results in `runs/<campaign>/<configuration>/` |
| `INSTANCES` | `instances` | directory of the instances |
| `REPS` | 1 | runs of each instance (repetitions use the same seeds and only measure the noise of the parallel run) |
| `TIMEOUT` | 900 | time limit of each run (one instance), in seconds: `-t` of backpainless |
| `PARTITION`, `ACCOUNT` | `solver`, `solver` | Slurm partition and account |
| `CPUS` | 4 | CPUs per worker |
| `MEM_PER_WORKER` | `15G` | memory of each worker, requested as `--mem-per-cpu` = `MEM_PER_WORKER` / `CPUS` (3840M) |
| `EXCLUDE` | `node906,node907` | nodes never used (`--exclude`); `EXCLUDE=` uses every node of the partition |
| `SERIAL` | 0 | 1: the jobs of the campaign run one at a time (cleaner timings, longer) |
| `MIX` | per experiment | worker types cycled over the workers, e.g. `MIX=ccx` (overrides the experiment) |
| `CB_ORDERS` | `natural random occ reverse occ-rev` | candidate orders (`-bb-order`) |
| `CB_CHUNKS` | `0 10 1` | chunk sizes of the CadiBack workers (`-bb-chunk`: 0 all, 1 one by one, K geometric) |
| `XOR_MS` | `10 12 16` | XORs per round of the `x` workers (`-bb-xor-m`) |
| `XOR_DENSITIES` | `0.5 0.3` | density of the XORs (`-bb-xor-density`) |
| `XOR_ROUNDS` | 1000 | XOR rounds before the CadiBack loop (`-bb-xor-rounds`) |
| `XOR_PATIENCE` | 0 | 0: a round is never abandoned (`-bb-xor-patience`) |
| `XOR_CONFL` | `2x` | conflict budget of an XOR node (`-bb-xor-confl`) |
| `MAXSAT_ROUNDS` | 0 | MaxSAT rounds of the `m` workers, 0: until a round drops nothing |
| `GSHR` | 1 | global sharing: 1 AllGather, 2 Mallob, 3 ring |
| `EXTRA_ARGS` | | added to every run, e.g. `-bb-share-cand-local` |
| `LAUNCHER` | `mpirun --bind-to none -np N` | e.g. `srun --mpi=pmix` if the cluster's MPI requires `srun` |
| `STARTUP_TIMEOUT` | 180 | seconds for the MPI ranks to start; after that the run is killed and retried |
| `START_RETRIES` | 2 | retries of a run whose ranks did not start |

### Options of each worker

Inside a type, the configuration changes from one worker to the next. Two option lists are combined so that both
change at every worker, and every pair appears once before any repeats:

- CadiBack: order × chunk, 15 different workers with the defaults.
- `x`: XOR count × density (6 different workers); the order also varies.
- `m`: the order varies.

Beyond that, the configurations repeat with different seeds (the CaDiCaL seed is the worker id). Each job writes its
table of workers to `workers.tsv`.

### Using bench.sbatch directly

`submit.sh` calls `bench.sbatch` once per configuration; it can also be used directly, with the same variables:

```bash
EXPERIMENT=xor WORKERS=16 bash scripts/slurm/bench.sbatch            # plan
EXPERIMENT=xor WORKERS=16 bash scripts/slurm/bench.sbatch --submit   # submit
```

## 6. Time limits and resuming

- **Per run:** `-t=TIMEOUT`. A run reaching it prints `s UNKNOWN` and counts as unsolved. A run that does not stop is
  killed 60 s later.
- **Per job:** `--time` = runs × (TIMEOUT + 120 s) + 300 s, capped to the time limit of the partition (with a warning
  that says how many runs fit). This is the worst case; a job ends as soon as its runs are done.
- A run only starts if it can finish before the job's time limit. To run what is missing (time limit, failure,
  cancelled job), submit the configuration again with the **same variables and campaign name**: runs that already
  have a result are skipped. `collect.sh` prints the exact command.

  ```bash
  CAMPAIGN=<name> bash scripts/slurm/submit.sh --submit xor-16
  ```

## 7. Monitoring and results

```bash
squeue -u $USER -o "%i %j %T %M %l %D %R"     # jobs: state, elapsed, limit, nodes, reason
tail -f runs/<campaign>/<experiment>-<N>/slurm-<jobid>.out
scancel -n bpl-<campaign>                      # cancels every job of a campaign
```

Layout of a campaign:

```
runs/<campaign>/<experiment>-<N>/
  config.env        configuration of the experiment
  workers.tsv       type and options of every worker (the worker id is the MPI rank)
  slurm-<jobid>.out output of the job
  logs/<task>.log   combined output of a run, all ranks (errors included)
  ranks/<task>/1/rank.<N>/stdout   output of rank N alone (mpirun only); the result is read from the winning rank's
                    file, because mpirun can glue a line of one rank to a line of another in the combined log
  rows/<task>.tsv   one result row per run
```

The task number is `rep × number of instances + instance index`, with the instances in alphabetical order.

### Summary

```bash
bash scripts/slurm/collect.sh runs/<campaign>
```

It writes all the rows to `runs/<campaign>/results.tsv` and prints:

- the mean PAR-2 time and the number of unsolved runs per instance and experiment (an unsolved run counts 2 × TIMEOUT);
- the geometric mean speedup over the first configuration (ordered cadiback, xor, maxsat, others, then by number of
  workers);
- the missing runs and the command to run them (only the incomplete configurations);
- the runs that crashed, with the reason in the status column, the runs killed after the timeout, and the runs whose
  MPI ranks never started (see below);
- an **error** if two runs of the same instance give different answers (status or backbone hash).

Status of a run: `SATISFIABLE` / `UNSATISFIABLE`, `UNKNOWN` (reached `TIMEOUT`), or why it stopped otherwise:

| Status | Meaning |
|---|---|
| `SEGFAULT` | a rank crashed (signal 11); mpirun then aborts the whole run |
| `OOM` | a rank was killed (signal 9) and Slurm marked a step `OUT_OF_MEMORY`: it exceeded `MEM_PER_WORKER` |
| `KILLED` | a rank was killed by signal 9 without an OOM record |
| `HANG` | the run did not stop within `TIMEOUT` + 60 s and was killed |
| `NOSTART` | the MPI ranks did not start within `STARTUP_TIMEOUT`, even after `START_RETRIES` retries (exit 125) |

After any run that did not end normally, the job kills the processes of this job left on its nodes (ranks and Open MPI
daemons) before the next run, so that a stuck run does not block the following ones.

Columns of `results.tsv`: task, instance, experiment, rep, exit (10 SAT, 20 UNSAT, 0 unknown), status, solved, wall
(s, includes ~2 s of MPI start and stop), bb_size, bb_hash, winner (the id of the winning worker, see `workers.tsv`),
timeout, nodes.

### The backbone

Only the winning rank prints the result, in the CadiBack format (`s SATISFIABLE`, one `b <literal>` line per backbone
literal, `b 0`):

```bash
grep -lE '^s ' runs/<campaign>/<experiment>-<N>/ranks/<task>/1/rank.*/stdout      # the winning rank
grep -E '^(s |b )' runs/<campaign>/<experiment>-<N>/ranks/<task>/1/rank.<N>/stdout
```

## Troubleshooting

- **`mpirun` fails to launch on the nodes**: the cluster may require `srun` (`LAUNCHER="srun --mpi=pmix"`, check
  `srun --mpi=list`), or Open MPI may lack Slurm support (`ompi_info | grep -i slurm`).
- **Every run has status `NONE`**: read `logs/<task>.log`, usually a missing library on the compute nodes (`ldd`) or a
  binary outside the shared directory.
- **Job pending with reason `Resources`**: the partition does not have `WORKERS × 4` free CPUs. Use fewer workers or
  wait. `PartitionTimeLimit`: lower `TIMEOUT` or `REPS`.
- **Out of memory** (state `OUT_OF_MEMORY`, or exit 137 in a run): raise `MEM_PER_WORKER`. A worker
  must fit on one node: on a 4-CPU node with ~15.8 GB, 15G is close to the maximum, and a larger request keeps the
  job pending.
