#!/bin/bash
# Runs a campaign described by a plan: a list of named phases, each a set of configurations submitted through
# submit.sh, all in the same campaign, so collect.sh puts them in one table. A configuration listed by several phases
# is submitted once per call, and a resubmission with the same campaign only runs what is missing.
#
# A plan is a bash file (scripts/slurm/plans/*.sh) sourced by this script. It sets its defaults and calls
#   phase <name> [VAR=value[,value...]]... -- <configuration>...
# for every phase, in order of priority. Each VAR=v1,v2 runs the configurations once per value (every combination
# when there are several), with VAR in the environment of submit.sh (SHARE, D_MODE, or any variable of bench.sbatch).
# Several phase lines with the same name form one phase. Example:
#   CAMPAIGN=${CAMPAIGN:-mystudy}
#   BEST=(cmx-16 cmxp-16)
#   phase best -- "${BEST[@]}"
#   phase sharing SHARE=no-clauses,none -- "${BEST[@]}"
#
# Usage, from the repository root:
#   bash scripts/slurm/campaign.sh <plan>                      # plan of every phase, nothing submitted
#   bash scripts/slurm/campaign.sh <plan> best sharing         # plan of these phases
#   bash scripts/slurm/campaign.sh <plan> --submit best        # submits them
#   bash scripts/slurm/campaign.sh <plan> --list               # names of the phases
#   bash scripts/slurm/collect.sh runs/<campaign>              # summary of everything run so far
# Every variable of bench.sbatch can also be set in the environment, for every phase.

set -euo pipefail

usage() {
	sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

(($#)) || usage 1
case $1 in -h | --help) usage ;; esac
plan=$1
shift
[[ -f $plan ]] || { echo "no plan file '$plan'" >&2; exit 1; }

submit=()
list=0
wanted=()
for a; do
	case $a in
	--submit) submit=(--submit) ;;
	--list) list=1 ;;
	-h | --help) usage ;;
	-*) echo "invalid option '$a'" >&2; exit 1 ;;
	*) wanted+=("$a") ;;
	esac
done

here=$(dirname "$0")
names=()   # phases in the order of the plan
seen=" "   # phase VAR=value configuration already submitted by this call
jobs=0
failed=0

selected() { # <name>
	((${#wanted[@]})) || return 0
	local w
	for w in "${wanted[@]}"; do [[ $w == "$1" ]] && return 0; done
	return 1
}

# Submits the configurations once per combination of the values of the remaining variables
expand() { # <assignments...> -- <configurations...>
	if [[ $1 == -- ]]; then
		shift
		local todo=() c key
		for c; do
			key="$vars|$c"
			[[ $seen == *" $key "* ]] && continue
			seen+="$key "
			todo+=("$c")
		done
		((${#todo[@]})) || return 0
		echo "-- ${vars:-(defaults)}: ${todo[*]}"
		jobs=$((jobs + ${#todo[@]}))
		env $vars bash "$here/submit.sh" "${submit[@]}" "${todo[@]}" | grep -v '^nothing submitted\|^summary:' ||
			failed=$((failed + 1))
		return 0
	fi
	local var=${1%%=*} values=${1#*=} value rest=("${@:2}") saved=$vars
	IFS=, read -ra values <<<"$values"
	for value in "${values[@]}"; do
		vars="$saved $var=$value"
		vars=${vars# }
		expand "${rest[@]}"
	done
	vars=$saved
}

phase() { # <name> [VAR=values]... -- <configurations...>
	local name=$1 args=()
	shift
	[[ " ${names[*]} " == *" $name "* ]] || names+=("$name")
	while (($#)) && [[ $1 != -- ]]; do
		[[ $1 =~ ^[A-Z_][A-Z0-9_]*=[^,]+(,[^,]+)*$ ]] || {
			echo "plan $plan, phase $name: invalid '$1', expected VAR=value[,value...] or --" >&2
			exit 1
		}
		args+=("$1")
		shift
	done
	(($#)) || { echo "plan $plan, phase $name: missing -- before the configurations" >&2; exit 1; }
	((list)) && return 0
	selected "$name" || return 0
	vars=
	expand "${args[@]}" "$@"
}

# First pass: only the names of the phases, so a wrong name stops the call before anything is submitted
listing=$list
list=1
# shellcheck source=/dev/null
source "$plan"
if ((listing)); then
	printf '%s\n' "${names[@]}"
	exit 0
fi
for w in "${wanted[@]}"; do
	[[ " ${names[*]} " == *" $w "* ]] || { echo "plan $plan has no phase '$w' (phases: ${names[*]})" >&2; exit 1; }
done
list=0
# shellcheck source=/dev/null
source "$plan"

echo "campaign ${CAMPAIGN:-?}: $jobs jobs (results in runs/${CAMPAIGN:-?})"
((${#submit[@]})) || echo "nothing submitted: add --submit"
echo "summary: bash $here/collect.sh runs/${CAMPAIGN:-?}"
((failed == 0)) || { echo "ERROR: $failed submit.sh calls failed" >&2; exit 1; }
