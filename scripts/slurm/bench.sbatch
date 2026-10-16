#!/bin/bash
# One experiment of a distributed benchmark of backpainless on Slurm: a worker mix (EXPERIMENT) run with WORKERS
# workers, each worker one MPI rank (-dist, -c=1) with its own 4 CPUs (--cpus-per-task). The ranks exchange learnt
# clauses, backbone units and the candidate board (backbone and free variables) through the global sharing. The job
# runs the tasks (instance, repetition) one after the other, each task one MPI run over all the workers.
# scripts/slurm/submit.sh submits the whole matrix (experiments x worker counts); this script runs one cell.
#
# Experiments (worker types assigned round robin, so that they are split as evenly as possible):
#   cadiback  only CadiBack workers (c)
#   xor       CadiBack and DiverseBackboneSearch workers (c, x), the x workers explore with many XOR rounds
#   maxsat    CadiBack, DiverseBackboneSearch and MaxSAT workers (c, x, m)
# Inside a type the configurations vary from one worker to the next (see "Worker configurations").
#
# Usage, from the repository root after 'make' (binary and instances on a file system shared by the nodes):
#   EXPERIMENT=xor WORKERS=16 bash scripts/slurm/bench.sbatch           # plan: runs, time limit, sbatch command
#   EXPERIMENT=xor WORKERS=16 bash scripts/slurm/bench.sbatch --submit  # submits it
#   scripts/slurm/collect.sh runs/<experiment dir>...                   # summary
# A resubmission with the same configuration and OUT skips the runs that already have a row.
#
# Time limits: -t=TIMEOUT per run (killed 60 s after it if it does not stop); the job's --time is computed from the
# number of runs, TIMEOUT + RUN_SLACK each, + JOB_SLACK, and capped to the time limit of the partition. A run starts
# only if it can finish before the time limit; the others are left for a resubmission.
#
#SBATCH --job-name=backpainless
#SBATCH --output=runs/slurm-%j.out
#SBATCH --partition=solver
#SBATCH --account=solver
#SBATCH --ntasks=8
#SBATCH --cpus-per-task=4
#SBATCH --mem-per-cpu=3900M
#SBATCH --time=00:20:00

set -euo pipefail
export LC_ALL=C

# ---------------------------------------------------------------------------------------------------------------------
# Configuration (every variable can be set in the environment, sbatch passes it on)

# module load gcc openmpi

EXPERIMENT=${EXPERIMENT:-cadiback}
WORKERS=${SLURM_NTASKS:-${WORKERS:-8}}       # workers = MPI ranks; in the job: given by sbatch --ntasks
CPUS=${SLURM_CPUS_PER_TASK:-${CPUS:-4}}     # CPUs of each worker (one solver thread, the rest for sharing and MPI)
INSTANCES=${INSTANCES:-instances}           # directory with *.cnf / *.cnf.xz / *.cnf.gz / *.cnf.bz2
REPS=${REPS:-3}
TIMEOUT=${TIMEOUT:-900}                     # -t of backpainless, seconds
BINARY=${BINARY:-./backpainless}
GSHR=${GSHR:-1}                             # -gshr-strat: 1 AllGather, 2 Mallob, 3 ring
PARTITION=${PARTITION:-solver}              # must match --partition (time limit of the partition)
ACCOUNT=${ACCOUNT:-solver}                  # must match --account
LABEL=${LABEL:-$EXPERIMENT-$WORKERS}        # name of the experiment in the results
SERIAL=${SERIAL:-1}                         # 1: --dependency=singleton, jobs of the same JOB_NAME run one at a time
JOB_NAME=${JOB_NAME:-backpainless}
EXTRA_ARGS=${EXTRA_ARGS:-}                  # added to every run, e.g. "-bb-share-cand-local"
RUN_SLACK=120 # seconds per run above TIMEOUT: kill after -t + 60 s (+10 s), MPI start and stop
JOB_SLACK=300 # seconds per job: logs, collect
# Open MPI's mpirun reads the Slurm allocation (one rank per task); --bind-to none, otherwise each rank is bound to one
# core. If the cluster's MPI is set up for srun: LAUNCHER="srun --mpi=pmix"
LAUNCHER=${LAUNCHER:-mpirun --bind-to none -np $WORKERS}

