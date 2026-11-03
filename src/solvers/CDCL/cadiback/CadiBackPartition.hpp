#pragma once

#include "solvers/CDCL/cadiback/CadiBackSqrt.hpp"

#include <cstdint>

/**
 * @brief Variant: the d workers split the variables into one part each and start with their own part.
 * @ingroup solving
 *
 * Each worker builds its candidates from its own first model, so the candidate lists of two workers differ (only in
 * non-backbone literals: every backbone literal is a candidate of every worker, with the same sign). The parts are
 * therefore defined on the variables, by a rule that every worker computes the same way from the formula alone: the
 * variables are ranked by their number of occurrences (both signs, ties by index; by index when the formula was loaded
 * without its clauses), and variable v goes to part rank(v) mod P, P being the number of d workers sharing a candidate
 * board. The parts are disjoint and identical on every worker, and so is the split of the backbone.
 *
 * Worker i processes the parts i, i+1, ..., P-1, 0, ..., i-1: its candidates are stably sorted by part, counted from
 * its own (inside a part they keep the -bb-order order), then CadiBackSqrt runs with the parts as blocks: the
 * constraint only holds candidates of the current part, with the usual chunk policy inside it, and a SAT answer filters
 * every part. With the board (-bb-share-cand consume), the parts of the other workers are mostly decided when a worker
 * reaches them. Every worker still computes the whole backbone.
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

	unsigned int blockOf(size_t i, int lit) const override;

  private:
	/// @brief Part of @p lit counted from the own part: 0 for the own part, then 1, ..., P-1.
	unsigned int relativePart(int lit) const;

	unsigned int m_partIndex = 0;
	unsigned int m_partCount = 1;

	/// @brief Part of each variable (index: variable), the same on every worker.
	std::vector<unsigned int> m_partOf;

	/// @brief Hash of @ref m_partOf, printed so that the workers can be checked to share one partition.
	uint64_t m_signature = 0;

	/// @brief Initial candidates of this worker in its own part.
	size_t m_ownCandidates = 0;
};
