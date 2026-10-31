#!/bin/bash
# Estudio 6: the 6 best portfolios of estudios 4 and 5 on the 100 instances of the main list, by phases in order of
# priority. Every phase is one call of submit.sh in the same campaign, so collect.sh puts them in one table; a
# configuration used by several questions is run once (c-16, cmxpd-16).
#
#   1  best        cmxpd cmx cmxp mxp mx cmp, 16 workers           (the 6 best of estudios 4 and 5)
#   2  partition   d s c cd, 16 workers                            (d against s, c and cd; cmxpd vs cmxp from phase 1)
#   3  sharing     cmxpd-16 with SHARE = no-clauses, no-backbones, no-free, none (phase 1 is SHARE=all)
#   4  diversity   m x p, 16 workers                               (one type per portfolio; c, d, s in phase 2)
#   5  scaling     phases 1, 2 and 4 with 8 workers
#   6  scaling     phases 1, 2 and 4 with 4 workers
# With 4 workers cmxpd cycles c, m, x, p (no d), with 8 it has a single d (one part): the effect of d is read at 16.
#
# Usage, from the repository root (after build_native.sh --slurm and download_instances.py main --dest instances/main):
#   bash scripts/slurm/studies/estudio6.sh                 # plan of every phase, nothing submitted
#   bash scripts/slurm/studies/estudio6.sh 1 2             # plan of phases 1 and 2
#   bash scripts/slurm/studies/estudio6.sh --submit 1 2    # submits them
#   bash scripts/slurm/collect.sh runs/estudio6            # summary of everything run so far
# The defaults (CAMPAIGN=estudio6, INSTANCES=instances/main, TIMEOUT=3600) and every variable of bench.sbatch can be set
# in the environment. A resubmission with the same campaign only runs what is missing.

set -euo pipefail

export CAMPAIGN=${CAMPAIGN:-estudio6}
export INSTANCES=${INSTANCES:-instances/main}
export TIMEOUT=${TIMEOUT:-3600}

BEST=(cmxpd cmx cmxp mxp mx cmp)
PARTITION_STUDY=(d s c cd)
DIVERSITY=(m x p)
SHARES=(no-clauses no-backbones no-free none)

submit=()
phases=()
for a; do
	case $a in
	--submit) submit=(--submit) ;;
	[1-6]) phases+=("$a") ;;
	-h | --help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "invalid argument '$a': expected --submit or a phase 1-6" >&2; exit 1 ;;
	esac
done
((${#phases[@]})) || phases=(1 2 3 4 5 6)

here=$(dirname "$0")/..
configs() { # <workers> <mixes...>
	local n=$1 m
	shift
	for m; do echo "$m-$n"; done
}

for phase in "${phases[@]}"; do
	case $phase in
	1) echo "== phase 1: the 6 best, 16 workers"; mapfile -t list < <(configs 16 "${BEST[@]}") ;;
	2) echo "== phase 2: partition (d) against s, c and cd, 16 workers"; mapfile -t list < <(configs 16 "${PARTITION_STUDY[@]}") ;;
	3)
		echo "== phase 3: sharing, cmxpd-16 without each part"
		for share in "${SHARES[@]}"; do
			SHARE=$share bash "$here/submit.sh" "${submit[@]}" cmxpd-16
		done
		continue
		;;
	4) echo "== phase 4: one type per portfolio, 16 workers"; mapfile -t list < <(configs 16 "${DIVERSITY[@]}") ;;
	5 | 6)
		n=$((phase == 5 ? 8 : 4))
		echo "== phase $phase: phases 1, 2 and 4 with $n workers"
		mapfile -t list < <(configs $n "${BEST[@]}" "${PARTITION_STUDY[@]}" "${DIVERSITY[@]}")
		;;
	esac
	bash "$here/submit.sh" "${submit[@]}" "${list[@]}"
done
