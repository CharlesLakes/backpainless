BackPainLess: A Framework for Parallel Backbone Computation
============================================================
[TOC]
## Overview
BackPainLess computes the **backbone** of a CNF formula in parallel. The backbone is the set of literals that are true in every model. BackPainLess is a fork of [Painless / D-Painless](https://github.com/lip6/painless), a framework for parallel and distributed SAT solving, and keeps its architecture:
- a portfolio of workers, where each worker runs a complete backbone extraction with its own diversified solver and the first one to finish wins;
- learnt clause sharing between the workers;
- distributed execution over MPI.

The backbone solver implements the CadiBack algorithm on top of CaDiCaL:
- A. Biere, N. Froleyks, W. Wang, [CadiBack: Extracting Backbones with CaDiCaL](https://wenxiwang.github.io/papers/cadiback.pdf), SAT 2023.
- Reference implementation: [arminbiere/cadiback](https://github.com/arminbiere/cadiback).

The following documentation is inherited from Painless and describes the shared architecture:
- the doxygen [Topics](https://lip6.github.io/painless/topics.html);
- [Main Interfaces](./docs/source/DevelopComponents.md);
- the TACAS25 experiments of D-Painless: [TACAS25](./docs/source/TACAS25.md).

## Original Painless authors
* Souheib BAARIR [souheib.baarir@lip6.fr](mailto:souheib.baarir@lip6.fr)
* Mazigh SAOUDI [mazigh.saoudi@epita.fr](mailto:mazigh.saoudi@epita.fr)

> [Contributors](./CONTRIBUTORS.md)

## Project Structure

### Core Components
- `src/containers/`: Data structures for clause management, formula representation and the `BackboneResult` type
- `src/sharing/`: Learnt clause sharing management and strategies
- `src/working/`: Worker organization and portfolio implementation
- `src/solvers/`: Backbone solver interface, factory and implementations (`CDCL/CadiBack`)
- `src/utils/`: Helper utilities and data structures
- `src/preprocessors/`: Phase initialization (GaspiInitializer)
- `src/disabled/`: Code kept for later reintegration but not compiled: PRS and SBVA preprocessing and the PRS portfolio. These preprocessors only preserve satisfiability, so they would change the backbone.

### Backbone Solvers
- `c`, **CadiBack**: the CadiBack algorithm on [CaDiCaL](https://github.com/arminbiere/cadical/tree/24d047563f5f4c9e37a74c04fa30059b2bbc4214) (v1.9.1).

The SAT solvers vendored by Painless are kept in `solvers/`, but they are not linked. They can be built individually (`make kissat`, `make solvers`, ...):
- [Kissat](https://github.com/arminbiere/kissat/tree/71caafb4d182ced9f76cef45b00f37cc598f2a37) (v4.0.2), Kissat-MAB, Kissat-INC
- [MapleCOMSPS](https://maplesat.github.io/solvers.html), [Glucose](https://www.labri.fr/perso/lsimon/glucose/), [MiniSat](http://minisat.se/), [Lingeling](https://github.com/arminbiere/lingeling), [YalSAT](https://github.com/arminbiere/yalsat)

## Build Requirements

### Prerequisites
- C++20 compatible compiler (GCC recommended)
- [Boost Library](https://www.boost.org/) headers
- [OpenMPI](https://www.open-mpi.org/) implementation (needed to build, even when running without MPI)
- Standard build tools (make)
- POSIX-compatible environment

### Build Instructions
1. Clone the repository:
   ```bash
   git clone https://github.com/CharlesLakes/backpainless
   cd backpainless
   ```

2. Build the entire project:
   ```bash
   make # -j for parallel and quicker make
   ```
   This builds CaDiCaL, then both the debug and release versions of BackPainLess.

3. Build specific targets:
   ```bash
   make debug     # Build debug version (-O0 -g3, with assertions)
   make release   # Build release version
   make cadical   # Build only CaDiCaL
   ```

4. Clean the build:
   ```bash
   make cleanpainless   # Clean the BackPainLess build
   make cleansolvers    # Clean solver builds
   make clean           # Clean BackPainLess and solver builds
   ```

### Output Files
The compiled binaries are located in:
- Debug build: `build/debug/backpainless_debug` (symlink `./backpainlessd`)
- Release build: `build/release/backpainless_release` (symlink `./backpainless`)

### Project Organization
```
.
├── build/          # Build output directory
├── docs/           # Documentation (inherited from Painless)
├── examples/       # Example CNF formulas
├── libs/           # External libraries
├── scripts/        # Utility scripts
├── solvers/        # Vendored sequential SAT solvers
└── src/            # BackPainLess source code
```

## Usage

```bash
./backpainless formula.cnf -c=8                  # 8 diversified CadiBack solvers sharing learnt clauses
./backpainless formula.cnf -c=4 -bb-chunk=1      # one-by-one candidate checking
./backpainless -help                             # all options
./backpainless -details=Backbone                 # backbone options in detail
mpirun -np 2 ./backpainless formula.cnf -dist -gshr-strat=1   # distributed mode
```

Output follows the CadiBack format:

| Formula | Output | Exit code |
|---|---|---|
| satisfiable | `s SATISFIABLE`, one `b <lit>` line per backbone literal, then `b 0` | 10 |
| unsatisfiable | `s UNSATISFIABLE` | 20 |
| timeout (`-t`) | `s UNKNOWN` | 0 |

`-no-backbone` prints only the status.

Backbone options:
- `-bb-chunk=<K>`: `0` (default) constrains all remaining candidates at once, `1` checks them one by one, `K > 1` grows the chunk geometrically (`10` is cadiback's `--chunking`).
- `-bb-no-flip`: do not drop flippable literals from the candidates.

### Checking results
For small formulas (up to 22 variables), `scripts/check_backbone.py` enumerates all models and compares the result with the BackPainLess output:
```bash
./backpainless examples/backbone_demo.cnf -c=4 > out.log
python3 scripts/check_backbone.py examples/backbone_demo.cnf out.log
```

## Scripts and Tools

### Launch Script (scripts/launch.sh)
A bash script for running and analyzing experiments:

```bash
./scripts/launch.sh <parameters_file> <input_files_list> [experiment_name] [debug]
```

#### Features:
- Automated execution of multiple instances
- MPI process management and cleanup (MPI is used if the `-gshr-strat` option is not negative)
- Enforces a timeout per instance via the `timeout` command
- Performance metrics collection (SATISFIABLE / UNSATISFIABLE / TIMEOUT counts, PAR2)

#### Parameters:
- `parameters_file`: Configuration file containing solver settings
- `input_files_list`: File containing paths to CNF formulas
- `experiment_name`: (Optional) Name for the experiment
- `debug`: (Optional) Use debug build with verbose MPI output (`--verbose --debug-daemons` outputs in `err_*.txt` file)

#### Output Structure:
```
outputs/
  ├── metric_${solver}_L${lstrat}_G${gstrat}_${timestamp}/
  │   ├── logs/                    # Per-instance logs
  │   │   ├── log_instance1.txt    # Solver output
  │   │   └── err_instance1.txt    # Error messages
  │   └── times_*.csv             # Detailed timing results
  └── times.csv                   # Overall experiments times
```

#### Example Usage:
```bash
# Run in release mode
./scripts/launch.sh scripts/allgather-params.sh formula_list.txt

# Run in debug mode
./scripts/launch.sh scripts/allgather-params.sh formula_list.txt experiment1 debug
```

### Result Analysis Script (scripts/plot.py)

A comprehensive analysis tool for experiment results that can:
- Generate performance statistics and visualizations
- Compare multiple solver configurations
- Create cumulative execution time plots
- Generate scatter plots comparing solver pairs

Usage:
```bash
# Analyze results from metric directories:
python scripts/plot.py --base-dir outputs --timeout 5000

# Use existing CSV file:
python scripts/plot.py --file results.csv --timeout 5000
```

The `--file` and `--base-dir` options are mutually exclusive.

The script supports various options:
- `--scatter-plots`: Generate solver comparison scatter plots
- `--output-dir`: Specify output directory for plots
- `--output-format`: Set plot format (pdf, png, etc.)
- `--dark-mode`: Use dark theme for plots

Output directory structure after a `--base-dir` execution on directory `outputs`:
```
outputs/
  ├── metric_solver1/    # Results for first solver configuration
  │   └── times_*.csv
  ├── metric_solver2/    # Results for second solver configuration
  │   └── times_*.csv
  └── combined_results.csv  # Generated combined statistics
```

The analysis provides:
- Solver performance statistics (solved instances, PAR2 scores, VBS-SMAPE score)
- SATISFIABLE/UNSATISFIABLE instance statistics
- Performance visualization plots
- Virtual best solver (VBS) analysis

## Contributing

- **Language:** commits, pull requests, code and comments are written in English.
- **Commit messages** follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):
  ```
  <type>[optional scope]: <description>

  [optional body]

  [optional footer(s)]
  ```
  Common types are `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `ci` and `chore`. A breaking change is marked with `!` after the type/scope, or with a `BREAKING CHANGE:` footer. For example, `fix(solvers): disable CaDiCaL reimply for flippable checks`.
- **AI coding assistants** follow the rules of the Linux kernel [AI Coding Assistants](https://docs.kernel.org/process/coding-assistants.html) guide:
  - AI agents MUST NOT add `Signed-off-by` tags. Only the human submitter can certify the contribution.
  - The human submitter reviews all AI-generated code and takes full responsibility for it.
  - Contributions made with AI assistance include an `Assisted-by` trailer:
    ```
    Assisted-by: LLM [TOOL1] [TOOL2]
    ```
    `[TOOL1] [TOOL2]` are optional specialized analysis tools (e.g. clang-tidy). Basic development tools (git, gcc, make, editors) are not listed.
  - An AI assistant that finds a bug must also fix it. It must build and verify the fix, and state explicitly what could not be built or tested.

### References

BackPainLess is based on Painless and D-Painless:

```
@InProceedings{10.1007/978-3-319-66263-3_15,
author="Le Frioux, Ludovic
and Baarir, Souheib
and Sopena, Julien
and Kordon, Fabrice",
editor="Gaspers, Serge
and Walsh, Toby",
title="PaInleSS: A Framework for Parallel SAT Solving",
booktitle="Theory and Applications of Satisfiability Testing -- SAT 2017",
year="2017",
publisher="Springer International Publishing",
address="Cham",
pages="233--250",
isbn="978-3-319-66263-3"
}
```
```
@InProceedings{10.1007/978-3-031-90653-4_3,
author="Saoudi, Mazigh
and Baarir, Souheib
and Sopena, Julien
and Lejemble, Thibault",
editor="Gurfinkel, Arie
and Heule, Marijn",
title="D-Painless: A Framework for Distributed Portfolio SAT Solving",
booktitle="Tools and Algorithms for the Construction and Analysis of Systems",
year="2025",
publisher="Springer Nature Switzerland",
address="Cham",
pages="45--64",
isbn="978-3-031-90653-4"
}
```

The backbone algorithm comes from CadiBack:

```
@InProceedings{biere_et_al:LIPIcs.SAT.2023.3,
author="Biere, Armin
and Froleyks, Nils
and Wang, Wenxi",
title="CadiBack: Extracting Backbones with CaDiCaL",
booktitle="26th International Conference on Theory and Applications of Satisfiability Testing (SAT 2023)",
series="Leibniz International Proceedings in Informatics (LIPIcs)",
volume="271",
pages="3:1--3:12",
year="2023",
doi="10.4230/LIPIcs.SAT.2023.3"
}
```
