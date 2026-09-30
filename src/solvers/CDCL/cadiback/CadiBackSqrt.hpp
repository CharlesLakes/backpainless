#pragma once

#include "solvers/CDCL/cadiback/CadiBackBase.hpp"

#include <limits>

/**
 * @brief Variant: the CadiBack algorithm applied block by block.
 * @ingroup solving
 *
 * The n initial candidates (in the -bb-order order) are split into consecutive blocks of ceil(sqrt(n)) candidates.
 * Blocks are processed one after the other: the constraint only holds candidates of the current block, with the
 * usual chunk policy inside the block (-bb-chunk: 0 = the whole remaining block, 1 = one-by-one, K = geometric
 * growth), and the chunk size is reset when a new block starts. A SAT answer still filters the candidates of every
 * block. With -bb-chunk=0 this interpolates between the one-by-one search and the all-candidates constraint.
 */
class CadiBackSqrt : public CadiBackBase
{
  public:
	CadiBackSqrt(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	const char* variantName() const override { return "CadiBackSqrt"; }

	void printWinningLog() override;

  protected:
	void initCandidates(std::vector<int>& candidates) override;

	void selectChunk(const std::vector<int>& candidates, std::vector<int>& chunk) override;

  private:
	static constexpr unsigned int NO_BLOCK = std::numeric_limits<unsigned int>::max();

	/// @brief Block of each variable (index: variable), set by initCandidates.
	std::vector<unsigned int> m_blockOf;

	/// @brief Block of the last chunk, NO_BLOCK before the first one.
	unsigned int m_currentBlock = NO_BLOCK;

	size_t m_blockSize = 0;
	size_t m_blockCount = 0;
};
