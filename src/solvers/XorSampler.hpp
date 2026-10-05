#pragma once

#include "solvers/XorOptions.hpp"

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

/**
 * @brief Random XOR exploration shared by the backbone solvers that split the search space with XORs, independent of
 * the SAT engine (DiverseBackboneSearch on CryptoMiniSat, CadiBackXor on CaDiCaL).
 * @ingroup solving
 */

/**
 * @brief Draws up to @p m linearly independent random XORs over @p pool (GF(2) basis: a XOR that is a sum of the
 * previous ones is already decided by the path to its node, it would not split anything).
 * @param pool Variables the XORs are drawn from (any numbering, copied as is in @p xors).
 * @param xors Filled with the variables of each XOR (right-hand sides are chosen by the DFS).
 * @return Number of dependent XORs drawn and dropped.
 */
unsigned long
drawRandomXors(const std::vector<int>& pool,
			   unsigned int m,
			   const XorDensity& density,
			   std::mt19937_64& rng,
			   std::vector<std::vector<int>>& xors);

/// @brief Answer of the engine on one node of the XOR tree.
enum class XorNodeAnswer
{
	SAT,	 ///< a model was found (the caller already used it)
	UNSAT,	 ///< no model: @ref XorNodeResult::deepest is the deepest XOR of the conflict
	UNKNOWN, ///< budget exhausted
	STOP	 ///< stop the exploration (interrupted, formula UNSAT, no candidate left)
};

struct XorNodeResult
{
	XorNodeAnswer answer;
	unsigned int deepest = 0; ///< UNSAT only: index (0 = root) of the deepest XOR the conflict depends on
};

struct XorDfsStats
{
	unsigned long sat = 0, unsat = 0, unknown = 0;
	unsigned long pruned = 0;	///< leaves of the subtrees pruned by a conflict (saturated)
	unsigned long depthSum = 0; ///< sum of the depths of the SAT nodes
	unsigned long nodes = 0;
};

/**
 * @brief Iterative DFS over the tree of the right-hand sides of m XORs (m <= 64), on (mask, depth).
 *
 * The node at depth d assumes the first d XORs, XOR j (j = 0 at the root) with the right-hand side of bit m-1-j of
 * 'mask', whose bits below the node are zero. Leaves mode (@p adaptive false): always depth m, counting up visits the
 * leaves from left to right. Adaptive mode: SAT goes down to the left child, UNSAT prunes the subtree of the deepest
 * XOR of the conflict, an exhausted budget skips the subtree of the node. Skipping the subtree at depth k moves to
 * (mask | (2^(m-k) - 1)) + 1, the carry climbs to the first ancestor with a right child left: its depth is m - ctz.
 * Skipping on a timeout only loses exploration, never soundness.
 *
 * @param solveNode Called as solveNode(depth, mask), returns an XorNodeResult.
 * @param nodeLimit Maximum number of nodes solved (0: no limit).
 * @return false when solveNode answered STOP.
 */
template<typename SolveNode>
bool
xorDfs(unsigned int m, bool adaptive, unsigned long nodeLimit, SolveNode&& solveNode, XorDfsStats& stats)
{
	uint64_t mask = 0;
	unsigned int depth = adaptive ? 1 : m;
	for (unsigned long nodes = 0; nodeLimit == 0 || nodes < nodeLimit; nodes++) {
		XorNodeResult result = solveNode(depth, mask);
		stats.nodes++;

		unsigned int skipDepth = depth; /* depth of the subtree to skip */
		switch (result.answer) {
			case XorNodeAnswer::STOP:
				return false;
			case XorNodeAnswer::SAT:
				stats.sat++;
				stats.depthSum += depth;
				if (adaptive && depth < m) {
					depth++; /* left child: its bit is already zero */
					continue;
				}
				break;
			case XorNodeAnswer::UNSAT: {
				stats.unsat++;
				skipDepth = result.deepest + 1;
				uint64_t leaves = 1ULL << (m - skipDepth);
				stats.pruned = (leaves > std::numeric_limits<unsigned long>::max() - stats.pruned)
								   ? std::numeric_limits<unsigned long>::max()
								   : stats.pruned + leaves;
				break;
			}
			case XorNodeAnswer::UNKNOWN:
				stats.unknown++;
				break;
		}

		/* First node of the next subtree: m = 64 overflows to 0 after the last one */
		uint64_t next = (mask | ((1ULL << (m - skipDepth)) - 1)) + 1;
		if ((m == 64) ? next == 0 : (next >> m) != 0)
			break;
		mask = next;
		depth = adaptive ? m - __builtin_ctzll(mask) : m;
	}
	return true;
}

/// @brief Right-hand side given to XOR @p j by the node @p mask of a tree of @p m XORs.
inline bool
xorRhs(uint64_t mask, unsigned int m, unsigned int j)
{
	return (mask >> (m - 1 - j)) & 1;
}
