#!/bin/bash
# Builds backpainless optimized for the processor of the cluster nodes (-march=native for backpainless, CaDiCaL and
# CryptoMiniSat), before the benchmarks and apart from them, so that no compilation runs inside a measured job.
#
#   bin/<cpu>/backpainless   one binary per processor type (<cpu>: hash of the model and flags of /proc/cpuinfo)
#   bin/<cpu>/cpu.txt        model, -march resolved by gcc, compiler, node and date of the build
#   bin/node/<node>          symbolic link to the binary of the processor of that node
#
# bench.sbatch runs every rank with the binary of its node (scripts/slurm/native.sh), the generic ./backpainless when
# there is none.
#
# Usage, from the repository root (shared by the login and compute nodes):
#   bash scripts/slurm/build_native.sh --slurm             # every node of PARTITION (one srun per node, one build per
#                                                          # processor type, the other nodes only get their link)
#   bash scripts/slurm/build_native.sh --slurm node910 node911   # these nodes only
#   srun -p solver -A solver -w node910 -c 8 bash scripts/slurm/build_native.sh   # the node of the srun
#   bash scripts/slurm/build_native.sh                     # this machine
#   bash scripts/slurm/build_native.sh --list              # binaries and nodes already built
#
# Options:
#   --force                 rebuild even if the binary of this processor exists
#   --deps <dir>            Boost and zlib installed in <dir> by install_deps.sh (default: ~/opt): sources <dir>/env.sh
#                           when it exists, else exports <dir>/boost_1_86_0 and <dir>/zlib/{include,lib} if present
#                           (CPLUS_INCLUDE_PATH, C_INCLUDE_PATH, LIBRARY_PATH, as in the README)
#   --boost <dir>           Boost headers directory (contains boost/), instead of the one of --deps
#   --zlib <prefix>         zlib prefix (include/, lib/), instead of the one of --deps
#   --env <file>            file sourced before the build (e.g. module load gcc openmpi), after the paths above
#   --no-deps               no export at all: the environment of the shell as it is
# The same through the environment: DEPS, BOOST_DIR, ZLIB_DIR, ENV_FILE. With --slurm: PARTITION, ACCOUNT, EXCLUDE
# (same defaults as bench.sbatch), BUILD_CPUS (8), BUILD_TIME (minutes, 60), IMMEDIATE (seconds to wait for a busy
# node, 120).

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$ROOT/bin
DEPS_SET=${DEPS:+1}
DEPS=${DEPS:-$HOME/opt}
BOOST_DIR=${BOOST_DIR:-}
ZLIB_DIR=${ZLIB_DIR:-}
ENV_FILE=${ENV_FILE:-}
NO_DEPS=0
FORCE=0
MODE=build
PARTITION=${PARTITION:-solver}
ACCOUNT=${ACCOUNT:-solver}
EXCLUDE=${EXCLUDE-node906,node907}
BUILD_CPUS=${BUILD_CPUS:-8}
BUILD_TIME=${BUILD_TIME:-60}
IMMEDIATE=${IMMEDIATE:-120}

usage() {
	sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}
die() { echo "ERROR: $*" >&2; exit 1; }

pass=() # options passed on to the builds of --slurm
nodes=()
while (($#)); do
	case $1 in
	--slurm) MODE=slurm ;;
	--list) MODE=list ;;
	--key) MODE=key ;;
	--force) FORCE=1 pass+=("$1") ;;
	--deps) DEPS=${2:?--deps needs a directory} DEPS_SET=1 pass+=("$1" "$2"); shift ;;
	--boost) BOOST_DIR=${2:?--boost needs a directory} pass+=("$1" "$2"); shift ;;
	--zlib) ZLIB_DIR=${2:?--zlib needs a directory} pass+=("$1" "$2"); shift ;;
	--env) ENV_FILE=${2:?--env needs a file} pass+=("$1" "$2"); shift ;;
	--no-deps) NO_DEPS=1 pass+=("$1") ;;
	-h | --help) usage ;;
	-*) echo "unknown option $1" >&2; usage 1 ;;
	*) nodes+=("$1") ;;
	esac
	shift
done

