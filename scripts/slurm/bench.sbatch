#!/bin/bash
# Portfolio benchmark of backpainless on Slurm: one array task per (instance, portfolio, repetition).
#
# 1. Edit the #SBATCH lines and the configuration below (or override INSTANCES, PORTFOLIOS, REPS, TIMEOUT with
#    --export). --time must be above TIMEOUT.
# 2. From the repository root, after 'make release':
#        mkdir -p runs
#        bash scripts/slurm/bench.sbatch           # prints the number of tasks and the sbatch command to run
#        sbatch --array=0-79 scripts/slurm/bench.sbatch
# 3. scripts/slurm/collect.sh runs/<job id>         # summary, and the command to resubmit the missing tasks
#
# Each task writes runs/<job id>/logs/<task>.log and runs/<job id>/rows/<task>.tsv.
#
#SBATCH --job-name=backpainless
#SBATCH --output=runs/slurm-%A_%a.out
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --hint=nomultithread
#SBATCH --mem=16G
#SBATCH --time=00:20:00
##SBATCH --partition=compute
##SBATCH --account=myaccount
##SBATCH --constraint=feature
##SBATCH --exclusive

set -euo pipefail
export LC_ALL=C

# ---------------------------------------------------------------------------------------------------------------------
# Configuration

# module load gcc openmpi

INSTANCES=${INSTANCES:-instances}            # directory with *.cnf / *.cnf.xz / *.cnf.gz / *.cnf.bz2
PORTFOLIOS=${PORTFOLIOS:-diverse8 diverse8+xor} # the first one is the baseline of collect.sh
REPS=${REPS:-3}
TIMEOUT=${TIMEOUT:-900}                       # -t of backpainless, seconds
BINARY=${BINARY:-./backpainless}

DIVERSE8="-c=8 -solver=ccccsssc -bb-chunk=0,0,10,10,0,0,10,1 -bb-order=natural,random,occ,random,occ,natural,occ,occ -bb-share-cand=1"
portfolio_args() {
	case $1 in
	diverse8) echo "$DIVERSE8" ;;
	diverse8+xor) echo "$DIVERSE8 -bb-xor-pre=01000001" ;; # XOR exploration on workers 1 and 7
	diverse8+xor-nopatience) echo "$DIVERSE8 -bb-xor-pre=01000001 -bb-xor-patience=0" ;;
	diverse8+xor-all) echo "$DIVERSE8 -bb-xor-pre=1" ;;
	diverse7+x) echo "-c=8 -solver=ccccsssx -bb-chunk=0,0,10,10,0,0,10,0 -bb-order=natural,random,occ,random,occ,natural,occ,natural -bb-share-cand=1" ;;
	cadiback) echo "-c=1 -solver=c -bb-chunk=0" ;;
	*) echo "unknown portfolio $1" >&2; exit 1 ;;
	esac
}

# ---------------------------------------------------------------------------------------------------------------------
# Task -> (rep, instance, portfolio). rep is the outermost loop: a partial run already covers every pair

read -ra ports <<<"$PORTFOLIOS"
mapfile -t files < <(find "$INSTANCES" -type f \( -name '*.cnf' -o -name '*.cnf.xz' -o -name '*.cnf.gz' \
	-o -name '*.cnf.bz2' \) | sort)
((${#files[@]})) || { echo "no instance in $INSTANCES" >&2; exit 1; }
for p in "${ports[@]}"; do portfolio_args "$p" >/dev/null; done
total=$((REPS * ${#files[@]} * ${#ports[@]}))

if [[ -z ${SLURM_ARRAY_TASK_ID:-} ]]; then
	echo "${#files[@]} instances x ${#ports[@]} portfolios x $REPS reps = $total tasks"
	echo "sbatch --array=0-$((total - 1)) $0"
	exit 0
fi

task=$SLURM_ARRAY_TASK_ID
((task < total)) || { echo "task $task >= $total" >&2; exit 1; }
port=${ports[task % ${#ports[@]}]}
inst=${files[task / ${#ports[@]} % ${#files[@]}]}
rep=$((task / (${#ports[@]} * ${#files[@]})))
read -ra args <<<"$(portfolio_args "$port")"

OUT=${OUT:-runs/$SLURM_ARRAY_JOB_ID}
mkdir -p "$OUT/logs" "$OUT/rows"
echo "task $task on $(hostname), cpus $(taskset -pc $$ | cut -d: -f2): $port rep $rep $inst"

# ---------------------------------------------------------------------------------------------------------------------
# Run

TMP_DIR=$(mktemp -d -p "${SLURM_TMPDIR:-${TMPDIR:-/tmp}}")
trap 'rm -rf "$TMP_DIR"' EXIT
input=$inst
case $inst in
*.xz) xz -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
*.gz) gzip -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
*.bz2) bzip2 -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
esac

log=$OUT/logs/$task.log
cmd=("$BINARY" "$input" "${args[@]}" "-t=$TIMEOUT")
echo "c task $task: ${cmd[*]}" >"$log"
start=$(date +%s.%N)
code=0
# a run that does not stop by itself after -t is killed 60 s later (exit 124)
timeout -k 10 $((TIMEOUT + 60)) "${cmd[@]}" >>"$log" 2>&1 || code=$?
end=$(date +%s.%N)

# ---------------------------------------------------------------------------------------------------------------------
# Result: task instance portfolio rep exit status solved wall bb_size bb_hash winner timeout node

clean=$(sed 's/\x1b\[[0-9;]*m//g' "$log")
status=$(grep -m1 '^s ' <<<"$clean" | cut -c3- || true)
[[ $status == SATISFIABLE || $status == UNSATISFIABLE ]] && solved=1 || solved=0
bb_size=$(grep -cE '^b -?[1-9]' <<<"$clean" || true)
bb_hash=$({ grep -E '^b -?[1-9]' <<<"$clean" || true; } | awk '{ print $2 }' | sort -n | sha1sum | cut -c1-12)
winner=$(grep -oE 'The winner is backbone solver [0-9]+' <<<"$clean" | grep -oE '[0-9]+$' || echo -)
wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$task" "$(basename "$inst")" "$port" "$rep" "$code" \
	"${status:-NONE}" "$solved" "$wall" "$bb_size" "$bb_hash" "$winner" "$TIMEOUT" "$(hostname)" >"$OUT/rows/.$task.tsv"
mv "$OUT/rows/.$task.tsv" "$OUT/rows/$task.tsv"
cat "$OUT/rows/$task.tsv"
