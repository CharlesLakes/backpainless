#pragma once

#include "solvers/BackboneSolverInterface.hpp"
#include "solvers/XorSampler.hpp"

#include "cryptominisat/src/cryptominisat.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <unordered_set>
#include <vector>

/**
 * @brief Backbone solver whose first goal is exploration: it diversifies the portfolio by splitting the search space
 * with random XOR constraints (CryptoMiniSat, native XORs with Gaussian elimination).
 * @ingroup solving
 *
 * Valiant-Vazirani / XOR streamlining (Gomes et al. 2006) / ApproxMC (Chakraborty et al. 2013): a random XOR over the
 * variables splits the models in two halves with high probability, m XORs make a binary tree of height m whose leaves
 * are the 2^m assignments of their right-hand sides.
 *
 * solve():
 * 1. Solve once, the candidates are the literals of the model (minus the flippable ones).
 * 2. -bb-xor-rounds rounds: draw m (-bb-xor-m <= 64) random XORs over the variables of the candidates, keeping only
 *    the ones linearly independent of the previous ones (GF(2) basis: a dependent XOR does not split anything), and
 *    visit the tree in DFS order with a 64-bit mask (bit m-1-j = right-hand side of XOR j, XOR 0 at the root) and a
 *    depth (-bb-xor-dfs): adaptive solves the inner nodes too, going down after SAT and skipping the subtree when the
 *    conflict budget runs out; leaves solves the leaves only. An UNSAT node prunes the subtree of the deepest XOR of
 *    its conflict. Every model (a model of the formula) marks the
 *    negation of each of its literals as "not backbone" (@ref m_notBackbone): a variable with both literals marked is
 *    free, it leaves the candidates and is published on the candidate board (-bb-share-cand).
 * 3. Complete the backbone with the CadiBack loop (-bb-chunk, -bb-order) on the same CryptoMiniSat instance, so that
 *    this solver can also win.
 *
 * Soundness of sharing: XOR j is added as the definition x_1 ^ ... ^ x_k ^ s_j = 0 of a fresh selector s_j and its
 * right-hand side is chosen by assuming s_j; the chunk constraint of step 3 is a clause guarded by a fresh activation
 * literal, also assumed. Fresh variables defined this way are a conservative extension of the formula: a learnt
 * clause over the original variables only is implied by the formula, the others are never exported. Every model is a
 * model of the formula.
 */
