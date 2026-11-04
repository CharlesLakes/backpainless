#!/bin/bash
# Precomputes one model per instance with the sequential CaDiCaL (solvers/cadical/build/cadical, no backpainless): a
# Slurm job array, one task per instance (2 CPUs and half the memory of a 4-CPU node by default, so two tasks per
# node, each with room for large instances), each a single CaDiCaL run with a time limit. It also measures the
# CaDiCaL time of the instances whose competition published no runtime (the recent list of download_instances.py).
#
# Output in OUT (default models/<name of INSTANCES>):
#   <hash>.model   the CaDiCaL output of a SAT instance (s and v lines), the format read by backpainless -model
#   results.tsv    one row per run: hash, instance, status (sat, unsat, unknown = time limit, error), seconds, node
# A resubmission only runs the instances without a sat, unsat or unknown row (errors, e.g. out of memory, are retried:
# raise MEM); RETRY_UNKNOWN=1 also runs again the time limits (e.g. with a larger TIMEOUT).
#
# Usage, from the repository root after 'make' (solvers/cadical/build/cadical, on a file system shared by the nodes):
#   python3 scripts/download_instances.py recent --dest instances/recent
#   INSTANCES=instances/recent bash scripts/slurm/models.sbatch             # plan: instances to do, sbatch command
#   INSTANCES=instances/recent bash scripts/slurm/models.sbatch --submit    # submits the array
#   INSTANCES=instances/recent bash scripts/slurm/models.sbatch --summary   # counts and CaDiCaL times so far
#
#SBATCH --job-name=bpl-models
#SBATCH --partition=solver
#SBATCH --account=solver
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2

set -euo pipefail
export LC_ALL=C

INSTANCES=${INSTANCES:-instances/recent}        # directory with *.cnf / *.cnf.xz / *.cnf.gz / *.cnf.bz2
OUT=${OUT:-models/$(basename "$INSTANCES")}
TIMEOUT=${TIMEOUT:-1800}                        # -t of CaDiCaL, seconds (killed 60 s after it)
CADICAL=${CADICAL:-solvers/cadical/build/cadical}
CPUS=${CPUS:-2}                                 # CPUs of each task (CaDiCaL uses one; the CPUs reserve the memory)
MEM=${MEM:-7680M}                               # memory of each task (--mem): 15 GB / 2
MAX_PARALLEL=${MAX_PARALLEL:-}                  # at most this many tasks at the same time (empty: no limit)
EXCLUDE=${EXCLUDE-node906,node907}              # --exclude (empty: none)
PARTITION=${PARTITION:-solver}
ACCOUNT=${ACCOUNT:-solver}
RETRY_UNKNOWN=${RETRY_UNKNOWN:-0}

hash_of() { basename "$1" | cut -d- -f1; }

# ---------------------------------------------------------------------------------------------------------------------
# Before the job: list of the instances to do and sbatch command

