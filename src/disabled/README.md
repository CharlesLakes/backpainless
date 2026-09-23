# Disabled components

This code is kept in the repository but not compiled (the Makefile skips `src/disabled/`). It depended on the removed
SAT `SolverInterface` and its results are not valid for backbones as is.

- `preprocessors/PreprocessorInterface.hpp`, `preprocessors/PRS-Preprocessors/`, `preprocessors/StructuredBva*`:
  PRS and SBVA preprocessing. These techniques only preserve satisfiability, so the backbone of the simplified formula
  is not the backbone of the input. They could be reintegrated with backbone-preserving settings, or by mapping the
  backbone back through the preprocessing (as `restoreModel` does for models).
- `working/PortfolioPRS.*`: the PRS-style distributed portfolio. It depends on the Kissat and MapleCOMSPS wrappers,
  which were removed from `src/solvers`.
