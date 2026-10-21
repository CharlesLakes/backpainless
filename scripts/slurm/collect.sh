#!/bin/bash
# Summary of bench.sbatch experiments: mean PAR-2 time (an unsolved run counts twice the timeout) and unsolved runs per
# instance and experiment, geometric mean speedup over the first experiment, the runs that crashed, and an error when
# two runs give a different answer for the same instance. Writes <first dir>/results.tsv (all the rows).
#
# Usage: scripts/slurm/collect.sh runs/<campaign>            # every experiment of a campaign (submit.sh)
#        scripts/slurm/collect.sh runs/<dir> [runs/<dir>...]  # given experiment dirs, the first is the baseline
# A campaign is ordered cadiback (or c, the baseline), xor, maxsat, then the other mixes by number of worker types,
# then by number of workers. The runs missing from a dir (still
# running, time limit, failure) are listed with the command to run them.

set -euo pipefail
export LC_ALL=C

(($#)) || { echo "usage: collect.sh runs/<campaign> | runs/<dir>..." >&2; exit 1; }
campaign=
if (($# == 1)) && [[ ! -d $1/rows && ! -f $1/config.env ]]; then
	campaign=$1
	mapfile -t dirs < <(for d in "$1"/*/; do [[ -d $d/rows || -f $d/config.env ]] && echo "${d%/}"; done |
		awk -F/ '{ n = $NF; e = n; sub(/-[0-9]+$/, "", e); w = n; sub(/.*-/, "", w)
			r = e == "cadiback" || e == "c" ? 0 : e == "xor" ? 1 : e == "maxsat" ? 2 : 2 + length(e)
			printf "%d\t%s\t%08d\t%s\n", r, e, w, $0 }' | sort | cut -f4)
	((${#dirs[@]})) || { echo "no experiment in $1" >&2; exit 1; }
else
	dirs=("$@")
fi
out=${campaign:-${dirs[0]}}/results.tsv

{
	printf "task\tinstance\texperiment\trep\texit\tstatus\tsolved\twall\tbb_size\tbb_hash\twinner\ttimeout\tnodes\n"
	for d in "${dirs[@]}"; do
		[[ -d $d/rows ]] && find "$d/rows" -name '[0-9]*.tsv' -exec cat {} + | sort -n
	done
} >"$out"
echo "$(($(wc -l <"$out") - 1)) runs done, results in $out"

incomplete=()
for d in "${dirs[@]}"; do
	total=
	[[ -f $d/config.env ]] && total=$(sed -n 's/^total=//p' "$d/config.env")
	if [[ -z $total ]]; then
		echo "  $d: no config.env yet (job not started)"
		continue
	fi
	missing=$(for ((i = 0; i < total; i++)); do [[ -f $d/rows/$i.tsv ]] || echo $i; done | awk '
		NR == 1 { s = p = $1; next }
		$1 == p + 1 { p = $1; next }
		{ out = out (out ? "," : "") (s == p ? s : s "-" p); s = p = $1 }
		END { if (NR) print out (out ? "," : "") (s == p ? s : s "-" p) }')
	if [[ -n $missing ]]; then
		echo "  $d: missing runs $missing (still running, time limit, or failed)"
		incomplete+=("$(basename "$d")")
	fi
done
if ((${#incomplete[@]})); then
	if [[ -n $campaign ]]; then
		echo "  to run the missing ones, with the same variables:" \
			"CAMPAIGN=$(basename "$campaign") bash scripts/slurm/submit.sh --submit ${incomplete[*]}"
	else
		echo "  to run the missing ones of a dir: (source <dir>/config.env; OUT=<dir> bash scripts/slurm/bench.sbatch --submit)"
	fi
fi

awk -F'\t' '
	FNR == 1 { next }
	{ if (!($2 in iseen)) { iseen[$2]; insts[++ni] = $2 }
	  if (!($3 in pseen)) { pseen[$3]; ports[++np] = $3 }
	  k = $2 SUBSEP $3; t = $7 ? $8 : 2 * $12
	  sum[k] += t; cnt[k]++; uns[k] += !$7; tot[$3] += t; tcnt[$3]++
	  if ($6 == "HANG") hang = hang " " $3 ":" $1
	  else if ($6 == "NOSTART") nostart = nostart " " $3 ":" $1
	  else if ($5 != 0 && $5 != 10 && $5 != 20) crash = crash " " $3 ":" $1 "(" $6 ", exit " $5 ")"
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
		if (crash) printf "\nWARNING runs that crashed (SEGFAULT, OOM: out of memory, KILLED: other kill; see logs/<task>.log):%s\n", crash
		if (hang) printf "\nWARNING runs killed after -t + 60 s (did not stop by themselves):%s\n", hang
		if (nostart) printf "\nWARNING runs whose MPI ranks never started, even after the retries:%s\n", nostart
		bad = 0
		for (i in nans) if (nans[i] > 1) { if (!bad++) print "\nERROR different answers for the same instance (status:backbone hash):"
			print "  " i ":" anss[i] }
		exit bad > 0 }
' "$out"
