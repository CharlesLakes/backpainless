#include "solvers/CDCL/cadiback/CadiBackPartition.hpp"

#include "utils/Logger.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <numeric>

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
	/* Only used for the block count of the log: the blocks are the parts (blockOf) */
	return std::max<size_t>(1, (n + m_partCount - 1) / m_partCount);
}

unsigned int
CadiBackPartition::relativePart(int lit) const
{
	return (m_partOf[std::abs(lit)] + m_partCount - m_partIndex) % m_partCount;
}

void
CadiBackPartition::initCandidates(std::vector<int>& candidates)
{
	if (!m_consumeCandidates)
		LOGWARN("%s %d: the candidate board is off for this worker (-bb-share-cand), the parts are only an order",
				variantName(),
				this->getSolverId());

	/* The rank of the variables depends only on the formula (the occurrences come from the initial clauses), so every
	 * worker, on every rank, computes the same parts */
	std::vector<int> vars(m_nbVars);
	std::iota(vars.begin(), vars.end(), 1);
	if (!m_occurrences.empty()) {
		auto total = [this](int v) { return (unsigned long)m_occurrences[v] + m_occurrences[-v]; };
		std::stable_sort(vars.begin(), vars.end(), [&total](int a, int b) { return total(a) > total(b); });
	}
	m_partOf.assign(m_nbVars + 1, 0);
	for (size_t rank = 0; rank < vars.size(); rank++)
		m_partOf[vars[rank]] = rank % m_partCount;

	/* FNV-1a of the parts of the variables 1..n */
	m_signature = 14695981039346656037ULL;
	for (unsigned int v = 1; v <= m_nbVars; v++) {
		m_signature ^= m_partOf[v];
		m_signature *= 1099511628211ULL;
	}

	/* Own part first, then the next ones; the order of -bb-order is kept inside each part */
	std::stable_sort(candidates.begin(), candidates.end(), [this](int a, int b) {
		return relativePart(a) < relativePart(b);
	});
	m_ownCandidates = std::count_if(candidates.begin(), candidates.end(), [this](int l) { return relativePart(l) == 0; });

	CadiBackSqrt::initCandidates(candidates);
	LOG1("%s %d: part %u of %u, %zu of its %zu candidates in it, partition signature %016" PRIx64,
		 variantName(),
		 this->getSolverId(),
		 m_partIndex,
		 m_partCount,
		 m_ownCandidates,
		 candidates.size(),
		 m_signature);
}

unsigned int
CadiBackPartition::blockOf(size_t /* i */, int lit) const
{
	return relativePart(lit);
}

void
CadiBackPartition::printWinningLog()
{
	CadiBackSqrt::printWinningLog();
	LOGSTAT("%s: part %u of %u (%zu initial candidates in it), partition signature %016" PRIx64,
			variantName(),
			m_partIndex,
			m_partCount,
			m_ownCandidates,
			m_signature);
}
