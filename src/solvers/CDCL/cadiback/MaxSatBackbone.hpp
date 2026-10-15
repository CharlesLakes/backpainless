#pragma once

#include "solvers/CDCL/cadiback/CadiBackBase.hpp"

#include <atomic>
#include <chrono>
#include <vector>

class MaxSatSatSolver;

/**
 * @brief Variant: rounds of MaxSAT (EvalMaxSAT) instead of the clause constraint of CadiBack.
 * @ingroup solving
 *
 * The constraint of CadiBack (-l1 v ... v -lk) asks for a model that flips at least one candidate. Here every negated
 * candidate is a soft unit and the formula is hard: an optimum of the MaxSAT problem flips as many candidates as
 * possible at once. Each round solves it on the candidates left by the previous one, until a round whose optimum flips
 * no candidate, which proves that all of them are backbone (no model falsifies any of them). Each round either drops a
 * candidate or ends, so the search is complete. Every model met by EvalMaxSAT on the way (not only the optimum) also
 * drops candidates (filterWithModel), and a core made of a single soft literal -l proves the backbone literal l.
 *
 * EvalMaxSAT runs on the CaDiCaL of CadiBackBase through MaxSatSatSolver (MaxSatBackbone.cpp), so the learnt clauses
 * are exported and imported by the usual Learner, the backbone literals as units and the board as in CadiBack. The
 * soft literals are only assumed; the cardinality constraints of EvalMaxSAT use fresh variables (above m_nbVars) and
 * only imply them from the original ones (a conservative extension, any model of the formula extends to them), its
 * other hard clauses are implied by unit propagation, and hardening is disabled (incremental mode, unweighted
 * anyway): the backbone stays the one of the formula and the Learner keeps dropping the clauses with a fresh variable.
 *
 * With -bb-maxsat-rounds=R > 0 the exploration stops after R rounds and the CadiBack loop finishes the candidates.
 */
class MaxSatBackbone : public CadiBackBase
{
	friend class MaxSatSatSolver;

  public:
	MaxSatBackbone(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	const char* variantName() const override { return "MaxSatBackbone"; }

	void printStatistics() override;

	void printWinningLog() override;

  protected:
	/// @brief The MaxSAT rounds (after the XOR exploration when -bb-xor-pre enables it).
	BackboneResult explore(std::vector<int>& candidates) override;

  private:
	/// @brief One EvalMaxSAT call over the soft literals -l of the candidates. Sets @p stagnated when its optimum
	/// flips no candidate. Throws MaxSatInterrupted (caught by explore) when the worker is interrupted.
	BackboneResult maxSatRound(std::vector<int>& candidates, bool& stagnated);

	/* Called by MaxSatSatSolver from the solving thread, inside EvalMaxSAT */

	/// @brief After every SAT answer: the model, restricted to the formula variables, is a model of the formula.
	void onMaxSatModel();

	/// @brief After an UNSAT answer whose only failed assumption is -lit (lit of the formula): lit is backbone.
	void onMaxSatUnitCore(int lit);

	/// @brief After every answer: true when the running round should be abandoned (no candidate left that the optimum
	/// could drop, or -bb-maxsat-patience calls without a drop in a round that already dropped candidates).
	bool roundExhausted();

	/// @brief Candidates of the running round (null outside of a round).
	std::vector<int>* m_roundCandidates = nullptr;

	/// @brief Per variable: soft literal of the running round not proven yet (1), proven by a unit core (2), else 0.
	std::vector<char> m_softState;

	/// @brief Unit cores of the running round, removed from the candidates at the end of the round.
	unsigned long m_coresInRound = 0;

	/// @brief Candidates dropped by the models of the running round, and SAT calls since the last drop.
	unsigned long m_droppedInRound = 0;
	unsigned long m_callsWithoutDrop = 0;

	/* Statistics */
	unsigned long m_roundsDone = 0;
	unsigned long m_abandonedRounds = 0;
	unsigned long m_maxSatCalls = 0;	  ///< SAT calls made by EvalMaxSAT
	unsigned long m_maxSatModels = 0;	  ///< SAT answers among them
	unsigned long m_droppedByModels = 0;  ///< candidates dropped by the models met by EvalMaxSAT
	unsigned long m_unitCores = 0;		  ///< backbone literals proven by a core of a single soft literal
	unsigned long m_stagnationProof = 0;  ///< backbone literals proven by the optimum of the last round
	double m_maxSatSeconds = 0;
	double m_satSeconds = 0; ///< part of m_maxSatSeconds spent in the CaDiCaL calls
	unsigned long m_propagateCalls = 0; ///< unit propagations asked by the at-most-one detection of EvalMaxSAT
	double m_propagateSeconds = 0;
	/// @brief Start of the running exploration (the statistics can be printed while it runs, when another worker wins)
	std::chrono::steady_clock::time_point m_maxSatStart;
	std::atomic<bool> m_maxSatRunning{ false }; /* read by the statistics, possibly from another thread */

	/// @brief Seconds spent in the MaxSAT rounds, including the running one.
	double maxSatSeconds() const;
};
