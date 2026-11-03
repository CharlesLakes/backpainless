#include "solvers/CDCL/cadiback/CadiBackPartition.hpp"

#include "utils/Logger.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

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

bool
CadiBackPartition::parseModeList(const std::string& list, std::vector<Mode>& modes)
{
	modes.clear();
	std::stringstream stream(list);
	std::string name;
	while (std::getline(stream, name, ',')) {
		if (name == "steal")
			modes.push_back(Mode::STEAL);
		else if (name == "walk")
			modes.push_back(Mode::WALK);
		else
			return false;
	}
	return !modes.empty() && list.back() != ',';
}

size_t
CadiBackPartition::blockVariables(unsigned int nbVars, int option)
{
	if (option > 0)
		return option;
	return std::max<size_t>(1, (size_t)std::ceil(std::sqrt((double)nbVars)));
}

size_t
CadiBackPartition::blocksPerPart(unsigned int nbVars, unsigned int parts, size_t blockVars)
{
	/* Part p holds the variables of ranks p, p + P, ...: at most ceil(n / P) of them */
	size_t largest = (nbVars + parts - 1) / std::max(1u, parts);
	return std::max<size_t>(1, (largest + blockVars - 1) / blockVars);
}

void
CadiBackPartition::setClaims(const std::shared_ptr<PartClaims>& claims, size_t blockVars)
{
	m_claims = claims;
	m_blockVars = std::max<size_t>(1, blockVars);
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

	/* Steal mode: the decisions of the others only arrive through the board, so it must be read and fed completely */
	m_stealing = false;
	if (m_mode == Mode::STEAL) {
		if (!m_claims)
			LOGWARN("%s %d: steal mode needs the candidate board (-bb-share-cand), walk mode instead",
					variantName(),
					this->getSolverId());
		else if (!m_consumeCandidates || !publishesBackbone() || !publishesFree())
			LOGWARN("%s %d: steal mode needs -bb-share-cand=1 and -bb-share-cand-kind=all for this worker, walk mode "
					"instead",
					variantName(),
					this->getSolverId());
		else
			m_stealing = true;
	}

	if (m_stealing) {
		/* Blocks of B variables in the order of the ranking inside each part: rank r is the (r / P)-th variable of
		 * part r mod P */
		m_blocksPerPart = blocksPerPart(m_nbVars, m_partCount, m_blockVars);
		assert(m_claims->size() == m_blocksPerPart * m_partCount);
		m_blockOfVar.assign(m_nbVars + 1, NO_BLOCK);
		for (size_t rank = 0; rank < vars.size(); rank++)
			m_blockOfVar[vars[rank]] = (rank % m_partCount) * m_blocksPerPart + (rank / m_partCount) / m_blockVars;
		m_current = NO_BLOCK;
		m_ownNext = 0;
	}

	/* Own part first, then the next ones; the order of -bb-order is kept inside each part (each block in steal mode) */
	std::stable_sort(candidates.begin(), candidates.end(), [this](int a, int b) {
		unsigned int pa = relativePart(a), pb = relativePart(b);
		if (pa != pb || !m_stealing)
			return pa < pb;
		return m_blockOfVar[std::abs(a)] < m_blockOfVar[std::abs(b)];
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
	if (m_stealing)
		LOG1("%s %d: steal mode, %zu blocks of %zu variables per part",
			 variantName(),
			 this->getSolverId(),
			 m_blocksPerPart,
			 m_blockVars);
}

unsigned int
CadiBackPartition::blockOf(size_t /* i */, int lit) const
{
	return relativePart(lit);
}

/* Steal mode */

void
CadiBackPartition::gatherChunk(const std::vector<int>& candidates, std::vector<int>& chunk) const
{
	for (int lit : candidates) {
		if (chunk.size() >= m_chunkSize)
			break;
		if (m_blockOfVar[std::abs(lit)] == m_current)
			chunk.push_back(lit);
	}
}

bool
CadiBackPartition::claimNextBlock(const std::vector<int>& candidates)
{
	const PartClaims::Code me = PartClaims::code(m_partIndex);
	m_present.assign(m_claims->size(), 0);
	for (int lit : candidates)
		m_present[m_blockOfVar[std::abs(lit)]] = 1;

	/* Own part, from the front; a block without any candidate left here is not claimed (its variables are decided
	 * for this worker, and published) */
	const size_t ownFirst = (size_t)m_partIndex * m_blocksPerPart;
	for (; m_ownNext < m_blocksPerPart; m_ownNext++) {
		size_t block = ownFirst + m_ownNext;
		if (m_present[block] && m_claims->tryClaim(block, me)) {
			m_current = block;
			m_ownNext++;
			m_ownBlocks++;
			return true;
		}
	}

	/* Steal from the end of the part with the most free blocks holding candidates of this worker */
	for (;;) {
		size_t best = NO_BLOCK, bestCount = 0;
		for (unsigned int part = 0; part < m_partCount; part++) {
			if (part == m_partIndex)
				continue;
			size_t count = 0, last = NO_BLOCK;
			for (size_t i = 0; i < m_blocksPerPart; i++) {
				size_t block = (size_t)part * m_blocksPerPart + i;
				PartClaims::Code owner = m_claims->owner(block);
				if (m_present[block] && (owner == 0 || owner == me)) {
					count++;
					last = block;
				}
			}
			if (count > bestCount) {
				bestCount = count;
				best = last;
			}
		}
		if (best == NO_BLOCK)
			return false;
		if (m_claims->tryClaim(best, me)) {
			m_current = best;
			m_stolenBlocks++;
			return true;
		}
		/* Claimed by another worker in the meantime: look again */
	}
}

void
CadiBackPartition::selectChunk(const std::vector<int>& candidates, std::vector<int>& chunk)
{
	if (!m_stealing) {
		CadiBackSqrt::selectChunk(candidates, chunk);
		return;
	}

	/* A lower d index of another rank claimed the same block in the same round: it keeps it */
	if (m_current != NO_BLOCK && m_claims->owner(m_current) != PartClaims::code(m_partIndex)) {
		m_lostBlocks++;
		LOGDEBUG1("%s %d: block %zu taken over by another rank", variantName(), this->getSolverId(), m_current);
		m_current = NO_BLOCK;
	}

	if (m_current != NO_BLOCK) {
		gatherChunk(candidates, chunk);
		if (!chunk.empty())
			return;
		m_current = NO_BLOCK; /* block done */
	}

	if (!claimNextBlock(candidates)) {
		/* Every remaining candidate is in a block of another worker: its answers come through the board */
		const auto start = std::chrono::steady_clock::now();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		m_waits++;
		m_waitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		return;
	}

	/* Same start as the whole search: the chunk policy restarts on every block */
	m_chunkSize = (m_chunkRate == 0) ? std::numeric_limits<size_t>::max() : 1;
	LOGDEBUG1("%s %d: starting block %zu", variantName(), this->getSolverId(), m_current);
	gatherChunk(candidates, chunk);
	assert(!chunk.empty());
}

void
CadiBackPartition::printStatistics()
{
	CadiBackSqrt::printStatistics();
	/* Printed under the logger lock taken by BackboneSolverFactory::printStats, so std::cout and not LOGSTAT */
	if (m_stealing)
		std::cout << "c|   part, solver " << this->getSolverId() << " (part " << m_partIndex << " of " << m_partCount
				  << ", steal, " << m_blocksPerPart << " blocks of " << m_blockVars << " variables per part): "
				  << m_ownBlocks << " own blocks, " << m_stolenBlocks << " stolen, " << m_lostBlocks << " lost, "
				  << m_waits << " waits (" << m_waitSeconds << " s)\n";
}

void
CadiBackPartition::printWinningLog()
{
	CadiBackSqrt::printWinningLog();
	LOGSTAT("%s: part %u of %u (%zu initial candidates in it), partition signature %016" PRIx64 ", %s mode",
			variantName(),
			m_partIndex,
			m_partCount,
			m_ownCandidates,
			m_signature,
			m_stealing ? "steal" : "walk");
	if (m_stealing)
		LOGSTAT("%s: %lu own blocks, %lu stolen, %lu lost, %lu waits (%.2f s), blocks of %zu variables",
				variantName(),
				m_ownBlocks,
				m_stolenBlocks,
				m_lostBlocks,
				m_waits,
				m_waitSeconds,
				m_blockVars);
}