if [[ -z ${SLURM_JOB_ID:-} ]]; then
	mode=plan
	case ${1:-} in
	--submit) mode=submit ;;
	--summary) mode=summary ;;
	-h | --help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	"") ;;
	*) echo "invalid argument '$1': expected --submit, --summary or nothing" >&2; exit 1 ;;
	esac

	if [[ $mode == summary ]]; then
		[[ -f $OUT/results.tsv ]] || { echo "no results in $OUT" >&2; exit 1; }
		# last row of each instance
		awk -F'\t' 'NR > 1 { last[$1] = $0 } END { for (h in last) print last[h] }' "$OUT/results.tsv" |
			awk -F'\t' '{ n[$3]++ }
				$3 == "sat" { t = $4 + 0; b = t <= 10 ? "0-10" : t <= 60 ? "10-60" : t <= 300 ? "60-300" : "300+"; s[b]++ }
				END {
					for (k in n) printf "%-8s %d\n", k, n[k]
					printf "sat by CaDiCaL time (s): 0-10 %d, 10-60 %d, 60-300 %d, 300+ %d\n", s["0-10"], s["10-60"], s["60-300"], s["300+"]
				}'
		echo "models: $(find "$OUT" -name '*.model' | wc -l) in $OUT"
		exit 0
	fi

	[[ -x $CADICAL ]] || { echo "no CaDiCaL binary $CADICAL: run 'make' (or make -C solvers/cadical)" >&2; exit 1; }
	[[ -d $INSTANCES ]] || { echo "no directory $INSTANCES" >&2; exit 1; }
	mkdir -p "$OUT"
	done_re='^(sat|unsat|unknown)$'
	((RETRY_UNKNOWN)) && done_re='^(sat|unsat)$'
	declare -A done=()
	if [[ -f $OUT/results.tsv ]]; then
		while IFS=$'\t' read -r h _ status _; do
			[[ $status =~ $done_re ]] && done[$h]=1 || unset "done[$h]"
		done < <(tail -n +2 "$OUT/results.tsv")
	fi
	todo=$OUT/todo.txt
	total=0
	: >"$todo"
	while IFS= read -r f; do
		total=$((total + 1))
		[[ -n ${done[$(hash_of "$f")]:-} ]] || echo "$f" >>"$todo"
	done < <(find "$INSTANCES" -maxdepth 1 \( -name '*.cnf' -o -name '*.cnf.xz' -o -name '*.cnf.gz' -o -name '*.cnf.bz2' \) | sort)
	n=$(wc -l <"$todo")
	echo "$total instances in $INSTANCES, $n to do, results in $OUT (TIMEOUT=$TIMEOUT s, $CPUS CPUs and $MEM per task)"
	((n)) || exit 0

	# the array reads its own copy: a later plan rewrites todo.txt
	list=$todo
	[[ $mode == submit ]] && list=$todo.$(date +%Y%m%d-%H%M%S) && cp "$todo" "$list"
	limit=$(((TIMEOUT + 120 + 59) / 60))
	array="0-$((n - 1))${MAX_PARALLEL:+%$MAX_PARALLEL}"
	cmd=(sbatch --job-name=bpl-models --partition="$PARTITION" --account="$ACCOUNT" --ntasks=1 --cpus-per-task="$CPUS"
		--mem="$MEM" --time="$limit" --array="$array" --output="$OUT/slurm-%A_%a.out")
	[[ -n $EXCLUDE ]] && cmd+=(--exclude="$EXCLUDE")
	cmd+=("$0")
	# the variables go through the environment of sbatch, as in bench.sbatch (no --export: on this cluster it left the
	# tasks held with user_env_retrieval_failed)
	echo "  INSTANCES=$INSTANCES OUT=$OUT TIMEOUT=$TIMEOUT CADICAL=$CADICAL TODO=$list ${cmd[*]}"
	if [[ $mode == submit ]]; then
		INSTANCES=$INSTANCES OUT=$OUT TIMEOUT=$TIMEOUT CADICAL=$CADICAL TODO=$list "${cmd[@]}"
	else
		echo "nothing submitted: add --submit"
	fi
	exit 0
fi

# ---------------------------------------------------------------------------------------------------------------------
# One task of the array: one instance

file=$(sed -n "$((SLURM_ARRAY_TASK_ID + 1))p" "$TODO")
[[ -n $file ]] || { echo "no line $SLURM_ARRAY_TASK_ID in $TODO" >&2; exit 1; }
h=$(hash_of "$file")
tmp=$OUT/$h.model.part
start=$(date +%s.%N)
set +e
timeout -k 10 $((TIMEOUT + 60)) "$CADICAL" -q -t "$TIMEOUT" "$file" >"$tmp"
code=$?
set -e
seconds=$(awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN { print b - a }')
case $code in
10) status=sat ;;
20) status=unsat ;;
0 | 124 | 137) status=unknown ;;
*) status=error ;;
esac
# CaDiCaL stopped by its own limit prints UNKNOWN (c UNKNOWN with -q) and exits 0; no answer at all is an error
[[ $status == unknown && $code == 0 ]] && ! grep -Eq '^[cs] UNKNOWN' "$tmp" && status=error
if [[ $status == sat ]] && grep -Eq '^v( .*)? 0$' "$tmp"; then
	mv "$tmp" "$OUT/$h.model"
else
	[[ $status == sat ]] && status=error # SAT without a complete model
	rm -f "$tmp"
fi
row=$(printf '%s\t%s\t%s\t%.1f\t%s' "$h" "$(basename "$file")" "$status" "$seconds" "$(hostname -s)")
(
	flock 9
	[[ -s $OUT/results.tsv ]] || printf 'hash\tinstance\tstatus\tseconds\tnode\n' >"$OUT/results.tsv"
	echo "$row" >>"$OUT/results.tsv"
) 9>"$OUT/results.lock"
echo "$row (exit $code)"
