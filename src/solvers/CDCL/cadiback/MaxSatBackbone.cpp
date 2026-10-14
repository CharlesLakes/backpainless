#include "solvers/CDCL/cadiback/MaxSatBackbone.hpp"

#include "utils/Logger.hpp"

#include "evalmaxsat/lib/EvalMaxSAT/src/EvalMaxSAT.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <set>
#include <unordered_map>

/* Thrown by MaxSatSatSolver when the worker is interrupted: EvalMaxSAT has no interruption and retries an unknown
 * answer, so the exception unwinds it up to MaxSatBackbone::explore */
struct MaxSatInterrupted
{};

/* Thrown by MaxSatSatSolver when the running round has nothing more to give (no candidate left, or out of patience
 * after it dropped some): the next round starts on the candidates left */
struct MaxSatRoundAbandoned
{};

/**
 * @brief The SAT solver interface expected by EvalMaxSAT (the one of its cadicalinterface.h), on the CaDiCaL of a
 * MaxSatBackbone worker.
 *
 * The variables 1..m_nbVars of EvalMaxSAT are the ones of the formula, its other variables (cardinality constraints)
 * are mapped to fresh CaDiCaL variables (CadiBackBase::freshVariable), so that several rounds (one EvalMaxSAT each) can
 * share the same CaDiCaL and its learnt clauses. Every answer is reported to the worker: models drop candidates, a
 * core made of a single soft literal proves a backbone literal.
 */
class MaxSatSatSolver
{
  public:
	explicit MaxSatSatSolver(MaxSatBackbone& owner)
		: m_owner(owner)
		, m_solver(*owner.solver)
		, m_nbVars((int)owner.m_nbVars)
	{
	}

	int newVar(bool /* decisionVar */ = true)
	{
		++m_nVar;
		if (m_nVar > m_nbVars) {
			int var = m_owner.freshVariable();
			m_freshToCadical.push_back(var);
			m_cadicalToFresh.emplace(var, m_nVar);
		}
		return m_nVar;
	}

	unsigned int nVars() { return (unsigned int)m_nVar; }

	void addClause(const std::vector<int>& clause)
	{
		for (int lit : clause) {
			while (std::abs(lit) > m_nVar)
				newVar();
			m_solver.add(toCadical(lit));
		}
		m_solver.add(0);
	}

	void simplify() {}

	bool solve() { return solveUntilAnswer({}) == 1; }

	bool solve(const std::vector<bool>& solution)
	{
		std::vector<int> assumptions;
		for (int var = 1; var < (int)solution.size(); var++)
			assumptions.push_back(toCadical(solution[var] ? var : -var));
		return solveUntilAnswer(assumptions) == 1;
	}

	template<class T>
	bool solve(const T& assumption)
	{
		return solveUntilAnswer(toCadical(assumption, 0)) == 1;
	}

	template<class T>
	bool solve(const T& assumption, const std::set<int>& forced)
	{
		std::vector<int> assumptions;
		for (int lit : forced)
			assumptions.push_back(toCadical(lit));
		for (int lit : assumption)
			if (forced.count(-lit) == 0)
				assumptions.push_back(toCadical(lit));
		return solveUntilAnswer(assumptions) == 1;
	}

	/// @return 1 SAT, 0 UNSAT, -1 budget exhausted (as cadicalinterface.h)
	template<class T>
	int solveLimited(const T& assumption, int confBudget, int except = 0)
	{
		return run(toCadical(assumption, except), confBudget, -1);
	}

	template<class T>
	int solveWithTimeout(const T& assumption, double timeoutSec, int except = 0)
	{
		return run(toCadical(assumption, except), -1, timeoutSec);
	}

	template<class T>
	int solveWithTimeoutAndLimit(const T& assumption, double timeoutSec, int confBudget, int except = 0)
	{
		return run(toCadical(assumption, except), confBudget, timeoutSec);
	}

	bool getValue(int lit) { return m_solver.val(toCadical(lit)) > 0; }

	std::vector<bool> getSolution()
	{
		std::vector<bool> solution(m_nVar + 1, false);
		for (int var = 1; var <= m_nVar; var++)
			solution[var] = m_solver.val(toCadical(var)) > 0;
		return solution;
	}

