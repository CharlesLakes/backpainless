#include "solvers/CDCL/cadiback/CadiBackSqrt.hpp"

#include "utils/Logger.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>

CadiBackSqrt::CadiBackSqrt(int id, const std::shared_ptr<ClauseDatabase>& clauseDB)
	: CadiBackBase(id, clauseDB, BackboneSolverType::CADIBACK_SQRT)
{
	initializeTypeId<CadiBackSqrt>();
}

void
CadiBackSqrt::initCandidates(std::vector<int>& candidates)
{
	/* Blocks are consecutive in the initial order. Every later change of the candidates (filter, board, fixed,
	 * backbone) keeps the order, so the remaining candidates of the lowest block are always a prefix */
	const size_t n = candidates.size();
	m_blockSize = std::max<size_t>(1, (size_t)std::ceil(std::sqrt((double)n)));
	m_blockCount = (n + m_blockSize - 1) / m_blockSize;
	m_currentBlock = NO_BLOCK;

	m_blockOf.assign(m_nbVars + 1, NO_BLOCK);
	for (size_t i = 0; i < n; i++)
		m_blockOf[std::abs(candidates[i])] = i / m_blockSize;

	LOG1("%s %d: %zu blocks of %zu candidates", variantName(), this->getSolverId(), m_blockCount, m_blockSize);
}

void
CadiBackSqrt::selectChunk(const std::vector<int>& candidates, std::vector<int>& chunk)
{
	const unsigned int block = m_blockOf[std::abs(candidates[0])];
	assert(block != NO_BLOCK);

	if (block != m_currentBlock) {
		/* Same start as the whole search: the chunk policy restarts on every block */
		m_currentBlock = block;
		m_chunkSize = (m_chunkRate == 0) ? std::numeric_limits<size_t>::max() : 1;
		LOGDEBUG1("%s %d: starting block %u", variantName(), this->getSolverId(), block);
	}

	for (size_t i = 0; i < candidates.size() && chunk.size() < m_chunkSize; i++) {
		if (m_blockOf[std::abs(candidates[i])] != block)
			break;
		chunk.push_back(candidates[i]);
	}
	assert(!chunk.empty());
}

void
CadiBackSqrt::printWinningLog()
{
	CadiBackBase::printWinningLog();
	LOGSTAT("%s: %zu blocks of %zu candidates", variantName(), m_blockCount, m_blockSize);
}
