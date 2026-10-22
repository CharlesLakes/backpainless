#!/bin/bash
# One experiment of a distributed benchmark of backpainless on Slurm: a worker mix (EXPERIMENT) run with WORKERS
# workers, each worker one MPI rank (-dist, -c=1) with its own 4 CPUs (--cpus-per-task). The ranks exchange learnt
# clauses, backbone units and the candidate board (backbone and free variables) through the global sharing. The job
# runs the tasks (instance, repetition) one after the other, each task one MPI run over all the workers.
# scripts/slurm/submit.sh submits the configurations you choose (e.g. xor-16), one job each, through this script.
#
# Worker types (letters of MIX, cycled over the workers, so that the types of a mix get the same number of workers,
# within one):
#   c  CadiBack on CaDiCaL
#   m  MaxSatBackbone: rounds of EvalMaxSAT on CaDiCaL, then the CadiBack loop
#   x  DiverseBackboneSearch: XOR exploration on CryptoMiniSat, then the CadiBack loop on CryptoMiniSat
#   p  CadiBack with the XOR exploration first, on CaDiCaL (-bb-xor-pre), then the CadiBack loop on the same solver
# Experiments: a name made of these letters is the MIX itself (e.g. cp: half c, half p; cmxp: the four types), plus
#   cadiback = c, xor = cx, maxsat = cxm (the mixes of the first campaigns)
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
#SBATCH --mem-per-cpu=3840M
#SBATCH --exclude=node906,node907
#SBATCH --time=00:20:00

set -euo pipefail
export LC_ALL=C

# ---------------------------------------------------------------------------------------------------------------------
# Configuration (every variable can be set in the environment, sbatch passes it on)

# module load gcc openmpi

EXPERIMENT=${EXPERIMENT:-cadiback}
WORKERS=${SLURM_NTASKS:-${WORKERS:-8}}       # workers = MPI ranks; in the job: given by sbatch --ntasks
CPUS=${SLURM_CPUS_PER_TASK:-${CPUS:-4}}     # CPUs of each worker (one solver thread, the rest for sharing and MPI)
MEM_PER_WORKER=${MEM_PER_WORKER:-15G}       # memory of each worker, asked as --mem-per-cpu = MEM_PER_WORKER / CPUS
EXCLUDE=${EXCLUDE-node906,node907}          # --exclude: nodes never used (empty: none)
INSTANCES=${INSTANCES:-instances}           # directory with *.cnf / *.cnf.xz / *.cnf.gz / *.cnf.bz2
REPS=${REPS:-1}
TIMEOUT=${TIMEOUT:-900}                     # -t of backpainless, seconds
BINARY=${BINARY:-./backpainless}
GSHR=${GSHR:-1}                             # -gshr-strat: 1 AllGather, 2 Mallob, 3 ring
PARTITION=${PARTITION:-solver}              # must match --partition (time limit of the partition)
ACCOUNT=${ACCOUNT:-solver}                  # must match --account
LABEL=${LABEL:-$EXPERIMENT-$WORKERS}        # name of the experiment in the results
SERIAL=${SERIAL:-0}                         # 1: --dependency=singleton, jobs of the same JOB_NAME run one at a time
JOB_NAME=${JOB_NAME:-backpainless}
EXTRA_ARGS=${EXTRA_ARGS:-}                  # added to every run, e.g. "-bb-share-cand-local"
STARTUP_TIMEOUT=${STARTUP_TIMEOUT:-180}      # seconds for MPI to start rank 0, else the run is killed and retried
START_RETRIES=${START_RETRIES:-2}           # retries of a run whose ranks did not start
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
*[!cmxp]*) MIX=${MIX:?unknown EXPERIMENT $EXPERIMENT: set MIX (worker types c, m, x, p, e.g. cxm)} ;;
*) MIX=${MIX:-$EXPERIMENT} ;;
esac
[[ $MIX =~ ^[cmxps]+$ ]] || { echo "invalid MIX=$MIX: expected worker types c, m, x, p, s" >&2; exit 1; }

