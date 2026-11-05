# Plan of the final benchmark of the paper (run with scripts/slurm/campaign.sh): the 6 best portfolios of estudios 4
# and 5 on the 100 instances of the paper list (python3 scripts/download_instances.py paper --dest instances/paper:
# the 85 SAT instances of 2026 and 15 of 2025 that CaDiCaL solves in at most 1800 s, 25 per time range), phases in
# order of priority:
#   best        cmxpd cmx cmxp mxp mx cmp, 16 workers
#   partition   d s c cd, 16 workers (cmxpd vs cmxp from best), and d cd with D_MODE=walk (d+walk-16, cd+walk-16):
#               the d workers re-check the parts of the others instead of claiming blocks
#   sharing     the 6 best with SHARE = no-clauses, no-backbones, no-free, none (best is SHARE=all)
#   diversity   m x p, 16 workers (one type per portfolio; c, d, s in partition)
#   scaling-8   best, partition (steal) and diversity with 8 workers
#   scaling-4   the same with 4 workers
# With 4 workers cmxpd cycles c, m, x, p (no d), with 8 it has a single d (one part): the effect of d is read at 16.
#
#   bash scripts/slurm/campaign.sh scripts/slurm/plans/paper.sh --submit best partition
#   bash scripts/slurm/collect.sh runs/paper
# CAMPAIGN, INSTANCES and TIMEOUT can be changed in the environment.

export CAMPAIGN=${CAMPAIGN:-paper}
export INSTANCES=${INSTANCES:-instances/paper}
export TIMEOUT=${TIMEOUT:-3600}

BEST=(cmxpd cmx cmxp mxp mx cmp)
PARTITION=(d s c cd)
DIVERSITY=(m x p)

with() { # <workers> <mixes...>: the configurations <mix>-<workers>
	local n=$1 m
	shift
	for m; do printf '%s ' "$m-$n"; done
}

phase best -- $(with 16 "${BEST[@]}")
phase partition -- $(with 16 "${PARTITION[@]}")
phase partition D_MODE=walk -- d-16 cd-16
phase sharing SHARE=no-clauses,no-backbones,no-free,none -- $(with 16 "${BEST[@]}")
phase diversity -- $(with 16 "${DIVERSITY[@]}")
phase scaling-8 -- $(with 8 "${BEST[@]}" "${PARTITION[@]}" "${DIVERSITY[@]}")
phase scaling-4 -- $(with 4 "${BEST[@]}" "${PARTITION[@]}" "${DIVERSITY[@]}")
