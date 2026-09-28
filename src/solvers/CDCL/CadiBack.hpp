#pragma once

#include "solvers/BackboneSolverInterface.hpp"

#include "cadical/src/cadical.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Backbone solver implementing the CadiBack algorithm on top of CaDiCaL.
 * @ingroup solving
 *
 * Algorithm 1 of Biere, Froleyks, Wang, "CadiBack: Extracting Backbones with CaDiCaL", SAT 2023, following the
 * reference implementation https://github.com/arminbiere/cadiback (cadiback.cpp) when both differ:
 * 1. Solve once. The candidates are the literals of the model that are not flippable.
 * 2. Until no candidate is left: move root-level fixed candidates to the backbone (drop the ones fixed false), then
 *    solve under the single clause constraint (CaDiCaL 'constrain') made of the negations of a chunk of candidates.
 *    SAT: drop the candidates falsified or flippable in the new model. UNSAT: the whole chunk is backbone.
 *
 * Learnt clauses are exported and imported through the Painless hooks of the vendored CaDiCaL (Learner interface).
 * Constraints act as assumptions, so every learnt clause is a consequence of the formula and can be shared.
 */
class CadiBack
	: public BackboneSolverInterface
	, public CaDiCaL::Learner
	, public CaDiCaL::Terminator
{
  public:
	CadiBack(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	~CadiBack() override;

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

  private:
	/// @brief Sets @ref cadicalOptions to the default configuration and applies it.
	void initCadicalOptions();

	/// @brief Applies @ref cadicalOptions to the solver.
	void applyCadicalOptions();

	/// @brief Solve under the clause 'constraint' (empty: no constraint). Maps CaDiCaL answers to 10/20/0.
	int solveUnderConstraint(const std::vector<int>& constraint);

	/// @brief Moves the root-level fixed candidates to the backbone, drops the ones fixed to false.
	void extractFixed(std::vector<int>& candidates);

	/// @brief Keeps only the candidates true and (unless disabled) not flippable in the current model.
	void filterWithModel(std::vector<int>& candidates);

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