# Worker configurations, they vary from one worker of a type to the next. Two lists A and B are combined so that both
# change from one worker to the next and every pair appears once in |A| * |B| workers: the k-th worker of a type takes
# A[k % |A|] and B[(k + k / lcm) % |B|].
#   c, s  order x chunk (15 different workers with the defaults)
#   p     order x chunk, and XORs per round x density (6), counted separately
#   x     XORs per round x density (6), and order k % |orders|
#   m     order
# Beyond the number of combinations they repeat with other seeds (the seed follows the worker id).
CB_ORDERS=${CB_ORDERS:-natural random occ reverse occ-rev}
CB_CHUNKS=${CB_CHUNKS:-0 10 1}               # 0: all candidates, 1: one by one, K: geometric growth
XOR_MS=${XOR_MS:-10 12 16}                   # XORs per round (2^m leaves), x and p
XOR_DENSITIES=${XOR_DENSITIES:-0.5 0.3}      # variables of each XOR: probability per candidate, x and p
# Rounds of exploration before the CadiBack loop (each round draws new XORs). x explores for long; p keeps the default
# of the binary, 1 round, the setting that sped up CadiBack in the local measurements (more rounds did not help)
XOR_ROUNDS=${XOR_ROUNDS:-1000}               # x workers
PRE_ROUNDS=${PRE_ROUNDS:-1}                  # p workers
XOR_CONFL=${XOR_CONFL:-2x}                   # conflict budget of a node, x and p
# Patience: a round is abandoned after that many conflicts without a new free variable ('<f>x' = f times the first
# model, 0 = never), and the exploration ends when an abandoned round found nothing, which hands the rest of the time
# to the CadiBack loop. The same 4x for x and p, so that they differ only by the engine; XOR_PATIENCE=0 gives the
# pure explorer x of estudio1-3 (it never reaches the CadiBack loop).
XOR_PATIENCE=${XOR_PATIENCE:-4x}             # x workers
PRE_PATIENCE=${PRE_PATIENCE:-4x}             # p workers
MAXSAT_ROUNDS=${MAXSAT_ROUNDS:-0}            # 0: until a round drops no candidate

