# Distributed benchmarks on a Slurm cluster

These scripts run BackPainLess portfolios on a Slurm cluster with MPI (`-dist`). Each **worker** is one MPI rank
running one solver thread (`-c=1`) on its own **4 CPUs** (`--cpus-per-task=4`). The spare CPUs go to the sharing
threads and MPI. The workers exchange learnt clauses, backbone units and the candidate board (backbone and free
variables) through the global sharing strategy, whether or not they run on the same node.

| File | Purpose |
|---|---|
| `submit.sh` | Submits a **campaign**: one job per (experiment, number of workers). |
| `bench.sbatch` | One job: one experiment with N workers, running every (instance, repetition) one after the other. |
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

## 4. Running a campaign

Every command runs **from the repository root** on the login node.

```bash
mkdir -p runs
bash scripts/slurm/submit.sh                  # plan: every job, its runs, time limit and CPUs; nothing submitted
bash scripts/slurm/submit.sh --submit         # submits the jobs
```

The default campaign is 3 experiments × 3 worker counts = 9 jobs:

| Experiment | Worker types (round robin, split as evenly as possible) | Workers |
|---|---|---|
| `cadiback` | CadiBack (`c`) only | 8, 16, 24 |
| `xor` | CadiBack + DiverseBackboneSearch (`c`, `x`), the `x` workers run many XOR rounds | 8, 16, 24 |
| `maxsat` | CadiBack + DiverseBackboneSearch + MaxSAT (`c`, `x`, `m`) | 8, 16, 24 |

With 4 CPUs per worker, 24 workers need 96 CPUs. Slurm places the workers wherever there are free CPUs: one per
4-CPU node, or several on a larger node.

What runs in parallel:

- **Inside a run**, all the workers of the experiment run at the same time and share learnt clauses, backbone units
  and the candidate board through MPI, whether or not they are on the same node. This is the parallel portfolio being
  studied.
- **The jobs of a campaign** are submitted together, and Slurm runs them at the same time as soon as it has the CPUs.
  Two jobs never share anything with each other: each one measures its own configuration (experiment and number of
  workers), so the results can be compared.
- **Inside a job**, the (instance, repetition) runs come one after the other, each run using all the workers.

Jobs running at the same time can share a node and its memory bandwidth, which adds some noise to the times.
`SERIAL=1` runs the jobs one after the other (same job name and `--dependency=singleton`) for cleaner timings.

### Configuration

Set the variables in front of the command. They apply to every job of the campaign:

```bash
REPS=1 TIMEOUT=300 WORKERS_LIST="8 16" EXPERIMENTS="cadiback xor" bash scripts/slurm/submit.sh --submit
```

| Variable | Default | Meaning |
|---|---|---|
| `EXPERIMENTS` | `cadiback xor maxsat` | experiments of the campaign |
| `WORKERS_LIST` | `8 16 24` | numbers of workers (MPI ranks) |
| `CAMPAIGN` | date and time | name of the campaign, results in `runs/<campaign>/` |
| `INSTANCES` | `instances` | directory of the instances |
| `REPS` | 3 | repetitions of each instance |
| `TIMEOUT` | 900 | `-t` of each run, in seconds |
| `PARTITION`, `ACCOUNT` | `solver`, `solver` | Slurm partition and account |
| `CPUS` | 4 | CPUs per worker |
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

### Worker configurations

Inside a type, the configuration changes from one worker to the next. Two option lists are combined so that both
change at every worker, and every pair appears once before any repeats:

- CadiBack: order × chunk, 15 different workers with the defaults.
- `x`: XOR count × density (6 different workers); the order also varies.
- `m`: the order varies.

Beyond that, the configurations repeat with different seeds (the CaDiCaL seed is the worker id). Each job writes its
table of workers to `workers.tsv`.

### A single experiment

`bench.sbatch` runs one cell of the matrix and accepts the same variables:

```bash
EXPERIMENT=xor WORKERS=16 bash scripts/slurm/bench.sbatch            # plan
EXPERIMENT=xor WORKERS=16 bash scripts/slurm/bench.sbatch --submit   # submit
```

## 5. Time limits and resuming

- **Per run:** `-t=TIMEOUT`. A run reaching it prints `s UNKNOWN` and counts as unsolved. A run that does not stop is
  killed 60 s later.
- **Per job:** `--time` = runs × (TIMEOUT + 120 s) + 300 s, capped to the time limit of the partition (with a warning
  that says how many runs fit). This is the worst case; a job ends as soon as its runs are done.
- A run only starts if it can finish before the job's time limit. To run what is missing (time limit, failure,
  cancelled job), submit again with the **same configuration and campaign name**: runs that already have a result
  are skipped.

  ```bash
  CAMPAIGN=<name> bash scripts/slurm/submit.sh --submit
  ```

## 6. Monitoring and results

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
  logs/<task>.log   full output of a run, all ranks (errors included)
  rows/<task>.tsv   one result row per run
```

The task number is `rep × number of instances + instance index`, with the instances in alphabetical order.

### Summary

```bash
bash scripts/slurm/collect.sh runs/<campaign>
```

It writes all the rows to `runs/<campaign>/results.tsv` and prints:

- the mean PAR-2 time and the number of unsolved runs per instance and experiment (an unsolved run counts 2 × TIMEOUT);
- the geometric mean speedup over the first experiment (`cadiback` with the fewest workers);
- the missing runs and the command to run them;
- the runs that crashed (exit code other than 0, 10, 20) and the runs killed after the timeout;
- an **error** if two runs of the same instance give different answers (status or backbone hash).

Columns of `results.tsv`: task, instance, experiment, rep, exit (10 SAT, 20 UNSAT, 0 unknown), status, solved, wall
(s, includes ~2 s of MPI start and stop), bb_size, bb_hash, winner (the id of the winning worker, see `workers.tsv`),
timeout, nodes.

### The backbone

Only the winning rank prints the result, in the CadiBack format (`s SATISFIABLE`, one `b <literal>` line per backbone
literal, `b 0`):

```bash
grep -E '^(s |b )' runs/<campaign>/<experiment>-<N>/logs/<task>.log
```

## Troubleshooting

- **`mpirun` fails to launch on the nodes**: the cluster may require `srun` (`LAUNCHER="srun --mpi=pmix"`, check
  `srun --mpi=list`), or Open MPI may lack Slurm support (`ompi_info | grep -i slurm`).
- **Every run has status `NONE`**: read `logs/<task>.log`, usually a missing library on the compute nodes (`ldd`) or a
  binary outside the shared directory.
- **Job pending with reason `Resources`**: the partition does not have `WORKERS × 4` free CPUs. Use fewer workers or
  wait. `PartitionTimeLimit`: lower `TIMEOUT` or `REPS`.
- **Out of memory**: the job asks for 3900 MB per CPU (`--mem-per-cpu` in `bench.sbatch`), about 15.6 GB per worker.