# Processor type: the model and the instruction set extensions (flags) of the first CPU, the same key as native.sh
cpu_key() {
	{ grep -m1 '^model name' /proc/cpuinfo; grep -m1 '^flags' /proc/cpuinfo; } | sha1sum | cut -c1-12
}
node_name() { echo "${SLURMD_NODENAME:-$(hostname -s)}"; }

# ---------------------------------------------------------------------------------------------------------------------
# --list, --key

if [[ $MODE == key ]]; then
	cpu_key
	exit 0
fi
if [[ $MODE == list ]]; then
	shopt -s nullglob
	for d in "$BIN"/*/cpu.txt; do
		k=$(basename "$(dirname "$d")")
		n=$(for l in "$BIN"/node/*; do [[ $(readlink "$l") == ../$k/backpainless ]] && basename "$l"; done | paste -sd, -)
		printf "%s  %-16s %s\n      nodes: %s\n" "$k" "$(sed -n 's/^march=//p' "$d")" "$(sed -n 's/^model=//p' "$d")" "${n:--}"
	done
	exit 0
fi

# ---------------------------------------------------------------------------------------------------------------------
# --slurm: one srun per node, one after the other (the nodes of an already built processor only get their link)

if [[ $MODE == slurm ]]; then
	command -v srun >/dev/null || die "srun not found: --slurm runs on the login node of a Slurm cluster"
	if ((${#nodes[@]} == 0)); then
		mapfile -t nodes < <(sinfo -h -N -p "$PARTITION" -o '%N %t' |
			awk -v ex=",$EXCLUDE," 'index(ex, "," $1 ",") == 0 && $2 !~ /^(down|drain|drng|fail|maint)/ && !seen[$1]++ { print $1 }')
	fi
	((${#nodes[@]})) || die "no node in partition $PARTITION"
	echo "${#nodes[@]} nodes of $PARTITION: ${nodes[*]}"
	failed=()
	for n in "${nodes[@]}"; do
		echo "== $n"
		srun -p "$PARTITION" -A "$ACCOUNT" -w "$n" -N1 -n1 -c "$BUILD_CPUS" --time="$BUILD_TIME" \
			--immediate="$IMMEDIATE" --job-name=bpl-build bash "$0" "${pass[@]}" || failed+=("$n")
	done
	echo
	bash "$0" --list
	((${#failed[@]} == 0)) || { echo "not built (busy or failed): ${failed[*]}, run again later with these nodes" >&2; exit 1; }
	exit 0
fi

# ---------------------------------------------------------------------------------------------------------------------
# Build for the processor of this machine

key=$(cpu_key)
node=$(node_name)
dest=$BIN/$key
mkdir -p "$BIN/node"
link() { ln -sfn "../$key/backpainless" "$BIN/node/$node"; }

if [[ -x $dest/backpainless && $FORCE == 0 ]]; then
	link
	echo "$node: processor $key already built ($(sed -n 's/^march=//p' "$dest/cpu.txt")), linked bin/node/$node"
	exit 0
fi

# Dependencies (Boost headers, zlib), as in install_deps.sh and the README
prepend() { export "$1=$2${!1:+:${!1}}"; }
if ((!NO_DEPS)); then
	# the default ~/opt may not exist (system Boost and zlib): only a directory given explicitly must exist
	[[ -d $DEPS || -z $DEPS_SET ]] || die "--deps $DEPS does not exist (--no-deps to use the environment)"
	if [[ -z $BOOST_DIR$ZLIB_DIR && -f $DEPS/env.sh ]]; then
		# shellcheck disable=SC1091
		source "$DEPS/env.sh"
	else
		boost=${BOOST_DIR:-$DEPS/boost_1_86_0}
		zlib=${ZLIB_DIR:-$DEPS/zlib}
		[[ -n $BOOST_DIR && ! -d $BOOST_DIR/boost ]] && die "--boost $BOOST_DIR has no boost/ directory"
		[[ -n $ZLIB_DIR && ! -f $ZLIB_DIR/include/zlib.h ]] && die "--zlib $ZLIB_DIR has no include/zlib.h"
		[[ -d $boost/boost ]] && prepend CPLUS_INCLUDE_PATH "$boost"
		if [[ -f $zlib/include/zlib.h ]]; then
			prepend CPLUS_INCLUDE_PATH "$zlib/include"
			prepend C_INCLUDE_PATH "$zlib/include"
			prepend LIBRARY_PATH "$zlib/lib"
		fi
	fi
fi
if [[ -n $ENV_FILE ]]; then
	[[ -f $ENV_FILE ]] || die "--env $ENV_FILE does not exist"
	# shellcheck disable=SC1090
	source "$ENV_FILE"
fi
command -v mpic++ >/dev/null || die "mpic++ not found (load the MPI module with --env)"

# Lock of this processor type (mkdir is atomic, also on NFS): another node of the same type may be building it
lock=$BIN/$key.lock
for ((i = 0; ; i++)); do
	mkdir "$lock" 2>/dev/null && break
	if [[ -n $(find "$lock" -maxdepth 0 -mmin +$((BUILD_TIME + 10)) 2>/dev/null) ]]; then
		echo "removing the stale lock $lock" >&2
		rm -rf "$lock"
		continue
	fi
	((i % 30)) || echo "$node: waiting for the build of $key by $(cat "$lock/node" 2>/dev/null || echo another node)"
	sleep 10
done
echo "$node" >"$lock/node"
TMP=$(mktemp -d -p "${SLURM_TMPDIR:-${TMPDIR:-/tmp}}" bpl-native.XXXXXX)
trap 'rm -rf "$TMP" "$lock"' EXIT
if [[ -x $dest/backpainless && $FORCE == 0 ]]; then
	link
	echo "$node: processor $key built by $(cat "$dest/cpu.txt" | sed -n 's/^node=//p') meanwhile, linked bin/node/$node"
	exit 0
fi

march=$(gcc -march=native -Q --help=target 2>/dev/null | awk '$1 == "-march=" { print $2; exit }')
jobs=$(nproc)
echo "$node: building for $(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//') (-march=native = ${march:-?})," \
	"processor $key, $jobs jobs, in $TMP"

# CaDiCaL: configured in a copy, so that solvers/cadical/makefile and build/ stay those of the generic build
mkdir -p "$TMP/cadical"
cp -r "$ROOT"/solvers/cadical/{configure,makefile.in,VERSION,scripts,src} "$TMP/cadical/"
(cd "$TMP/cadical" && CXXFLAGS=-march=native ./configure >"$TMP/cadical.log" 2>&1 &&
	make -C build -j"$jobs" libcadical.a >>"$TMP/cadical.log" 2>&1) ||
	{ tail -30 "$TMP/cadical.log" >&2; die "CaDiCaL build failed (log above)"; }

# CryptoMiniSat and backpainless, objects in $TMP (the target is the file, not 'release': ./backpainless is untouched)
out=$TMP/bp/release/backpainless_release
make -C "$ROOT" -j"$jobs" ARCH_FLAGS=-march=native BUILD_DIR="$TMP/bp" CMS_BUILD="$TMP/cms" \
	CADICAL_BUILD="$TMP/cadical/build" "$TMP/cms/libcryptominisat5.a" >"$TMP/make.log" 2>&1 &&
	make -C "$ROOT" -j"$jobs" ARCH_FLAGS=-march=native BUILD_DIR="$TMP/bp" CMS_BUILD="$TMP/cms" \
		CADICAL_BUILD="$TMP/cadical/build" "$out" >>"$TMP/make.log" 2>&1 ||
	{ tail -30 "$TMP/make.log" >&2; die "build failed (log above)"; }

# Smoke test before installing it
"$out" "$ROOT/examples/backbone_demo.cnf" -c=1 -t=60 >"$TMP/smoke.log" 2>&1 || true
grep -q '^s SATISFIABLE' "$TMP/smoke.log" || { tail -20 "$TMP/smoke.log" >&2; die "the binary fails on examples/backbone_demo.cnf"; }

mkdir -p "$dest"
cp "$out" "$dest/backpainless.tmp"
mv -f "$dest/backpainless.tmp" "$dest/backpainless"
{
	echo "model=$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
	echo "march=${march:-unknown}"
	echo "compiler=$(g++ --version | head -1)"
	echo "commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
	echo "node=$node"
	echo "date=$(date '+%F %T')"
} >"$dest/cpu.txt"
link
echo "$node: built bin/$key/backpainless (${march:-?}), linked bin/node/$node"
