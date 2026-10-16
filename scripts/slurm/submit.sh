#!/bin/bash
# Submits a benchmark campaign: one bench.sbatch job per (experiment, worker count), each worker with 4 CPUs.
# Results in runs/<campaign>/<experiment>-<workers>/, summary with collect.sh runs/<campaign>.
#
# Usage, from the repository root:
#   bash scripts/slurm/submit.sh            # plan of every job (runs, time limit, CPUs), nothing submitted
#   bash scripts/slurm/submit.sh --submit   # submits them
#   CAMPAIGN=<name> bash scripts/slurm/submit.sh --submit   # resumes a campaign: only the missing runs
#
# Configuration (environment): EXPERIMENTS, WORKERS_LIST, CAMPAIGN, plus every variable of bench.sbatch (INSTANCES,
# REPS, TIMEOUT, CB_ORDERS, XOR_ROUNDS, ...), the same for every job. With SERIAL=1 (default) the jobs share a job
# name and --dependency=singleton: they run one after the other, so that they never compete for nodes or memory
# bandwidth; SERIAL=0 lets Slurm run them together.

set -euo pipefail

EXPERIMENTS=${EXPERIMENTS:-cadiback xor maxsat}
WORKERS_LIST=${WORKERS_LIST:-8 16 24}
CAMPAIGN=${CAMPAIGN:-$(date +%Y%m%d-%H%M)}
export JOB_NAME=${JOB_NAME:-bpl-$CAMPAIGN}

here=$(dirname "$0")
echo "campaign $CAMPAIGN: experiments '$EXPERIMENTS' x workers '$WORKERS_LIST' (results in runs/$CAMPAIGN)"
for exp in $EXPERIMENTS; do
	for n in $WORKERS_LIST; do
		EXPERIMENT=$exp WORKERS=$n LABEL=$exp-$n OUT=runs/$CAMPAIGN/$exp-$n bash "$here/bench.sbatch" "$@"
	done
done
[[ ${1:-} == --submit ]] || echo "nothing submitted: add --submit (resume later with CAMPAIGN=$CAMPAIGN)"
echo "summary: bash $here/collect.sh runs/$CAMPAIGN"
