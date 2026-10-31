#include "solvers/CDCL/cadiback/CadiBackPartition.hpp"

#include "utils/Logger.hpp"

#include <algorithm>

CadiBackPartition::CadiBackPartition(int id, const std::shared_ptr<ClauseDatabase>& clauseDB)
	: CadiBackSqrt(id, clauseDB, BackboneSolverType::CADIBACK_PARTITION)
{
	initializeTypeId<CadiBackPartition>();
}

void
CadiBackPartition::setPartition(unsigned int index, unsigned int count)
{
	m_partCount = std::max(1u, count);
	m_partIndex = index % m_partCount;
}

size_t
CadiBackPartition::blockSize(size_t n) const
{
	return std::max<size_t>(1, (n + m_partCount - 1) / m_partCount);
}

void
CadiBackPartition::initCandidates(std::vector<int>& candidates)
{
	if (!m_consumeCandidates)
		LOGWARN("%s %d: the candidate board is off for this worker (-bb-share-cand), the parts are only an order",
				variantName(),
				this->getSolverId());

	const size_t n = candidates.size();
	const size_t size = blockSize(n);
	/* With fewer candidates than workers some parts are empty: the worker takes a non-empty one */
	m_parts = std::max<size_t>(1, (n + size - 1) / size);
	m_ownPart = m_partIndex % m_parts;
	m_initialCount = n;
	std::rotate(candidates.begin(), candidates.begin() + std::min(n, m_ownPart * size), candidates.end());

	CadiBackSqrt::initCandidates(candidates);
	LOG1("%s %d: part %u of %zu (%u workers)", variantName(), this->getSolverId(), m_ownPart, m_parts, m_partCount);
}

unsigned int
CadiBackPartition::blockOf(size_t i, int /* lit */) const
{
	/* Position i of the rotated candidates comes from position (own * size + i) mod n of the initial order. Blocks
	 * are the parts counted from the own one, so the last part (possibly shorter) keeps its bounds */
	const size_t initial = (m_ownPart * m_blockSize + i) % m_initialCount;
	return (initial / m_blockSize + m_parts - m_ownPart) % m_parts;
}

void
CadiBackPartition::printWinningLog()
{
	CadiBackSqrt::printWinningLog();
	LOGSTAT("%s: part %u of %zu", variantName(), m_ownPart, m_parts);
}
