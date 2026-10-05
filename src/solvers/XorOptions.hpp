#pragma once

#include <string>
#include <vector>

/**
 * @brief Options of the random XOR exploration of DiverseBackboneSearch (-bb-xor-*), independent of the SAT engine.
 * @ingroup solving
 */

/// @brief Variables of a random XOR: each candidate variable with @ref probability, or exactly @ref length of them.
struct XorDensity
{
	double probability = 0.5; ///< used when length is 0
	unsigned int length = 0;  ///< fixed number of variables (0: use probability)
};

/// @brief Conflict budget of a DFS node: absolute, or relative to the conflicts of the first model.
struct XorBudget
{
	unsigned long conflicts = 0; ///< absolute budget when factor is 0 (0: no limit)
	double factor = 2.0;		 ///< > 0: factor times the conflicts of the first solve call (at least MIN_XOR_BUDGET)
};

/// @brief Smallest relative budget (an easy first model takes almost no conflict).
constexpr unsigned long MIN_XOR_BUDGET = 1000;

/// @brief XOR options of one worker.
struct XorOptions
{
	unsigned int count = 10;		///< m, XORs per round (1 to 64)
	XorDensity density;				///< variables of each XOR
	unsigned long rounds = 1;		///< rounds of exploration
	bool adaptive = true;			///< adaptive DFS (solve the inner nodes too) or leaves only
	bool exportLearnt = false;		///< x: export the small learnt clauses (fixed literals always are)
	bool pre = false;				///< c, s: run the XOR exploration before CadiBack
	unsigned long leaves = 0;		///< nodes solved per round (0: no limit)
	XorBudget budget;				///< conflict budget of a DFS node
};

/// @brief Parsed -bb-xor-* lists, each one cycled over the solver ids independently (as the other -bb-* lists).
struct XorOptionLists
{
	std::vector<unsigned long> counts;
	std::vector<XorDensity> densities;
	std::vector<unsigned long> rounds;
	std::vector<unsigned long> leaves;
	std::vector<char> adaptive;
	std::string exportMask;
	std::string preMask;
	std::vector<XorBudget> budgets;

	/// @brief Options of the solver with global id @p id.
	XorOptions forSolver(unsigned int id) const;
};

/// @brief Maximum number of XORs per round: the DFS over their parities is a 64-bit mask.
constexpr unsigned int MAX_XOR_COUNT = 64;

/// @brief Parses the -bb-xor-* parameters. Returns false and sets @p error when one of them is invalid.
bool parseXorOptionLists(XorOptionLists& lists, std::string& error);

/// @brief Parses a comma-separated list of densities: probabilities in (0,1] written with a '.', or integers >= 1.
bool parseXorDensityList(const std::string& list, std::vector<XorDensity>& densities);

/// @brief Parses a comma-separated list of DFS modes: adaptive, leaves.
bool parseXorDfsList(const std::string& list, std::vector<char>& adaptive);

/// @brief Parses a comma-separated list of budgets: integers (absolute, 0 = no limit) or '<factor>x' (relative).
bool parseXorBudgetList(const std::string& list, std::vector<XorBudget>& budgets);

/// @brief Readable form of a budget ("10000" or "2x").
std::string xorBudgetName(const XorBudget& budget);

/// @brief Readable form of a density ("p=0.5" or "k=8").
std::string xorDensityName(const XorDensity& density);