# Per-worker lists (one value per worker, the global solver id is the rank) -> backpainless arguments, and the table
# of the workers (stdout of worker_table)
build_worker_args() {
	local orders chunks ms dens i t k o ch xm xd pre pat
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
	W_TYPE= W_SOLVER= W_PRE= W_ORDER=() W_CHUNK=() W_XM=() W_XD=() W_PAT=() W_ROUNDS=()
	for ((i = 0; i < WORKERS; i++)); do
		t=${MIX:i % ${#MIX}:1}
		k=${count[$t]:-0}
		count[$t]=$((k + 1))
		o=${orders[k % ${#orders[@]}]} ch=0 xm=${ms[0]} xd=${dens[0]} pre=0 pat=$XOR_PATIENCE rounds=$XOR_ROUNDS
		case $t in
		c | s | p) ch=${chunks[$(pair $k ${#orders[@]} ${#chunks[@]})]} ;;
		esac
		case $t in
		x | p) xm=${ms[k % ${#ms[@]}]} xd=${dens[$(pair $k ${#ms[@]} ${#dens[@]})]} ;;
		esac
		[[ $t == p ]] && pre=1 pat=$PRE_PATIENCE rounds=$PRE_ROUNDS
		W_TYPE+=$t W_SOLVER+=${t/p/c} W_PRE+=$pre
		W_ORDER+=("$o") W_CHUNK+=("$ch") W_XM+=("$xm") W_XD+=("$xd") W_PAT+=("$pat") W_ROUNDS+=("$rounds")
	done
	local IFS=,
	WORKER_ARGS=(-dist "-gshr-strat=$GSHR" -c=1 "-solver=$W_SOLVER" "-bb-order=${W_ORDER[*]}"
		"-bb-chunk=${W_CHUNK[*]}" -bb-share-cand=1 "-bb-xor-m=${W_XM[*]}" "-bb-xor-density=${W_XD[*]}"
		"-bb-xor-rounds=${W_ROUNDS[*]}" "-bb-xor-patience=${W_PAT[*]}" "-bb-xor-confl=$XOR_CONFL" "-bb-xor-pre=$W_PRE"
		"-bb-maxsat-rounds=$MAXSAT_ROUNDS")
}
worker_table() {
	local i t
	printf "worker\ttype\torder\tchunk\txor-m\txor-density\txor-patience\txor-rounds\n"
	for ((i = 0; i < WORKERS; i++)); do
		t=${W_TYPE:i:1}
		case $t in
		c | s) printf "%d\t%s\t%s\t%s\t-\t-\t-\t-\n" $i "$t" "${W_ORDER[i]}" "${W_CHUNK[i]}" ;;
		p) printf "%d\tp\t%s\t%s\t%s\t%s\t%s\t%s\n" $i "${W_ORDER[i]}" "${W_CHUNK[i]}" "${W_XM[i]}" "${W_XD[i]}" "${W_PAT[i]}" "${W_ROUNDS[i]}" ;;
		x) printf "%d\tx\t%s\t-\t%s\t%s\t%s\t%s\n" $i "${W_ORDER[i]}" "${W_XM[i]}" "${W_XD[i]}" "${W_PAT[i]}" "${W_ROUNDS[i]}" ;;
		*) printf "%d\t%s\t%s\t-\t-\t-\t-\t-\n" $i "$t" "${W_ORDER[i]}" ;;
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

# MEM_PER_WORKER (<n>[K|M|G|T], Slurm units, M by default) / CPUS -> --mem-per-cpu in MB
mem_per_cpu() {
	awk -v m="$MEM_PER_WORKER" -v c="$CPUS" 'BEGIN {
		u = toupper(substr(m, length(m))); n = m + 0
		f = u == "K" ? 1 / 1024 : u == "G" ? 1024 : u == "T" ? 1024 * 1024 : 1
		if (n <= 0) exit 1; printf "%dM", n * f / c }'
}
MEM_PER_CPU=$(mem_per_cpu) || { echo "invalid MEM_PER_WORKER=$MEM_PER_WORKER" >&2; exit 1; }

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
	echo "  $WORKERS workers x ($CPUS CPUs, $MEM_PER_WORKER = $CPUS x $MEM_PER_CPU), -t=$TIMEOUT per run: time limit" \
		"$(to_hms $limit)${EXCLUDE:+, excluding $EXCLUDE}"
	if command -v sinfo >/dev/null; then
		max=$(to_secs "$(sinfo -h -p "$PARTITION" -o '%l' | head -1)")
		if ((max >= 0 && limit > max)); then
			echo "  WARNING the partition allows $(to_hms $max): only $(((max - JOB_SLACK) / (TIMEOUT + RUN_SLACK)))" \
				"runs fit, resubmit with the same OUT for the rest" >&2
			limit=$max
		fi
		# CPUs of the partition: allocated/idle/other/total
		# CPUs of the nodes that are not excluded: all of them, and those of idle nodes
		read -r all idle < <(sinfo -h -N -p "$PARTITION" -o '%N %c %t' |
			awk -v ex=",$EXCLUDE," 'index(ex, "," $1 ",") == 0 && !seen[$1]++ { a += $2; if ($3 == "idle") i += $2 }
				END { print a + 0, i + 0 }')
		((WORKERS * CPUS <= all)) || { echo "  ERROR $((WORKERS * CPUS)) CPUs asked, $all in $PARTITION" >&2; exit 1; }
		echo "  $((WORKERS * CPUS)) CPUs asked, $idle on idle nodes, $all in $PARTITION"
	fi
	cmd=(sbatch --job-name="$JOB_NAME" --partition="$PARTITION" --account="$ACCOUNT" --ntasks="$WORKERS"
		--cpus-per-task="$CPUS" --mem-per-cpu="$MEM_PER_CPU" --time="$(to_hms $limit)" --output="$OUT/slurm-%j.out")
	[[ -n $EXCLUDE ]] && cmd+=(--exclude="$EXCLUDE")
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
	for v in EXPERIMENT LABEL MIX WORKERS CPUS MEM_PER_WORKER MEM_PER_CPU EXCLUDE INSTANCES REPS TIMEOUT GSHR BINARY CB_ORDERS CB_CHUNKS XOR_MS \
		XOR_DENSITIES XOR_ROUNDS PRE_ROUNDS XOR_PATIENCE PRE_PATIENCE XOR_CONFL MAXSAT_ROUNDS EXTRA_ARGS total; do
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
# process names of the ranks: the name the binary is run by (./backpainless, a symlink) and the file it points to
binary_names="$(basename "$BINARY") $(basename "$(readlink -f "$BINARY")")"

# Kills the processes of this job that a killed run may have left on the nodes (ranks, Open MPI daemons), so that the
# next run can start. Only processes whose environment holds this job's id: other jobs on the same node are untouched.
cleanup_nodes() {
	[[ -n ${SLURM_JOB_ID:-} ]] && command -v srun >/dev/null || return 0
	timeout 60 srun --overlap --nodes="$SLURM_JOB_NUM_NODES" --ntasks-per-node=1 --cpus-per-task=1 bash -c '
		for name in $0 orted; do
			for pid in $(pgrep -u "$USER" -x "${name:0:15}"); do
				tr "\0" "\n" <"/proc/$pid/environ" 2>/dev/null | grep -qx "SLURM_JOB_ID=$1" && kill -KILL "$pid"
			done
		done; true' "$binary_names" "$SLURM_JOB_ID" >/dev/null 2>&1 || true
}

# Number of job steps that Slurm marked OUT_OF_MEMORY so far (an OOM kill of a rank raises it)
oom_steps() { sacct -j "$SLURM_JOB_ID" -n -P -o State 2>/dev/null | grep -c OUT_OF_MEMORY || true; }

# Stops a run: SIGTERM (mpirun forwards it to the ranks), SIGKILL 10 s later
stop_run() {
	kill -TERM "$1" 2>/dev/null || true
	for _ in {1..10}; do kill -0 "$1" 2>/dev/null || break; sleep 1; done
	kill -KILL "$1" 2>/dev/null || true
	wait "$1" 2>/dev/null || true
}

# One attempt of the run: sets code to its exit code, 124 when killed after TIMEOUT + 60 s, or "nostart" when rank 0
# did not print its parameters (it does right after MPI_Init) within STARTUP_TIMEOUT
run_attempt() {
	local off pid t0 started=0
	off=$(stat -c %s "$log")
	rm -rf "$rankdir"
	"${cmd[@]}" >>"$log" 2>&1 &
	pid=$!
	t0=$SECONDS
	code=0
	while kill -0 "$pid" 2>/dev/null; do
		sleep 2
		if ((!started)); then
			if tail -c +$((off + 1)) "$log" | grep -aq "c Parameters:"; then
				started=1
			elif ((SECONDS - t0 > STARTUP_TIMEOUT)); then
				stop_run "$pid"
				code=nostart
				return
			fi
		fi
		if ((SECONDS - t0 > TIMEOUT + 60)); then
			stop_run "$pid"
			code=124
			return
		fi
	done
	wait "$pid" || code=$?
}

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

	# The output of every rank also goes to its own file (Open MPI: ranks/<task>/1/rank.<N>/stdout): mpirun forwards
	# the ranks' output in chunks, so in the combined log a line of one rank can end up glued to a line of another
	log=$OUT/logs/$task.log
	rankdir=$OUT/ranks/$task
	launch=("${launcher[@]}")
	[[ ${launcher[0]##*/} == mpirun ]] && launch+=(--output-filename "$rankdir")
	cmd=("${launch[@]}" "$BINARY" "$input" "${WORKER_ARGS[@]}" "${extra[@]}" "-t=$TIMEOUT")
	echo "c task $task: ${cmd[*]}" >"$log"
	ooms=$(oom_steps)
	for ((attempt = 1; ; attempt++)); do
		start=$(date +%s.%N)
		run_attempt
		end=$(date +%s.%N)
		if [[ $code != 0 && $code != 10 && $code != 20 ]]; then
			# state and peak memory of the job steps (one per node for mpirun): tells an OOM kill from other kills
			command -v sacct >/dev/null && sacct -j "$SLURM_JOB_ID" -P -o JobID,State,ExitCode,MaxRSS,MaxRSSNode \
				2>/dev/null | tail -n 12 | sed 's/^/c sacct: /' >>"$log" || true
			cleanup_nodes
		fi
		[[ $code == nostart ]] || break
		echo "c attempt $attempt: no rank started within $STARTUP_TIMEOUT s, nodes cleaned" >>"$log"
		if ((attempt > START_RETRIES || SECONDS + TIMEOUT + RUN_SLACK > deadline)); then
			code=125
			break
		fi
	done

	# Result: task instance experiment rep exit status solved wall bb_size bb_hash winner timeout nodes
	# (exit: the highest of the ranks; winner: global solver id = rank, see workers.tsv). Only the winning rank prints
	# s/b: read from its own file when there is one, else from the combined log
	src=$log
	if [[ -d $rankdir ]]; then
		f=$(grep -lE '^s (SATISFIABLE|UNSATISFIABLE)' "$rankdir"/*/rank.*/stdout 2>/dev/null | head -1 || true)
		[[ -n $f ]] && src=$f
	fi
	clean=$(sed 's/\x1b\[[0-9;]*m//g' "$src")
	status=$(grep -m1 '^s ' <<<"$clean" | cut -c3- || true)
	[[ $status == SATISFIABLE || $status == UNSATISFIABLE ]] && solved=1 || solved=0
	bb_size=$(grep -cE '^b -?[1-9]' <<<"$clean" || true)
	bb_hash=$({ grep -E '^b -?[1-9]' <<<"$clean" || true; } | awk '{ print $2 }' | sort -n | sha1sum | cut -c1-12)
	# every rank prints its local winner: keep the one of the winning rank
	rank=$(grep -m1 -oE 'The winner is mpi process [0-9]+' <<<"$clean" | grep -oE '[0-9]+$' || true)
	winner=$(grep -oE "\[mpi:${rank:-x}\] The winner is backbone solver [0-9]+" <<<"$clean" | grep -m1 -oE '[0-9]+$' || echo -)
	wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')
	# why an unsolved run stopped, when it is not the timeout of backpainless
	if ((!solved)) && [[ -z $status ]]; then
		sig=$(sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -m1 -oE 'exited on signal [0-9]+' | grep -oE '[0-9]+$' || true)
		case $code:$sig in
		125:*) status=NOSTART ;;
		*:11) status=SEGFAULT ;;
		*:9) (($(oom_steps) > ooms)) && status=OOM || status=KILLED ;;
		124:*) status=HANG ;;
		*) status=NONE ;;
		esac
		echo "c result: exit $code, signal ${sig:-none}, $status" >>"$log"
	fi

	printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$task" "$(basename "$inst")" "$LABEL" "$rep" "$code" \
		"${status:-NONE}" "$solved" "$wall" "$bb_size" "$bb_hash" "$winner" "$TIMEOUT" "$nodes" >"$OUT/rows/.$task.tsv"
	mv "$OUT/rows/.$task.tsv" "$OUT/rows/$task.tsv"
	cat "$OUT/rows/$task.tsv"
done
