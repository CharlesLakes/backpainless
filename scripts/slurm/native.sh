#!/bin/bash
# Runs the backpainless binary built by build_native.sh for the processor of this node (one MPI rank of bench.sbatch):
# bin/node/<node>, else bin/<processor>/backpainless of the repository.
# Usage: bash scripts/slurm/native.sh <backpainless arguments>...

bin=$(cd "$(dirname "$0")/../.." && pwd)/bin
node=${SLURMD_NODENAME:-$(hostname -s)}
exe=$bin/node/$node
if [[ ! -x $exe ]]; then
	# same key as build_native.sh: a node of an already built processor that has no link yet
	key=$({ grep -m1 '^model name' /proc/cpuinfo; grep -m1 '^flags' /proc/cpuinfo; } | sha1sum | cut -c1-12)
	exe=$bin/$key/backpainless
fi
if [[ ! -x $exe ]]; then
	echo "c ERROR no binary for $node in $bin: run scripts/slurm/build_native.sh --slurm $node" >&2
	exit 127
fi
echo "c native binary on $node: $(readlink -f "$exe")" >&2
exec "$exe" "$@"
