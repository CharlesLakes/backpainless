#!/bin/bash
# Distributed portfolio benchmark of backpainless on Slurm: ONE job takes all the idle nodes of the partition and runs
# the tasks (instance, portfolio, repetition) one after the other, each task one MPI run over all those nodes (-dist):
# one rank per node running WORKERS solver threads (1 by default: one worker alone on a 4-CPU machine, the other CPUs
# left to the sharing threads and MPI), ranks exchanging learnt clauses and backbone units by global sharing
# (-gshr-strat), and the candidate board of decided variables (-bb-share-cand: backbone and free variables), also
# exchanged across ranks by the global sharing.
#
# Partition solver: only the small nodes node910-927 (4 CPUs, ~15.8 GB) are used, every node taken whole
# (--exclusive); node906-907 (96 CPUs) are excluded so that every run gets the same hardware.
#
# Time limits: -t=TIMEOUT per run (killed 60 s after it if it does not stop); the job's --time is computed from the
# number of runs, TIMEOUT + RUN_SLACK each, + JOB_SLACK, and checked against the time limit of the partition. Inside
# the job, a run starts only if it can finish before the time limit; the others are left for a resubmission.
#
# 1. Edit the configuration below (or set INSTANCES, PORTFOLIOS, REPS, TIMEOUT, GSHR, LAUNCHER in the environment,
#    sbatch passes it on).
# 2. From the repository root, after 'make release' (binary and instances on a file system shared by the nodes):
#        mkdir -p runs
#        bash scripts/slurm/bench.sbatch           # idle nodes, number of runs, time limit, and the sbatch command
#        bash scripts/slurm/bench.sbatch --submit  # submits it: sbatch -N <idle nodes> --time=<computed> ...
# 3. scripts/slurm/collect.sh runs/<job id> <number of runs>   # summary; tasks left (time limit, failure) are run
#    by resubmitting with the same configuration and OUT=runs/<job id>: runs with a row are skipped
#
# Each task writes runs/<job id>/logs/<task>.log and runs/<job id>/rows/<task>.tsv.
#
#SBATCH --job-name=backpainless
#SBATCH --output=runs/slurm-%j.out
#SBATCH --partition=solver
#SBATCH --account=solver
#SBATCH --exclude=node906,node907
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=15G
#SBATCH --exclusive
#SBATCH --time=00:20:00

set -euo pipefail
export LC_ALL=C

# ---------------------------------------------------------------------------------------------------------------------
# Configuration

# module load gcc openmpi

INSTANCES=${INSTANCES:-instances}            # directory with *.cnf / *.cnf.xz / *.cnf.gz / *.cnf.bz2
PORTFOLIOS=${PORTFOLIOS:-diverse8 diverse8+xor diverse9m+xor} # the first one is the baseline of collect.sh
REPS=${REPS:-3}
TIMEOUT=${TIMEOUT:-900}                       # -t of backpainless, seconds
BINARY=${BINARY:-./backpainless}
GSHR=${GSHR:-1}                               # -gshr-strat: 1 AllGather, 2 Mallob, 3 ring
PARTITION=${PARTITION:-solver}                # must match --partition, for the idle nodes and the time limit
EXCLUDE=${EXCLUDE:-node906,node907}           # must match --exclude
NODES=${SLURM_JOB_NUM_NODES:-${NODES:-}}      # in the job: given by sbatch -N; before: all idle nodes
CPUS=${SLURM_CPUS_PER_TASK:-${CPUS:-4}}           # CPUs of a node, all reserved by the job
WORKERS=${WORKERS:-1}                         # -c: solver threads per node
RUN_SLACK=120 # seconds per run above TIMEOUT: kill after -t + 60 s (+10 s), MPI start and stop
JOB_SLACK=300 # seconds per job: sinfo, logs, collect
# Open MPI's mpirun reads the Slurm allocation (one rank per node); --bind-to none, otherwise each rank is bound to one
# core and its 4 workers share it. If the cluster's MPI is set up for srun: LAUNCHER="srun --mpi=pmix"
LAUNCHER=${LAUNCHER:-mpirun --bind-to none -np ${NODES:-1}}