	/// @brief The literals of @p assumptions that failed in the last UNSAT answer (cached: the worker may add clauses
	/// to CaDiCaL right after the answer, which ends the UNSAT state).
	template<class T>
	std::vector<int> getConflict(const T& assumptions)
	{
		std::vector<int> conflict;
		for (int lit : assumptions)
			if (std::find(m_failed.begin(), m_failed.end(), toCadical(lit)) != m_failed.end())
				conflict.push_back(lit);
		return conflict;
	}

	bool propagate(const std::vector<int>& assumptions, std::vector<int>& implied)
	{
		std::vector<int> cadicalImplied;
		const auto start = std::chrono::steady_clock::now();
		bool ok = m_solver.find_up_implicants(toCadical(assumptions, 0), cadicalImplied);
		m_owner.m_propagateCalls++;
		m_owner.m_propagateSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		for (int lit : cadicalImplied) {
			int emsLit = fromCadical(lit);
			if (emsLit)
				implied.push_back(emsLit);
		}
		return ok;
	}

  private:
	int toCadical(int lit) const
	{
		const int var = std::abs(lit);
		if (var <= m_nbVars)
			return lit;
		assert(var <= m_nVar);
		const int mapped = m_freshToCadical[var - m_nbVars - 1];
		return lit < 0 ? -mapped : mapped;
	}

	template<class T>
	std::vector<int> toCadical(const T& lits, int except) const
	{
		std::vector<int> mapped;
		for (int lit : lits)
			if (lit != except)
				mapped.push_back(toCadical(lit));
		return mapped;
	}

	/// @brief EvalMaxSAT literal of a CaDiCaL literal, 0 for a variable of another round (or of -bb-xor-pre).
	int fromCadical(int lit) const
	{
		const int var = std::abs(lit);
		if (var <= m_nbVars)
			return lit;
		auto it = m_cadicalToFresh.find(var);
		if (it == m_cadicalToFresh.end())
			return 0;
		return lit < 0 ? -it->second : it->second;
	}

	/// @brief A call without budget: an unknown answer is retried (as cadicalinterface.h), except on an interrupt.
	int solveUntilAnswer(const std::vector<int>& assumptions)
	{
		int res;
		do
			res = run(assumptions, -1, -1);
		while (res < 0);
		return res;
	}

	/// @brief One CaDiCaL call. @p conflicts < 0: no conflict limit, @p seconds < 0: no deadline.
	int run(const std::vector<int>& assumptions, int conflicts, double seconds)
	{
		if (m_owner.stopSolver)
			throw MaxSatInterrupted{};

		for (int lit : assumptions)
			m_solver.assume(lit);
		if (conflicts >= 0)
			m_solver.limit("conflicts", conflicts);
		if (seconds >= 0)
			m_owner.setSolveDeadline(seconds);

		m_owner.m_maxSatCalls++;
		m_owner.m_satCalls++;
		const auto start = std::chrono::steady_clock::now();
		const int res = m_solver.solve();
		m_owner.m_satSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		m_owner.clearSolveDeadline();

		if (m_owner.stopSolver)
			throw MaxSatInterrupted{};

		m_failed.clear();
		int answer = -1;
		if (res == 10) {
			m_owner.onMaxSatModel();
			answer = 1;
		} else if (res == 20) {
			for (int lit : assumptions)
				if (m_solver.failed(lit))
					m_failed.push_back(lit);
			/* No other assumption is involved: the formula (with definitions only) implies the negation */
			if (m_failed.size() == 1 && std::abs(m_failed[0]) <= m_nbVars)
				m_owner.onMaxSatUnitCore(-m_failed[0]);
			answer = 0;
		}
		if (m_owner.roundExhausted())
			throw MaxSatRoundAbandoned{};
		return answer;
	}

	MaxSatBackbone& m_owner;
	CaDiCaL::Solver& m_solver;
	const int m_nbVars;
	int m_nVar = 0;
	std::vector<int> m_freshToCadical; ///< EvalMaxSAT variable m_nbVars + 1 + i -> CaDiCaL variable
	std::unordered_map<int, int> m_cadicalToFresh;
	std::vector<int> m_failed; ///< failed assumptions (CaDiCaL literals) of the last UNSAT answer
};

