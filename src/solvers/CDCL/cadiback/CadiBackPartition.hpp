#pragma once

#include "solvers/CDCL/cadiback/CadiBackSqrt.hpp"

/**
 * @brief Variant: the d workers split the candidates into one part each and start with their own part.
 * @ingroup solving
 *
 * The n initial candidates (in the -bb-order order) are split into P consecutive parts of ceil(n / P) candidates, P
 * being the number of d workers sharing a candidate board. Worker i processes the parts i, i+1, ..., P-1, 0, ..., i-1
 * (the candidates are rotated so that its part comes first), running CadiBackSqrt with the parts as blocks: the
 * constraint only holds candidates of the current part, with the usual chunk policy inside it, and a SAT answer
 * filters every part. With the board (-bb-share-cand consume), the parts of the other workers are mostly decided when a
 * worker reaches them. Every worker still computes the whole backbone.
 */
class CadiBackPartition : public CadiBackSqrt
{
  public:
	CadiBackPartition(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	const char* variantName() const override { return "CadiBackPartition"; }

	/// @brief This worker owns part @p index out of @p count (set by BackboneSolverFactory::diversification).
	void setPartition(unsigned int index, unsigned int count);

	void printWinningLog() override;

  protected:
	void initCandidates(std::vector<int>& candidates) override;

	size_t blockSize(size_t n) const override;

	unsigned int blockOfPosition(size_t i) const override;

  private:
	unsigned int m_partIndex = 0;
	unsigned int m_partCount = 1;

	/// @brief Part owned by this worker and number of parts, for the candidates of this run (n may be < P).
	unsigned int m_ownPart = 0;
	size_t m_parts = 1;
	size_t m_initialCount = 0;
};