# Per-worker lists, indexed by the global solver id (rank * WORKERS + local id): with one worker per node, node i runs
# worker i % 8 of diverse8; with more than 8 nodes, the 8 configurations are repeated (other seeds). Every list of a
# portfolio has the same length, so that a configuration stays the same when it is repeated.
DIVERSE8="-solver=ccccsssc -bb-chunk=0,0,10,10,0,0,10,1 -bb-order=natural,random,occ,random,occ,natural,occ,occ -bb-share-cand=1"
# diverse8 + a MaxSAT worker (m: rounds of EvalMaxSAT, then the CadiBack loop). On a shared machine it slowed the
# others down (memory, L3); alone on its node it costs nothing to them. 18 nodes = each of the 9 configurations twice
DIVERSE9M="-solver=ccccssscm -bb-chunk=0,0,10,10,0,0,10,1,0 -bb-order=natural,random,occ,random,occ,natural,occ,occ,natural -bb-share-cand=1"
portfolio_args() {
	case $1 in
	diverse8) echo "$DIVERSE8" ;;
	diverse8+xor) echo "$DIVERSE8 -bb-xor-pre=01000001" ;; # XOR exploration on workers 1 and 7 (of each 8)
	diverse9m) echo "$DIVERSE9M" ;;
	diverse9m+xor) echo "$DIVERSE9M -bb-xor-pre=010000010" ;; # XOR exploration on workers 1 and 7 (of each 9)
	diverse8+xor-nopatience) echo "$DIVERSE8 -bb-xor-pre=01000001 -bb-xor-patience=0" ;;
	diverse8+xor-all) echo "$DIVERSE8 -bb-xor-pre=1" ;;
	diverse7+x) echo "-solver=ccccsssx -bb-chunk=0,0,10,10,0,0,10,0 -bb-order=natural,random,occ,random,occ,natural,occ,natural -bb-share-cand=1" ;;
	cadiback) echo "-solver=c -bb-chunk=0" ;; # CadiBack on every worker, only sharing
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

done_task() { [[ -n ${OUT:-} && -f $OUT/rows/$1.tsv ]]; }
pending=0
for ((t = 0; t < total; t++)); do done_task $t || pending=$((pending + 1)); done

# Slurm durations ([D-]HH:MM:SS, MM:SS, ...) <-> seconds
to_secs() {
	awk -v d="$1" 'BEGIN { if (d == "" || d ~ /^(UNLIMITED|INVALID|infinite)$/) { print -1; exit }
		days = 0; if (d ~ /-/) { split(d, a, "-"); days = a[1]; d = a[2] }
		n = split(d, p, ":"); s = 0; for (i = 1; i <= n; i++) s = s * 60 + p[i]
		print days * 86400 + s }'
}
to_hms() { printf "%d:%02d:%02d" $(($1 / 3600)) $(($1 / 60 % 60)) $(($1 % 60)); }

# ---------------------------------------------------------------------------------------------------------------------
# Before the job: all idle nodes and the time limit

if [[ -z ${SLURM_JOB_ID:-} ]]; then
	echo "${#files[@]} instances x ${#ports[@]} portfolios x $REPS reps = $total runs, $pending to do${OUT:+ in $OUT}"
	((pending)) || exit 0
	if [[ -z $NODES ]]; then
		command -v sinfo >/dev/null || { echo "no sinfo: set NODES" >&2; exit 1; }
		NODES=$(sinfo -h -N -p "$PARTITION" -t idle -o '%N %c' |
			awk -v c="$CPUS" -v ex=",$EXCLUDE," '$2 == c && index(ex, "," $1 ",") == 0 && !seen[$1]++' | wc -l)
		((NODES)) || { echo "no idle node with $CPUS CPUs in $PARTITION" >&2; exit 1; }
	fi
	limit=$((pending * (TIMEOUT + RUN_SLACK) + JOB_SLACK))
	echo "$NODES nodes x $CPUS CPUs, $WORKERS worker(s) per node, -t=$TIMEOUT per run: time limit $(to_hms $limit)"
	max=-1
	command -v sinfo >/dev/null && max=$(to_secs "$(sinfo -h -p "$PARTITION" -o '%l' | head -1)")
	if ((max >= 0 && limit > max)); then
		echo "WARNING the partition allows $(to_hms $max): only $(((max - JOB_SLACK) / (TIMEOUT + RUN_SLACK))) runs" \
			"fit, resubmit with OUT=<the job dir> for the rest" >&2
		limit=$max
	fi
	cmd=(sbatch -N "$NODES" --time="$(to_hms $limit)" "$0")
	echo "${OUT:+OUT=$OUT }${cmd[*]}"
	[[ ${1:-} == --submit ]] && "${cmd[@]}"
	exit 0
