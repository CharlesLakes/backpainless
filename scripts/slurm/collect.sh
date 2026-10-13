#!/bin/bash
# Summary of a bench.sbatch run: mean PAR-2 time (an unsolved run counts twice the timeout) and unsolved runs per
# instance and portfolio, geometric mean speedup over the first portfolio, and an error when two runs give a different
# answer for the same instance. Writes <dir>/results.tsv.
#
# Usage: scripts/slurm/collect.sh runs/<job id> [number of tasks]
#   with the number of tasks, also lists the missing ones and the sbatch command to resubmit them

set -euo pipefail
export LC_ALL=C

dir=${1:?usage: collect.sh runs/<job id> [number of tasks]}
total=${2:-}

{
	printf "task\tinstance\tportfolio\trep\texit\tstatus\tsolved\twall\tbb_size\tbb_hash\twinner\ttimeout\tnode\n"
	find "$dir/rows" -name '[0-9]*.tsv' -exec cat {} + | sort -n
} >"$dir/results.tsv"
echo "$(($(wc -l <"$dir/results.tsv") - 1)) tasks done, results in $dir/results.tsv"

if [[ -n $total ]]; then
	missing=$(for ((i = 0; i < total; i++)); do [[ -f $dir/rows/$i.tsv ]] || echo $i; done | awk '
		NR == 1 { s = p = $1; next }
		$1 == p + 1 { p = $1; next }
		{ out = out (out ? "," : "") (s == p ? s : s "-" p); s = p = $1 }
		END { if (NR) print out (out ? "," : "") (s == p ? s : s "-" p) }')
	if [[ -n $missing ]]; then
		echo "missing tasks: $missing (still queued/running, or failed). To resubmit them:"
		echo "  sbatch --array=$missing --export=ALL,OUT=$dir scripts/slurm/bench.sbatch"
	fi
fi

awk -F'\t' '
	FNR == 1 { next }
	{ if (!($2 in iseen)) { iseen[$2]; insts[++ni] = $2 }
	  if (!($3 in pseen)) { pseen[$3]; ports[++np] = $3 }
	  k = $2 SUBSEP $3; t = $7 ? $8 : 2 * $12
	  sum[k] += t; cnt[k]++; uns[k] += !$7; tot[$3] += t; tcnt[$3]++
	  if ($5 == 124 || $5 == 137) hang = hang " " $1
	  if ($7) { a = $6 ":" $10; if (!(($2, a) in ans)) { ans[$2, a]; nans[$2]++; anss[$2] = anss[$2] " " a } } }
	END {
		w = 14; for (j = 1; j <= np; j++) if (length(ports[j]) > w) w = length(ports[j])
		printf "\n%-34s", "instance (mean PAR-2 s / unsolved)"
		for (j = 1; j <= np; j++) printf " %*s", w, ports[j]; print ""
		for (i = 1; i <= ni; i++) {
			printf "%-34s", substr(insts[i], 1, 34)
			for (j = 1; j <= np; j++) { k = insts[i] SUBSEP ports[j]
				if (cnt[k]) printf " %*.1f / %-3d", w - 6, sum[k] / cnt[k], uns[k]; else printf " %*s", w, "-" }
			print "" }
		printf "%-34s", "TOTAL mean PAR-2"
		for (j = 1; j <= np; j++) printf " %*.1f", w, tot[ports[j]] / tcnt[ports[j]]; print ""
		base = ports[1]
		printf "\ngeometric mean speedup over %s (instances where both have runs):\n", base
		for (j = 2; j <= np; j++) { p = ports[j]; l = m = better = worse = 0
			for (i = 1; i <= ni; i++) { a = insts[i] SUBSEP base; b = insts[i] SUBSEP p
				if (cnt[a] && cnt[b]) { r = (sum[a] / cnt[a]) / (sum[b] / cnt[b]); l += log(r); m++
					better += r > 1.05; worse += r < 1 / 1.05 } }
			if (m) printf "  %-*s x%.3f  (%d faster, %d slower by > 5%%, %d instances)\n", w, p, exp(l / m), better, worse, m }
		if (hang) printf "\nWARNING runs killed after -t + 60 s (did not stop by themselves): tasks%s\n", hang
		bad = 0
		for (i in nans) if (nans[i] > 1) { if (!bad++) print "\nERROR different answers for the same instance (status:backbone hash):"
			print "  " i ":" anss[i] }
		exit bad > 0 }
' "$dir/results.tsv"
