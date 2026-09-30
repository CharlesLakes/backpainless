#pragma once

#include "solvers/BackboneSolverInterface.hpp"

#include "cadical/src/cadical.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Base of the backbone solvers following the CadiBack algorithm on top of CaDiCaL.
 * @ingroup solving
 *
 * Algorithm 1 of Biere, Froleyks, Wang, "CadiBack: Extracting Backbones with CaDiCaL", SAT 2023, following the
 * reference implementation https://github.com/arminbiere/cadiback (cadiback.cpp) when both differ:
 * 1. Solve once. The candidates are the literals of the model that are not flippable.
 * 2. Until no candidate is left: move root-level fixed candidates to the backbone (drop the ones fixed false), then
 *    solve under the single clause constraint (CaDiCaL 'constrain') made of the negations of a chunk of candidates.
 *    SAT: drop the candidates falsified or flippable in the new model. UNSAT: the whole chunk is backbone.
 *
 * The loop of solve() is fixed; variants of the search derive from this class (one file per variant in this
 * directory) and override the protected hooks: initCandidates, selectChunk, onSat, onUnsat. The defaults implement
 * the algorithm of the paper, see CadiBack for the baseline. A variant must keep the invariants documented on each
 * hook, since what a solver exports (learnt clauses, backbone units) must stay implied by the formula alone.
 *
 * Learnt clauses are exported and imported through the Painless hooks of the vendored CaDiCaL (Learner interface).
 * Constraints act as assumptions, so every learnt clause is a consequence of the formula and can be shared.
 */
class CadiBackBase
	: public BackboneSolverInterface
	, public CaDiCaL::Learner
	, public CaDiCaL::Terminator
{
  public:
	CadiBackBase(int id, const std::shared_ptr<ClauseDatabase>& clauseDB, BackboneSolverType type);

	~CadiBackBase() override;

	/// @brief Short name of the variant, used in the statistics and the winning log.
	virtual const char* variantName() const = 0;

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

	/// @brief A map from a CaDiCaL option name to its value.
	std::unordered_map<std::string, int> cadicalOptions;

  protected:
	/* Hooks of the search, the defaults implement the algorithm of the paper */

	/// @brief Called once on the initial candidates, after the first model filtered them and -bb-order sorted them.
	/// Default: nothing.
	/// @details May reorder (any order is sound) and set up the state of the variant, but every candidate must stay.
	virtual void initCandidates(std::vector<int>& candidates);

	/// @brief Fills @p chunk (given empty) with the candidates whose negations form the next constraint. Default: the
	/// first min(@ref m_chunkSize, |candidates|) ones.
	/// @details @p candidates is not empty and has no fixed literal. The chunk must be a non-empty subset of it.
	virtual void selectChunk(const std::vector<int>& candidates, std::vector<int>& chunk);

	/// @brief Called after a SAT answer under the constraint of @p chunk. Default: filterWithModel, and with chunking
	/// resets @ref m_chunkSize to 1.
	/// @details Must only drop candidates refuted by the current model (a model of the formula).
	virtual void onSat(std::vector<int>& candidates, const std::vector<int>& chunk);

	/// @brief Called after an UNSAT answer under the constraint of @p chunk. Default: every chunk literal goes through
	/// commitBackbone and leaves the candidates, and with chunking @ref m_chunkSize is multiplied by K.
	/// @details UNSAT proves that every literal of the chunk is backbone: they must all leave the candidates.
	virtual void onUnsat(std::vector<int>& candidates, const std::vector<int>& chunk);

	/* Helpers for the hooks */

	/// @brief Moves the root-level fixed candidates to the backbone, drops the ones fixed to false.
	void extractFixed(std::vector<int>& candidates);

	/// @brief Keeps only the candidates true and (unless disabled) not flippable in the current model. With
	/// -bb-share-cand, publishes the variables of the dropped candidates as free on the candidate board.
	void filterWithModel(std::vector<int>& candidates);

	/// @brief Records a backbone literal proven by an UNSAT answer and adds it to CaDiCaL as a unit clause.
	void commitBackbone(int lit);

	/// @brief Removes the literals of @p lits from @p candidates, keeping the order of the others.
	void removeCandidates(std::vector<int>& candidates, const std::vector<int>& lits);

	/// @brief Chunk rate K (-bb-chunk) read at the start of solve(): 0 = all candidates, 1 = one-by-one, K>1 = growth.
	unsigned long m_chunkRate = 0;

	/// @brief Size of the next chunk for the default selectChunk (SIZE_MAX when the chunk holds all candidates).
	size_t m_chunkSize = 0;

	/// @brief Occurrences of each literal in the initial clauses (empty when loaded with loadFormula), for -bb-order.
	LiteralOccurrences m_occurrences;

	/// @brief Seed given by diversify, used by the random candidate order.
	unsigned int m_seed = 0;

  private:
	/// @brief Sets @ref cadicalOptions to the default configuration and applies it.
	void initCadicalOptions();

	/// @brief Applies @ref cadicalOptions to the solver.
	void applyCadicalOptions();

	/// @brief Solve under the clause 'constraint' (empty: no constraint). Maps CaDiCaL answers to 10/20/0.
	int solveUnderConstraint(const std::vector<int>& constraint);

	/// @brief Adds a proven backbone literal to @ref m_backbone, exports it as a unit clause to the sharing clients
	/// (only if enabled for this worker by -bb-share-units) and publishes it on the candidate board (-bb-share-cand).
	void addBackboneLiteral(int lit);

	/// @brief With -bb-share-cand consume: drops the candidates whose variable another solver published as free and
	/// moves the ones it published as backbone to @ref m_backbone (also added to CaDiCaL as units). Called by the loop
	/// of solve() before each chunk, so every variant benefits from it.
	void syncWithBoard(std::vector<int>& candidates);

  protected:
	std::unique_ptr<CaDiCaL::Solver> solver;

	/// @brief Used to stop or continue the resolution (read by CaDiCaL through terminate()).
	std::atomic<bool> stopSolver;

	unsigned int m_nbVars = 0;

	std::vector<int> m_backbone;

	/* Statistics */
	unsigned long m_satCalls = 0;
	unsigned long m_satAnswers = 0;
	unsigned long m_unsatAnswers = 0;
	unsigned long m_fixedFound = 0;
	unsigned long m_boardFree = 0;	   ///< candidates dropped because the board says free
	unsigned long m_boardBackbone = 0; ///< backbone literals taken from the board

	/*----------------------Learner------------------------*/
	/// @details The Learner methods are called by CaDiCaL from the solving thread only
  public:
	/// @brief Tells if CaDiCaL should export a clause of the given size and glue
	bool learning(int size, int glue) override;

	/// @brief Receives the literals of the clause to export, zero terminated
	void learn(int lit) override;

	/// @brief Tells CaDiCaL if a clause is waiting to be imported
	bool hasClauseToImport() override;

	/// @brief Gives CaDiCaL the clause to import
	void getClauseToImport(std::vector<int>& clause, int& glue) override;

  private:
	/// @brief Clause being exported
	simpleClause tempClause;

	/// @brief Glue of the clause being exported
	int lbd;

	/// @brief Next clause to import (loaded in hasClauseToImport)
	ClauseExchangePtr tempClauseToImport;

	/*-----------------------Terminator----------------------*/
	bool terminate() override { return this->stopSolver; }
};