/*----------------------Main Class------------------------*/

MaxSatBackbone::MaxSatBackbone(int id, const std::shared_ptr<ClauseDatabase>& clauseDB)
	: CadiBackBase(id, clauseDB, BackboneSolverType::MAXSAT)
{
	initializeTypeId<MaxSatBackbone>();
}

BackboneResult
MaxSatBackbone::explore(std::vector<int>& candidates)
{
	BackboneResult result = CadiBackBase::explore(candidates);
	if (result != BackboneResult::COMPLETE)
		return result;

	m_maxSatStart = std::chrono::steady_clock::now();
	m_maxSatRunning = true;
	/* EvalMaxSAT shuffles with a generator per thread, seeded here since diversify runs in another thread */
	unsigned int seed = m_seed;
	MaLib::MonRand::seed(seed);

	try {
		for (unsigned long round = 0; (m_maxSatRounds == 0 || round < m_maxSatRounds) && !candidates.empty();
			 round++) {
			if (stopSolver) {
				result = BackboneResult::UNKNOWN;
				break;
			}
			syncWithBoard(candidates);
			extractFixed(candidates);
			if (candidates.empty())
				break;
			bool stagnated = false;
			result = maxSatRound(candidates, stagnated);
			if (result != BackboneResult::COMPLETE || stagnated)
				break;
		}
	} catch (const MaxSatInterrupted&) {
		m_roundCandidates = nullptr;
		clearSolveDeadline();
		result = BackboneResult::UNKNOWN;
	}

	m_maxSatSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - m_maxSatStart).count();
	m_maxSatRunning = false;
	LOG1("%s %d: MaxSAT exploration done in %.2f s, %lu rounds, %zu candidates left",
		 variantName(),
		 this->getSolverId(),
		 m_maxSatSeconds,
		 m_roundsDone,
		 candidates.size());
	return result;
}

BackboneResult
MaxSatBackbone::maxSatRound(std::vector<int>& candidates, bool& stagnated)
{
	const size_t softCount = candidates.size();
	m_roundsDone++;

	MaxSatSatSolver satSolver(*this);
	EvalMaxSAT<MaxSatSatSolver> maxSat(&satSolver);
	/* No hardening (it adds the soft literals proven by the bound as hard units, not implied by the formula) */
	maxSat.setIncremental(true);
	for (unsigned int var = 1; var <= m_nbVars; var++)
		maxSat.newVar();

	m_softState.assign(m_nbVars + 1, 0);
	for (int lit : candidates) {
		maxSat.addClause({ -lit }, 1);
		m_softState[std::abs(lit)] = 1;
	}
	m_coresInRound = 0;
	m_droppedInRound = 0;
	m_roundCandidates = &candidates;

	m_callsWithoutDrop = 0;
	bool abandoned = false;
	bool solved = true;
	try {
		solved = maxSat.solve();
	} catch (const MaxSatRoundAbandoned&) {
		abandoned = true;
		m_abandonedRounds++;
	}
	m_roundCandidates = nullptr;

	/* Backbone literals proven by unit cores leave the candidates (already committed) */
	if (m_coresInRound > 0)
		candidates.erase(std::remove_if(candidates.begin(),
										candidates.end(),
										[this](int lit) { return m_softState[std::abs(lit)] == 2; }),
						 candidates.end());

	if (abandoned) {
		LOG1("%s %d: MaxSAT round %lu abandoned, %zu soft literals, %zu candidates left",
			 variantName(),
			 this->getSolverId(),
			 m_roundsDone,
			 softCount,
			 candidates.size());
		return BackboneResult::COMPLETE;
	}

	if (!solved) {
		/* The first call found a model of the formula, and EvalMaxSAT only adds definitions */
		LOGERROR("%s %d: EvalMaxSAT answered UNSAT on a satisfiable formula", variantName(), this->getSolverId());
		return BackboneResult::UNKNOWN;
	}

	const t_weight cost = maxSat.getCost();
	LOG1("%s %d: MaxSAT round %lu, %zu soft literals, optimum cost %llu, %zu candidates left",
		 variantName(),
		 this->getSolverId(),
		 m_roundsDone,
		 softCount,
		 cost,
		 candidates.size());

	if (cost == softCount) {
		/* No model falsifies a candidate of the round, not even the one that falsifies the most: all backbone */
		for (int lit : candidates)
			commitBackbone(lit);
		m_stagnationProof += candidates.size();
		candidates.clear();
		stagnated = true;
	} else if (candidates.size() + m_coresInRound == softCount) {
		/* Cannot happen (the optimum falsifies a candidate and was seen by onMaxSatModel): leave it to CadiBack */
		LOGWARN("%s %d: MaxSAT round dropped no candidate, CadiBack finishes", variantName(), this->getSolverId());
		stagnated = true;
	}
	return BackboneResult::COMPLETE;
}

