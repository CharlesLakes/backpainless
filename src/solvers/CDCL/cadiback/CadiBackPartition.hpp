#pragma once

#include "containers/PartClaims.hpp"
#include "solvers/CDCL/cadiback/CadiBackSqrt.hpp"

#include <cstdint>
#include <memory>

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
 * reaches them. Every worker still computes the whole backbone. This is the walk mode (-bb-part-mode=walk): a worker
 * that reaches a part before the board decided it checks its candidates again, the work is done several times.
 *
 * Steal mode (-bb-part-mode=steal, the default): every part is cut into blocks of B variables (-bb-part-block, by
 * default ceil(sqrt(#variables))) in the order of the ranking, again the same on every worker, and a worker only
 * constrains the candidates of the blocks it claimed in a table shared with the board (PartClaims). It claims the
 * blocks of its own part from the front, one at a time; once none is left it steals a block from the end of the part
 * with the most free blocks holding candidates of its own, so that the owner and the thieves only meet at the end.
 * When every block holding its remaining candidates is claimed by others, it waits for their answers on the board
 * (empty chunk, a 1 ms sleep). The chunk policy restarts on every block. A SAT answer still filters every candidate.
 * No candidate is checked by two workers, except with -dist when two ranks claim the same block in one sharing round
 * (the lower d index keeps it, the other gives it up at its next chunk).
 *
 * Steal mode needs the board both ways and every status published (-bb-share-cand=1, -bb-share-cand-kind=all), since
 * the decisions of the others only arrive through it; a d worker without them falls back to walk. Waiting on a block
 * claimed by another d worker is safe: every candidate of that block is either a candidate of the owner (with either
 * sign, decided and published by it) or a variable it already published as free or backbone.
 */
class CadiBackPartition : public CadiBackSqrt
{
  public:
	CadiBackPartition(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	const char* variantName() const override { return "CadiBackPartition"; }

	/// @brief This worker owns part @p index out of @p count (set by BackboneSolverFactory::diversification).
	void setPartition(unsigned int index, unsigned int count);

	enum class Mode
	{
		STEAL, ///< only the blocks claimed in PartClaims
		WALK   ///< every part, own one first
	};

	/// @brief Parses a comma-separated list of -bb-part-mode (steal, walk). Returns false if it is empty or invalid.
	static bool parseModeList(const std::string& list, std::vector<Mode>& modes);

	/// @brief Mode of this worker (set per worker by -bb-part-mode).
	void setMode(Mode mode) { m_mode = mode; }

	/// @brief Variables per block for a formula of @p nbVars variables and -bb-part-block=@p option.
	static size_t blockVariables(unsigned int nbVars, int option);

	/// @brief Blocks of each part (the largest part, every part gets this many ids) for @p parts parts.
	static size_t blocksPerPart(unsigned int nbVars, unsigned int parts, size_t blockVars);

	/// @brief Table of the claims of the d workers sharing a board, with blocks of @p blockVars variables.
	void setClaims(const std::shared_ptr<PartClaims>& claims, size_t blockVars);

	void printStatistics() override;

	void printWinningLog() override;

  protected:
	void initCandidates(std::vector<int>& candidates) override;

	size_t blockSize(size_t n) const override;

	unsigned int blockOf(size_t i, int lit) const override;

	void selectChunk(const std::vector<int>& candidates, std::vector<int>& chunk) override;

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

	/* Steal mode */

	static constexpr size_t NO_BLOCK = (size_t)-1;

	/// @brief Claims the next block holding candidates: own part from the front, else stolen from the end of another
	/// part. Returns false if every block holding candidates is claimed by another worker.
	bool claimNextBlock(const std::vector<int>& candidates);

	/// @brief Fills @p chunk with up to @ref m_chunkSize candidates of the current block.
	void gatherChunk(const std::vector<int>& candidates, std::vector<int>& chunk) const;

	Mode m_mode = Mode::WALK;

	/// @brief Steal mode in use (decided by initCandidates: it needs the claims and the board both ways).
	bool m_stealing = false;

	std::shared_ptr<PartClaims> m_claims;

	size_t m_blockVars = 1;

	size_t m_blocksPerPart = 1;

	/// @brief Block of each variable (part * blocks per part + index in the part), the same on every worker.
	std::vector<size_t> m_blockOfVar;

	/// @brief Blocks holding remaining candidates (scratch of claimNextBlock).
	std::vector<char> m_present;

	/// @brief Block of the current chunks, NO_BLOCK when a new one must be claimed.
	size_t m_current = NO_BLOCK;

	/// @brief Next block of the own part to try (index inside the part).
	size_t m_ownNext = 0;

	unsigned long m_ownBlocks = 0;	  ///< blocks of the own part claimed
	unsigned long m_stolenBlocks = 0; ///< blocks of other parts claimed
	unsigned long m_lostBlocks = 0;	  ///< claimed blocks taken over by a lower d index of another rank
	unsigned long m_waits = 0;		  ///< empty chunks (every block left claimed by others)
	double m_waitSeconds = 0;
};
