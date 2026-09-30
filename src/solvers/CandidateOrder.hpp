#pragma once

#include <string>
#include <vector>

/**
 * @brief Static orders of the initial backbone candidates (-bb-order), independent of the SAT engine.
 * @ingroup solving
 *
 * The order only decides which candidates are tried first (the first ones of the chunk, or the blocks of a variant),
 * every order is sound.
 *
 * To add an order, edit the 6 places marked "NEW ORDER (i/6)".
 */
enum class CandidateOrder
{
	NATURAL, ///< by variable index (cadiback.cpp)
	REVERSE, ///< by decreasing variable index
	RANDOM,	 ///< shuffled with the seed of the solver
	OCC,	 ///< by decreasing number of occurrences of the candidate literal in the formula
	OCC_REV, ///< by increasing number of occurrences of the candidate literal in the formula
	/* NEW ORDER (1/6): add the value here */
};

/// @brief Parses one order name (natural, reverse, random, occ, occ-rev). Returns false for an unknown name.
bool parseCandidateOrder(const std::string& name, CandidateOrder& order);

/// @brief Parses the comma-separated list of -bb-order. Returns false if it is empty or has an unknown name.
bool parseCandidateOrderList(const std::string& list, std::vector<CandidateOrder>& orders);

const char* candidateOrderName(CandidateOrder order);

/// @brief Counts the occurrences of every literal, indexed by occurrenceIndex. Filled from the initial clauses.
class LiteralOccurrences
{
  public:
	void reset(unsigned int nbVars) { m_counts.assign(2 * (nbVars + 1), 0); }

	/// @brief Counts one literal of an original clause (0 and out of range literals are ignored).
	void count(int lit)
	{
		size_t idx = occurrenceIndex(lit);
		if (lit && idx < m_counts.size())
			m_counts[idx]++;
	}

	bool empty() const { return m_counts.empty(); }

	unsigned int operator[](int lit) const
	{
		size_t idx = occurrenceIndex(lit);
		return (idx < m_counts.size()) ? m_counts[idx] : 0;
	}

  private:
	static size_t occurrenceIndex(int lit) { return 2 * (size_t)((lit > 0) ? lit : -lit) + (lit < 0); }

	std::vector<unsigned int> m_counts;
};

/**
 * @brief Sorts @p candidates in @p order. Ties keep the variable order.
 * @param occurrences Needed by OCC and OCC_REV: when empty (formula loaded without its clauses), the order stays
 * natural.
 * @param seed Used by RANDOM.
 */
void sortCandidates(std::vector<int>& candidates,
					CandidateOrder order,
					const LiteralOccurrences& occurrences,
					unsigned int seed);