void
MaxSatBackbone::onMaxSatModel()
{
	m_maxSatModels++;
	if (!m_roundCandidates)
		return;
	const size_t before = m_roundCandidates->size();
	filterWithModel(*m_roundCandidates);
	const size_t dropped = before - m_roundCandidates->size();
	m_droppedByModels += dropped;
	m_droppedInRound += dropped;
	if (dropped)
		m_callsWithoutDrop = 0;
}

bool
MaxSatBackbone::roundExhausted()
{
	if (!m_roundCandidates)
		return false;
	m_callsWithoutDrop++;
	/* Only the candidates proven by unit cores are left: the optimum would not drop anything more */
	if (m_roundCandidates->size() == m_coresInRound)
		return true;
	/* A round that dropped nothing is never abandoned: its optimum is the proof that ends the search */
	return m_maxSatPatience > 0 && m_droppedInRound > 0 && m_callsWithoutDrop >= m_maxSatPatience;
}

void
MaxSatBackbone::onMaxSatUnitCore(int lit)
{
	const unsigned int var = std::abs(lit);
	/* Only a soft literal of the round (the negation of a candidate) not proven yet; a fresh soft literal of EvalMaxSAT
	 * (cardinality output) has a variable above m_nbVars and never gets here */
	if (!m_roundCandidates || m_softState[var] != 1)
		return;
	m_softState[var] = 2;
	m_coresInRound++;
	m_unitCores++;
	commitBackbone(lit);
	LOGDEBUG2("%s %d: unit core proves backbone literal %d", variantName(), this->getSolverId(), lit);
}

double
MaxSatBackbone::maxSatSeconds() const
{
	double running =
		m_maxSatRunning ? std::chrono::duration<double>(std::chrono::steady_clock::now() - m_maxSatStart).count()
						: 0.0;
	return m_maxSatSeconds + running;
}

void
MaxSatBackbone::printStatistics()
{
	CadiBackBase::printStatistics();
	std::cout << "c|   maxsat, solver " << this->getSolverId() << " (rounds limit " << m_maxSatRounds
			  << ", patience " << m_maxSatPatience << "): " << m_roundsDone << " rounds (" << m_abandonedRounds
			  << " abandoned), " << m_maxSatCalls << " SAT calls (" << m_maxSatModels
			  << " models), " << m_droppedByModels << " candidates dropped by models, " << m_unitCores
			  << " backbone by unit cores, " << m_stagnationProof << " by the last optimum, " << maxSatSeconds() << " s ("
			  << m_satSeconds << " s in CaDiCaL, " << m_propagateCalls << " propagations in " << m_propagateSeconds
			  << " s)"
			  << (m_maxSatRunning ? " (still exploring)" : "") << "\n";
}

void
MaxSatBackbone::printWinningLog()
{
	CadiBackBase::printWinningLog();
	LOGSTAT("MaxSAT exploration: %lu rounds (%lu abandoned), %lu SAT calls (%lu models), %lu candidates dropped by models, %lu backbone "
			"literals by unit cores, %lu by the last optimum, %.2f s",
			m_roundsDone,
			m_abandonedRounds,
			m_maxSatCalls,
			m_maxSatModels,
			m_droppedByModels,
			m_unitCores,
			m_stagnationProof,
			maxSatSeconds());
}
