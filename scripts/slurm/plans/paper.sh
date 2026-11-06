# Plan of the final benchmark of the paper (run with scripts/slurm/campaign.sh): backbone search on the 100 instances
# of the paper list (the 85 SAT instances of 2026 and 15 of 2025 that CaDiCaL solves in at most 1800 s, 25 per time
# range), every worker starting from the precomputed CaDiCaL model of the instance (USE_MODEL=1, -model): the first
# SAT call is skipped, what is measured is the search of the backbone.
#
# Worker types: c CadiBack, s CadiBack block by block (each worker alone), d partition of the variables among the d
# workers (blocks claimed and stolen), m MaxSAT, x XOR on CryptoMiniSat, p XOR on CaDiCaL then CadiBack.
# Phases in order of priority, every parallel one with 4, 8 and 16 workers:
#   sequential  every solver alone with 1 worker: c s m x p (d with one worker is s with a single part)
#   solo        every solver alone: c s d m x p
#   best        the 4 best mixes of estudios 4 and 5: cmxpd cmx cmxp mxp (cmxpd vs cmxp: the partition)
#   sharing     the same 4 with SHARE=none (nothing shared) against best (everything shared)
# With 4 workers cmxpd cycles c, m, x, p, the same as cmxp: cmxpd-4 is left out. With 8 it has a single d.
#
#   python3 scripts/download_instances.py paper --models --dest instances/paper   # instances + models/recent
#   bash scripts/slurm/campaign.sh scripts/slurm/plans/paper.sh --submit
#   bash scripts/slurm/collect.sh runs/paper
# CAMPAIGN, INSTANCES, TIMEOUT (1 hour per instance, the limit of the study), MODELS and USE_MODEL can be changed in
# the environment.

export CAMPAIGN=${CAMPAIGN:-paper}
export INSTANCES=${INSTANCES:-instances/paper}
export TIMEOUT=${TIMEOUT:-3600}
export MODELS=${MODELS:-models/recent}
export USE_MODEL=${USE_MODEL:-1}

SEQUENTIAL=(c s m x p)
SOLO=(c s d m x p)
BEST=(cmxpd cmx cmxp mxp)
BEST_4=(cmx cmxp mxp)

with() { # <workers> <mixes...>: the configurations <mix>-<workers>
	local n=$1 m
	shift
	for m; do printf '%s ' "$m-$n"; done
}

phase sequential -- $(with 1 "${SEQUENTIAL[@]}")
for n in 16 8 4; do
	phase solo -- $(with $n "${SOLO[@]}")
done
phase best -- $(with 16 "${BEST[@]}") $(with 8 "${BEST[@]}") $(with 4 "${BEST_4[@]}")
phase sharing SHARE=none -- $(with 16 "${BEST[@]}") $(with 8 "${BEST[@]}") $(with 4 "${BEST_4[@]}")
