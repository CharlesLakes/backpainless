# EvalMaxSAT (vendored)

[EvalMaxSAT](https://github.com/FlorentAvellaneda/EvalMaxSAT) by Florent Avellaneda, GPL-3.0 (see `LICENSE`), at commit
`94e677456cab96b2718f6da3f808bf694ac383d6`. Used by MaxSatBackbone (`-solver=m`,
`src/solvers/CDCL/cadiback/MaxSatBackbone.cpp`).

Only the headers of `lib/EvalMaxSAT/src` and `lib/MaLib/src` are kept (`mcqd.cpp` only includes `mcqd.h`, nothing is
compiled separately). The bundled CaDiCaL (a modified sc2021) and Glucose, their interfaces, CLI11 and `main` are not
vendored: `EvalMaxSAT<SAT_SOLVER>` runs on the CaDiCaL 1.9.1 of BackPainLess (`solvers/cadical`, which got the
`find_up_implicants` used by `propagate`), whose `CaDiCaL::` symbols would otherwise clash.

Changes, marked `// Begin BackPainLess` / `// End BackPainLess`:
- `EvalMaxSAT.h`: no default SAT solver (interfaces not vendored), MaLib headers included by relative path, constructor
  taking a SAT solver owned by the caller, no `o <cost>` line on stdout; `adapt_am1_exact` links the graph through an
  index of the soft literals instead of a scan (same links) and skips more than 5000 soft literals instead of 30000
  (the graph takes size^2 bytes).
- `coutUtil.h`: `MonPrint` is silent unless `EVALMAXSAT_VERBOSE` is defined in a debug build (stdout carries the
  backbone).
- `rand.h`: `MonRand` uses one generator per thread (several workers run EvalMaxSAT at the same time).
