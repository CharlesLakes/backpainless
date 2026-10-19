#!/bin/bash
# Submits the configurations you choose, one bench.sbatch job each. A configuration is <experiment>-<workers>, each
# worker with 4 CPUs:
#   cadiback-<N>  only CadiBack workers (c)
#   xor-<N>       CadiBack and DiverseBackboneSearch workers (c, x)
#   maxsat-<N>    CadiBack, DiverseBackboneSearch and MaxSAT workers (c, x, m)
#   <name>-<N>    any other name with MIX=<worker types>, e.g. MIX=ccxm mymix-16
# Results in runs/<campaign>/<configuration>/, summary with collect.sh runs/<campaign>.
#
# Usage, from the repository root:
#   bash scripts/slurm/submit.sh xor-16                      # plan of that job (runs, time limit, CPUs)
#   bash scripts/slurm/submit.sh --submit xor-16             # submits it
#   bash scripts/slurm/submit.sh --submit cadiback-8 xor-8   # several configurations, each its own job
#   CAMPAIGN=<name> bash scripts/slurm/submit.sh --submit xor-16   # same campaign: only the missing runs
#
# Every variable of bench.sbatch (INSTANCES, REPS, TIMEOUT, CB_ORDERS, XOR_ROUNDS, ...) can be set in the environment,
# the same for every configuration of the call. The jobs run at the same time as soon as Slurm has the CPUs; SERIAL=1
# runs the jobs of a campaign one after the other (same job name and --dependency=singleton).

set -euo pipefail

usage() {
	sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

submit=()
configs=()
for a; do
	case $a in
	--submit) submit=(--submit) ;;
	-h | --help) usage ;;
	*-[0-9]*) configs+=("$a") ;;
	*) echo "invalid configuration '$a': expected <experiment>-<workers>, e.g. xor-16" >&2; exit 1 ;;
	esac
done
((${#configs[@]})) || { echo "no configuration given" >&2; usage 1; }

CAMPAIGN=${CAMPAIGN:-$(date +%Y%m%d-%H%M)}
export JOB_NAME=${JOB_NAME:-bpl-$CAMPAIGN}

here=$(dirname "$0")
echo "campaign $CAMPAIGN (results in runs/$CAMPAIGN)"
failed=()
for c in "${configs[@]}"; do
	exp=${c%-*} n=${c##*-}
	EXPERIMENT=$exp WORKERS=$n LABEL=$c OUT=runs/$CAMPAIGN/$c bash "$here/bench.sbatch" "${submit[@]}" || failed+=("$c")
done
((${#submit[@]})) || echo "nothing submitted: add --submit (resume later with CAMPAIGN=$CAMPAIGN)"
echo "summary: bash $here/collect.sh runs/$CAMPAIGN"
((${#failed[@]} == 0)) || { echo "ERROR not submitted: ${failed[*]}" >&2; exit 1; }