class DiverseBackboneSearch : public BackboneSolverInterface
{
  public:
	DiverseBackboneSearch(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	~DiverseBackboneSearch() override;

	/* Execution */

	BackboneResult solve(const std::vector<int>& cube) override;

	std::vector<int> getBackbone() override;

	void setSolverInterrupt() override;

	void unsetSolverInterrupt() override;

	void diversify(const SeedGenerator& getSeed) override;

	/* Formula */

	void loadFormula(const char* filename) override;

	void addInitialClauses(const std::vector<simpleClause>& clauses, unsigned int nbVars) override;

	void addInitialClauses(const lit_t* literals, unsigned int clsCount, unsigned int nbVars) override;

	unsigned int getVariablesCount() override;

	void setPhase(const unsigned int var, const bool phase) override;

	/* Sharing */

	bool importClause(const ClauseExchangePtr& clause) override;

	void importClauses(const std::vector<ClauseExchangePtr>& clauses) override;

	/* Statistics */

	void printStatistics() override;

	void printWinningLog() override;

  private:
	/* Search */

	/// @brief One round of XOR exploration over the variables of @p candidates. COMPLETE when the round is over,
	/// UNSAT when the formula is unsatisfiable, UNKNOWN when interrupted.
	BackboneResult exploreRound(std::vector<int>& candidates);

	/// @brief CadiBack loop on the remaining candidates (step 3). COMPLETE, or UNKNOWN when interrupted.
	BackboneResult completeBackbone(std::vector<int>& candidates);

	/// @brief Solves under @p assumptions with a conflict budget (0: until an answer or an interruption). Imports the
	/// pending clauses before and exports the learnt ones after. Returns 10 (SAT), 20 (UNSAT) or 0 (unknown).
	int solveWith(const std::vector<CMSat::Lit>& assumptions, unsigned long conflicts);

	/// @brief Records the current model: marks the negations of its literals as not backbone, drops the candidates
	/// that became free (falsified, or flippable unless -bb-no-flip) and publishes them on the candidate board.
	void filterWithModel(std::vector<int>& candidates);

	/// @brief True if flipping the true literal @p lit keeps every original clause satisfied by the current model.
	bool flippable(int lit) const;

	/// @brief Moves the root-level fixed candidates to the backbone, drops the ones fixed to false.
	void extractFixed(std::vector<int>& candidates);

	/// @brief With -bb-share-cand consume: drops the candidates published as free, takes the published backbone.
	void syncWithBoard(std::vector<int>& candidates);

	/// @brief Marks the variable of @p lit as free (both literals not backbone) and publishes it on the board.
	void markFree(int lit);

	/// @brief Records a backbone literal proven by this solver, adds it as a unit, exports and publishes it.
	void commitBackbone(int lit);

	/// @brief Adds a backbone literal to @ref m_backbone, publishes it on the board and exports it as a unit (only if
	/// enabled for this worker by -bb-share-units).
	void addBackboneLiteral(int lit);

	/// @brief Adds the clauses received from the other solvers (between two solve calls only).
	void importPendingClauses();

	/// @brief Exports the new fixed literals and small learnt clauses over the original variables.
	void exportLearntClauses();

	/// @brief Exports one clause if it was not exported before.
	void exportOnce(std::vector<int>& clause, int lbd);

	static CMSat::Lit toCms(int lit) { return CMSat::Lit(std::abs(lit) - 1, lit < 0); }

	static int fromCms(CMSat::Lit lit) { return lit.sign() ? -(int)(lit.var() + 1) : (int)(lit.var() + 1); }

	/// @brief Index of a literal in the arrays indexed by literal.
	static size_t litIndex(int lit) { return 2 * (size_t)std::abs(lit) + (lit < 0); }

	/// @brief Value of @p lit in the current model (@ref m_model): true / false.
	bool modelValue(int lit) const { return (m_model[std::abs(lit)] > 0) == (lit > 0); }

  private:
	std::unique_ptr<CMSat::SATSolver> m_solver;

	/// @brief Interruption requested. CryptoMiniSat resets its own flag at the start of each solve call, so this one is
	/// checked between calls and the calls are bounded (see solveWith).
	std::atomic<bool> m_stop;

	unsigned int m_nbVars = 0;

	/// @brief Conflict budget of a DFS node, from -bb-xor-confl and the conflicts of the first model (0: no limit).
	unsigned long m_nodeBudget = 0;

	std::mt19937_64 m_rng;

	unsigned int m_seed = 0;

	/// @brief Original clauses (flat, zero separated) and, per literal, the offsets of the clauses containing it. Used
	/// by flippable; empty with -bb-no-flip.
	std::vector<int> m_clauses;
	std::vector<std::vector<size_t>> m_occurrenceLists;

	/// @brief Occurrences of each literal, for -bb-order.
	LiteralOccurrences m_occurrences;

	/// @brief Current model, indexed by variable: 1 true, -1 false.
	std::vector<int8_t> m_model;

	/// @brief Indexed by literal: 1 if a model falsifies it, i.e. it is known not to be a backbone literal.
	std::vector<char> m_notBackbone;

	/// @brief Indexed by variable: 1 if both literals are not backbone.
	std::vector<char> m_free;

	std::vector<int> m_backbone;

	/// @brief Indexed by variable: sign of its root-level value in CryptoMiniSat (0 when not fixed).
	std::vector<int8_t> m_fixed;

	/// @brief Indexed by variable: 1 if its unit was already exported, or must not be (added by this solver).
	std::vector<char> m_unitExported;

	/// @brief Hashes of the clauses already exported (CryptoMiniSat gives its whole learnt database at each query).
	std::unordered_set<uint64_t> m_exportedHashes;

	uint64_t m_conflictsAtLastExport = 0;

	/* Statistics */
	unsigned long m_satCalls = 0;
	unsigned long m_satAnswers = 0;
	unsigned long m_unsatAnswers = 0;
	unsigned long m_fixedFound = 0;
	unsigned long m_xorRounds = 0;
	unsigned long m_xorsAdded = 0;
	unsigned long m_xorsDependent = 0;
	XorDfsStats m_dfsStats;
	unsigned long m_freeByXor = 0;		 ///< free variables found during the XOR exploration
	unsigned long m_freeVariables = 0;	 ///< free variables found by this solver
	unsigned long m_boardFree = 0;		 ///< candidates dropped because the board says free
	unsigned long m_boardBackbone = 0;	 ///< backbone literals taken from the board
	unsigned long m_publishedFree = 0;	 ///< free variables this solver was the first to publish on the board
	unsigned long m_publishedBackbone = 0; ///< backbone literals this solver was the first to publish on the board
	unsigned long m_exported = 0;		 ///< learnt clauses exported
	/// @brief Seconds spent in CryptoMiniSat solve calls, in the exports, the imports and filterWithModel
	double m_timeSolve = 0, m_timeExport = 0, m_timeImport = 0, m_timeFilter = 0;
};