# Worker types of each experiment, cycled over the workers (MIX overrides it, e.g. MIX=ccx)
case $EXPERIMENT in
cadiback) MIX=${MIX:-c} ;;
xor) MIX=${MIX:-cx} ;;
maxsat) MIX=${MIX:-cxm} ;;
*) MIX=${MIX:?unknown EXPERIMENT $EXPERIMENT: set MIX (worker types, e.g. cxm)} ;;
esac

# Worker configurations. Two lists A and B are combined so that both change from one worker to the next and every
# pair appears once in |A| * |B| workers: the k-th worker of a type takes A[k % |A|] and B[(k + k / lcm) % |B|]. CadiBack
# (c, s): order x chunk (15 different workers with the defaults); x: m x density (6), and order k % |orders|; m: order.
# Beyond the number of combinations they repeat with other seeds.
CB_ORDERS=${CB_ORDERS:-natural random occ reverse occ-rev}
CB_CHUNKS=${CB_CHUNKS:-0 10 1}               # 0: all candidates, 1: one by one, K: geometric growth
XOR_MS=${XOR_MS:-10 12 16}                   # XORs per round (2^m leaves)
XOR_DENSITIES=${XOR_DENSITIES:-0.5 0.3}      # variables of each XOR: probability per candidate
XOR_ROUNDS=${XOR_ROUNDS:-1000}               # rounds of exploration before the CadiBack loop (default of x: 1)
XOR_PATIENCE=${XOR_PATIENCE:-0}              # 0: a round is never abandoned (default of x: 4x)
XOR_CONFL=${XOR_CONFL:-2x}                   # conflict budget of a node
MAXSAT_ROUNDS=${MAXSAT_ROUNDS:-0}            # 0: until a round drops no candidate