fi

# ---------------------------------------------------------------------------------------------------------------------
# In the job: the runs one after the other, each over all the nodes of the job

OUT=${OUT:-runs/$SLURM_JOB_ID}
mkdir -p "$OUT/logs" "$OUT/rows"
nodes=$SLURM_JOB_NODELIST
left=$(to_secs "$(squeue -h -j "$SLURM_JOB_ID" -o '%L' 2>/dev/null || true)")
deadline=$((SECONDS + (left >= 0 ? left : 1 << 30)))
echo "job $SLURM_JOB_ID on $nodes ($NODES x $CPUS CPUs, $WORKERS worker(s) per node): $pending runs to do, $left s left, results in $OUT"
# rank 0 parses the instance and broadcasts it: decompressed on the node of this script, where rank 0 runs
TMP_DIR=$(mktemp -d -p "${SLURM_TMPDIR:-${TMPDIR:-/tmp}}")
trap 'rm -rf "$TMP_DIR"' EXIT

for ((task = 0; task < total; task++)); do
	done_task $task && continue
	if ((SECONDS + TIMEOUT + RUN_SLACK > deadline)); then
		echo "time limit: stopping before task $task, resubmit with OUT=$OUT"
		break
	fi
	port=${ports[task % ${#ports[@]}]}
	inst=${files[task / ${#ports[@]} % ${#files[@]}]}
	rep=$((task / (${#ports[@]} * ${#files[@]})))
	read -ra args <<<"$(portfolio_args "$port")"
	echo "task $task: $port rep $rep $inst"

	input=$inst
	case $inst in
	*.xz) xz -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
	*.gz) gzip -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
	*.bz2) bzip2 -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
	esac

	log=$OUT/logs/$task.log
	read -ra launcher <<<"$LAUNCHER"
	cmd=("${launcher[@]}" "$BINARY" "$input" -dist "-gshr-strat=$GSHR" "-c=$WORKERS" "${args[@]}" "-t=$TIMEOUT")
	echo "c task $task: ${cmd[*]}" >"$log"
	start=$(date +%s.%N)
	code=0
	# a run that does not stop by itself after -t is killed 60 s later (exit 124); killing mpirun stops every rank
	timeout -k 10 $((TIMEOUT + 60)) "${cmd[@]}" >>"$log" 2>&1 || code=$?
	end=$(date +%s.%N)

	# Result: task instance portfolio rep exit status solved wall bb_size bb_hash winner timeout nodes
	# (exit: the highest of the ranks; winner: global solver id, rank = winner / WORKERS; only the winning rank prints s/b)

	clean=$(sed 's/\x1b\[[0-9;]*m//g' "$log")
	status=$(grep -m1 '^s ' <<<"$clean" | cut -c3- || true)
	[[ $status == SATISFIABLE || $status == UNSATISFIABLE ]] && solved=1 || solved=0
	bb_size=$(grep -cE '^b -?[1-9]' <<<"$clean" || true)
	bb_hash=$({ grep -E '^b -?[1-9]' <<<"$clean" || true; } | awk '{ print $2 }' | sort -n | sha1sum | cut -c1-12)
	# every rank prints its local winner: keep the one of the winning rank
	rank=$(grep -m1 -oE 'The winner is mpi process [0-9]+' <<<"$clean" | grep -oE '[0-9]+$' || true)
	winner=$(grep -oE "\[mpi:${rank:-x}\] The winner is backbone solver [0-9]+" <<<"$clean" | grep -m1 -oE '[0-9]+$' || echo -)
	wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

	printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$task" "$(basename "$inst")" "$port" "$rep" "$code" \
		"${status:-NONE}" "$solved" "$wall" "$bb_size" "$bb_hash" "$winner" "$TIMEOUT" "$nodes" >"$OUT/rows/.$task.tsv"
	mv "$OUT/rows/.$task.tsv" "$OUT/rows/$task.tsv"
	cat "$OUT/rows/$task.tsv"
done