# Per-worker lists (one value per worker, the global solver id is the rank) -> backpainless arguments, and the table
# of the workers (stdout of worker_table)
build_worker_args() {
	local orders chunks ms dens i t k o ch xm xd
	read -ra orders <<<"$CB_ORDERS"
	read -ra chunks <<<"$CB_CHUNKS"
	read -ra ms <<<"$XOR_MS"
	read -ra dens <<<"$XOR_DENSITIES"
	local -A count=()
	pair() { # index in B of the k-th worker for lists of sizes |A| = $2, |B| = $3
		local a=$2 b=$3 g=$2 h=$3 t
		while ((h)); do t=$((g % h)) g=$h h=$t; done
		echo $((($1 + $1 / (a / g * b)) % b))
	}
	W_SOLVER= W_ORDER=() W_CHUNK=() W_XM=() W_XD=()
	for ((i = 0; i < WORKERS; i++)); do
		t=${MIX:i % ${#MIX}:1}
		k=${count[$t]:-0}
		count[$t]=$((k + 1))
		o=${orders[k % ${#orders[@]}]} ch=0 xm=${ms[0]} xd=${dens[0]}
		case $t in
		c | s) ch=${chunks[$(pair $k ${#orders[@]} ${#chunks[@]})]} ;;
		x) xm=${ms[k % ${#ms[@]}]} xd=${dens[$(pair $k ${#ms[@]} ${#dens[@]})]} ;;
		m) ;;
		*) echo "unknown worker type '$t' in MIX=$MIX" >&2; exit 1 ;;
		esac
		W_SOLVER+=$t W_ORDER+=("$o") W_CHUNK+=("$ch") W_XM+=("$xm") W_XD+=("$xd")
	done
	local IFS=,
	WORKER_ARGS=(-dist "-gshr-strat=$GSHR" -c=1 "-solver=$W_SOLVER" "-bb-order=${W_ORDER[*]}"
		"-bb-chunk=${W_CHUNK[*]}" -bb-share-cand=1 "-bb-xor-m=${W_XM[*]}" "-bb-xor-density=${W_XD[*]}"
		"-bb-xor-rounds=$XOR_ROUNDS" "-bb-xor-patience=$XOR_PATIENCE" "-bb-xor-confl=$XOR_CONFL"
		"-bb-maxsat-rounds=$MAXSAT_ROUNDS")
}
worker_table() {
	local i
	printf "worker\ttype\torder\tchunk\txor-m\txor-density\n"
	for ((i = 0; i < WORKERS; i++)); do
		case ${W_SOLVER:i:1} in
		c | s) printf "%d\t%s\t%s\t%s\t-\t-\n" $i "${W_SOLVER:i:1}" "${W_ORDER[i]}" "${W_CHUNK[i]}" ;;
		x) printf "%d\tx\t%s\t-\t%s\t%s\n" $i "${W_ORDER[i]}" "${W_XM[i]}" "${W_XD[i]}" ;;
		*) printf "%d\t%s\t%s\t-\t-\t-\n" $i "${W_SOLVER:i:1}" "${W_ORDER[i]}" ;;
		esac
	done
}

# ---------------------------------------------------------------------------------------------------------------------
# Task -> (rep, instance). rep is the outermost loop: a partial run already covers every instance

mapfile -t files < <(find "$INSTANCES" -type f \( -name '*.cnf' -o -name '*.cnf.xz' -o -name '*.cnf.gz' \
	-o -name '*.cnf.bz2' \) | sort)
((${#files[@]})) || { echo "no instance in $INSTANCES" >&2; exit 1; }
total=$((REPS * ${#files[@]}))
build_worker_args

done_task() { [[ -n ${OUT:-} && -f $OUT/rows/$1.tsv ]]; }
pending=0
for ((t = 0; t < total; t++)); do done_task $t || pending=$((pending + 1)); done

# Slurm durations ([D-]HH:MM:SS, MM:SS, ...) <-> seconds
to_secs() {
	awk -v d="$1" 'BEGIN { if (d == "" || d ~ /^(UNLIMITED|INVALID|infinite)$/) { print -1; exit }
		days = 0; if (d ~ /-/) { split(d, a, "-"); days = a[1]; d = a[2] }
		n = split(d, p, ":"); s = 0; for (i = 1; i <= n; i++) s = s * 60 + p[i]; print days * 86400 + s }'
}
to_hms() { printf "%d:%02d:%02d" $(($1 / 3600)) $(($1 / 60 % 60)) $(($1 % 60)); }

# ---------------------------------------------------------------------------------------------------------------------
# Before the job: time limit and sbatch command

if [[ -z ${SLURM_JOB_ID:-} ]]; then
	OUT=${OUT:-runs/$LABEL}
	echo "$LABEL ($MIX): ${#files[@]} instances x $REPS reps = $total runs, $pending to do in $OUT"
	((pending)) || exit 0
	limit=$((pending * (TIMEOUT + RUN_SLACK) + JOB_SLACK))
	echo "  $WORKERS workers x $CPUS CPUs, -t=$TIMEOUT per run: time limit $(to_hms $limit)"
	if command -v sinfo >/dev/null; then
		max=$(to_secs "$(sinfo -h -p "$PARTITION" -o '%l' | head -1)")
		if ((max >= 0 && limit > max)); then
			echo "  WARNING the partition allows $(to_hms $max): only $(((max - JOB_SLACK) / (TIMEOUT + RUN_SLACK)))" \
				"runs fit, resubmit with the same OUT for the rest" >&2
			limit=$max
		fi
		# CPUs of the partition: allocated/idle/other/total
		read -r _ idle _ all < <(sinfo -h -p "$PARTITION" -o '%C' | tr / ' ')
		((WORKERS * CPUS <= all)) || { echo "  ERROR $((WORKERS * CPUS)) CPUs asked, $all in $PARTITION" >&2; exit 1; }
		echo "  $((WORKERS * CPUS)) CPUs asked, $idle idle of $all in $PARTITION"
	fi
	cmd=(sbatch --job-name="$JOB_NAME" --partition="$PARTITION" --account="$ACCOUNT" --ntasks="$WORKERS"
		--cpus-per-task="$CPUS" --time="$(to_hms $limit)" --output="$OUT/slurm-%j.out")
	((SERIAL)) && cmd+=(--dependency=singleton)
	cmd+=("$0")
	echo "  EXPERIMENT=$EXPERIMENT OUT=$OUT ${cmd[*]}"
	if [[ ${1:-} == --submit ]]; then
		mkdir -p "$OUT"
		OUT=$OUT EXPERIMENT=$EXPERIMENT WORKERS=$WORKERS LABEL=$LABEL "${cmd[@]}"
	fi
	exit 0
fi

# ---------------------------------------------------------------------------------------------------------------------
# In the job: the runs one after the other, each over all the workers of the job

OUT=${OUT:-runs/$SLURM_JOB_ID}
mkdir -p "$OUT/logs" "$OUT/rows"
nodes=$SLURM_JOB_NODELIST
left=$(to_secs "$(squeue -h -j "$SLURM_JOB_ID" -o '%L' 2>/dev/null || true)")
deadline=$((SECONDS + (left >= 0 ? left : 1 << 30)))
worker_table >"$OUT/workers.tsv"
{
	for v in EXPERIMENT LABEL MIX WORKERS CPUS INSTANCES REPS TIMEOUT GSHR BINARY CB_ORDERS CB_CHUNKS XOR_MS \
		XOR_DENSITIES XOR_ROUNDS XOR_PATIENCE XOR_CONFL MAXSAT_ROUNDS EXTRA_ARGS total; do
		printf '%s=%q\n' "$v" "${!v}"
	done
} >"$OUT/config.env"
echo "job $SLURM_JOB_ID on $nodes: $LABEL ($MIX), $WORKERS workers x $CPUS CPUs, $pending runs to do, $left s left"
echo "results in $OUT, workers in $OUT/workers.tsv"
# rank 0 parses the instance and broadcasts it: decompressed on the node of this script, where rank 0 runs
TMP_DIR=$(mktemp -d -p "${SLURM_TMPDIR:-${TMPDIR:-/tmp}}")
trap 'rm -rf "$TMP_DIR"' EXIT
read -ra launcher <<<"$LAUNCHER"
read -ra extra <<<"$EXTRA_ARGS"

for ((task = 0; task < total; task++)); do
	done_task $task && continue
	if ((SECONDS + TIMEOUT + RUN_SLACK > deadline)); then
		echo "time limit: stopping before task $task, resubmit with OUT=$OUT"
		break
	fi
	inst=${files[task % ${#files[@]}]}
	rep=$((task / ${#files[@]}))
	echo "task $task: rep $rep $inst"

	input=$inst
	case $inst in
	*.xz) xz -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
	*.gz) gzip -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
	*.bz2) bzip2 -dc "$inst" >"$TMP_DIR/instance.cnf"; input=$TMP_DIR/instance.cnf ;;
	esac

	log=$OUT/logs/$task.log
	cmd=("${launcher[@]}" "$BINARY" "$input" "${WORKER_ARGS[@]}" "${extra[@]}" "-t=$TIMEOUT")
	echo "c task $task: ${cmd[*]}" >"$log"
	start=$(date +%s.%N)
	code=0
	# a run that does not stop by itself after -t is killed 60 s later (exit 124); killing mpirun stops every rank
	timeout -k 10 $((TIMEOUT + 60)) "${cmd[@]}" >>"$log" 2>&1 || code=$?
	end=$(date +%s.%N)

	# Result: task instance experiment rep exit status solved wall bb_size bb_hash winner timeout nodes
	# (exit: the highest of the ranks; winner: global solver id = rank, see workers.tsv; only the winning rank
	# prints s/b)
	clean=$(sed 's/\x1b\[[0-9;]*m//g' "$log")
	status=$(grep -m1 '^s ' <<<"$clean" | cut -c3- || true)
	[[ $status == SATISFIABLE || $status == UNSATISFIABLE ]] && solved=1 || solved=0
	bb_size=$(grep -cE '^b -?[1-9]' <<<"$clean" || true)
	bb_hash=$({ grep -E '^b -?[1-9]' <<<"$clean" || true; } | awk '{ print $2 }' | sort -n | sha1sum | cut -c1-12)
	# every rank prints its local winner: keep the one of the winning rank
	rank=$(grep -m1 -oE 'The winner is mpi process [0-9]+' <<<"$clean" | grep -oE '[0-9]+$' || true)
	winner=$(grep -oE "\[mpi:${rank:-x}\] The winner is backbone solver [0-9]+" <<<"$clean" | grep -m1 -oE '[0-9]+$' || echo -)
	wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

	printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$task" "$(basename "$inst")" "$LABEL" "$rep" "$code" \
		"${status:-NONE}" "$solved" "$wall" "$bb_size" "$bb_hash" "$winner" "$TIMEOUT" "$nodes" >"$OUT/rows/.$task.tsv"
	mv "$OUT/rows/.$task.tsv" "$OUT/rows/$task.tsv"
	cat "$OUT/rows/$task.tsv"
done
