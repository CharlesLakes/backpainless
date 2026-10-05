#include "solvers/CDCL/cryptominisat/DiverseBackboneSearch.hpp"
#include "solvers/XorSampler.hpp"

#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/Parameters.hpp"
#include "utils/Parsers.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {
double
seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Conflicts of one call when no budget is given: the call is repeated until an answer. CryptoMiniSat resets its
/// interrupt flag at the start of a call, so an interrupt arriving just before it only delays the stop by one slice.
constexpr unsigned long UNBOUNDED_SLICE = 100'000;

/// Conflicts between two exports of the learnt clauses (an export walks the whole learnt database).
constexpr uint64_t EXPORT_INTERVAL = 1000;

/// Glue bounds of the export passes: CryptoMiniSat does not give the glue of a clause, a clause found by the pass
/// with bound g is exported with lbd min(g, size).
constexpr uint32_t EXPORT_GLUES[] = { 2, 6 };

uint64_t
hashClause(const std::vector<int>& sortedClause)
{
	/* splitmix64 combination: a collision only skips the export of a clause */
	uint64_t hash = 0x9e3779b97f4a7c15ULL ^ sortedClause.size();
	for (int lit : sortedClause) {
		uint64_t x = hash ^ (uint64_t)(uint32_t)lit;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
		hash = x ^ (x >> 31);
	}
	return hash;
}

}

DiverseBackboneSearch::DiverseBackboneSearch(int id, const std::shared_ptr<ClauseDatabase>& clauseDB)
	: BackboneSolverInterface(id, clauseDB, BackboneSolverType::DIVERSE_XOR)
	, m_stop(false)
{
	m_solver = std::make_unique<CMSat::SATSolver>();
	m_solver->set_verbosity(0);
	m_solver->set_num_threads(1);
	/* BVA variables are hidden from the outside numbering: the clauses using them could not be exported */
	m_solver->set_no_bva();
	initializeTypeId<DiverseBackboneSearch>();
}

DiverseBackboneSearch::~DiverseBackboneSearch() {}

/* Search */

int
DiverseBackboneSearch::solveWith(const std::vector<CMSat::Lit>& assumptions, unsigned long conflicts)
{
	m_satCalls++;
	int result = 0;
	do {
		if (m_stop)
			return 0;
		double t0 = seconds();
		importPendingClauses();
		double t1 = seconds();
		m_solver->set_max_confl(conflicts ? conflicts : UNBOUNDED_SLICE);
		CMSat::lbool answer = m_solver->solve(&assumptions);
		double t2 = seconds();
		exportLearntClauses();
		m_timeImport += t1 - t0;
		m_timeSolve += t2 - t1;
		m_timeExport += seconds() - t2;
		result = (answer == CMSat::l_True) ? 10 : (answer == CMSat::l_False) ? 20 : 0;
	} while (result == 0 && conflicts == 0);
	return result;
}

BackboneResult
DiverseBackboneSearch::solve(const std::vector<int>& cube)
{
	if (!this->isInitialized()) {
		LOGWARN("DiverseBackboneSearch %d was not initialized to be launched!", this->getSolverId());
		return BackboneResult::UNKNOWN;
	}
	if (!cube.empty())
		LOGWARN("DiverseBackboneSearch %d ignores the given cube of size %zu", this->getSolverId(), cube.size());

	m_backbone.clear();
	m_model.assign(m_nbVars + 1, 0);
	m_notBackbone.assign(2 * (m_nbVars + 1), 0);
	m_free.assign(m_nbVars + 1, 0);
	m_fixed.assign(m_nbVars + 1, 0);
	m_unitExported.assign(m_nbVars + 1, 0);

	/* (res, sigma) <- SAT(phi) */
	int res = solveWith({}, 0);
	if (res == 20) {
		LOGSTAT("DiverseBackboneSearch %d: formula is UNSATISFIABLE", this->getSolverId());
		return BackboneResult::UNSAT;
	}
	if (res != 10)
		return BackboneResult::UNKNOWN;
	m_satAnswers++;

	/* A relative budget follows the hardness of the instance: a node with fewer constraints than the first call can
	 * still need as many conflicts to find a model */
	const XorBudget& budget = m_xorOptions.budget;
	m_nodeBudget = (budget.factor > 0.0)
					   ? std::max<unsigned long>(MIN_XOR_BUDGET, budget.factor * m_solver->get_sum_conflicts())
					   : budget.conflicts;

	/* The candidates are the literals of the first model, filterWithModel drops the flippable ones */
	std::vector<int> candidates;
	candidates.reserve(m_nbVars);
	const auto& model = m_solver->get_model();
	for (int var = 1; var <= (int)m_nbVars; var++)
		candidates.push_back((model[var - 1] == CMSat::l_False) ? -var : var);
	filterWithModel(candidates);
	sortCandidates(candidates, m_candidateOrder, m_occurrences, m_seed);

	LOG1("DiverseBackboneSearch %d: %zu initial candidates out of %u variables, xor m=%u %s, %lu rounds, node budget "
		 "%lu conflicts",
		 this->getSolverId(),
		 candidates.size(),
		 m_nbVars,
		 m_xorOptions.count,
		 xorDensityName(m_xorOptions.density).c_str(),
		 m_xorOptions.rounds,
		 m_nodeBudget);

	/* Exploration: random XOR splits of the search space */
	for (unsigned long round = 0; round < m_xorOptions.rounds && !candidates.empty(); round++) {
		if (m_stop)
			return BackboneResult::UNKNOWN;
		syncWithBoard(candidates);
		extractFixed(candidates);
		if (candidates.empty())
			break;
		BackboneResult result = exploreRound(candidates);
		if (result != BackboneResult::COMPLETE)
			return result;
	}

	/* Completion: the CadiBack loop on the remaining candidates */
	BackboneResult result = completeBackbone(candidates);
	if (result != BackboneResult::COMPLETE)
		return result;

	std::sort(m_backbone.begin(), m_backbone.end(), [](int a, int b) { return std::abs(a) < std::abs(b); });

	LOGSTAT("DiverseBackboneSearch %d: formula is SATISFIABLE, backbone size %zu / %u variables (%lu SAT calls)",
			this->getSolverId(),
			m_backbone.size(),
			m_nbVars,
			m_satCalls);

	return BackboneResult::COMPLETE;
}

BackboneResult
DiverseBackboneSearch::exploreRound(std::vector<int>& candidates)
{
	std::vector<int> pool;
	pool.reserve(candidates.size());
	for (int lit : candidates)
		pool.push_back(std::abs(lit));

	std::vector<std::vector<int>> xors;
	m_xorsDependent += drawRandomXors(pool, m_xorOptions.count, m_xorOptions.density, m_rng, xors);
	if (xors.empty())
		return BackboneResult::COMPLETE;

	/* XOR j: x_1 ^ ... ^ x_k ^ s_j = 0 defines the fresh selector s_j = parity, assuming s_j = b means parity = b */
	const unsigned int m = xors.size();
	std::vector<uint32_t> selectors(m);
	std::vector<uint32_t> vars;
	for (unsigned int j = 0; j < m; j++) {
		m_solver->new_var();
		selectors[j] = m_solver->nVars() - 1;
		vars.clear();
		for (int var : xors[j])
			vars.push_back(var - 1);
		vars.push_back(selectors[j]);
		m_solver->add_xor_clause(vars, false);
	}
	m_xorRounds++;
	m_xorsAdded += m;

	LOG2("DiverseBackboneSearch %d: round %lu, %u XORs over %zu candidate variables",
		 this->getSolverId(),
		 m_xorRounds,
		 m,
		 pool.size());

	const unsigned long freeBefore = m_freeVariables;
	bool formulaUnsat = false;
	std::vector<CMSat::Lit> assumptions;
	assumptions.reserve(m);
	auto solveNode = [&](unsigned int depth, uint64_t mask) -> XorNodeResult {
		if (m_stop || candidates.empty())
			return { XorNodeAnswer::STOP };
		assumptions.clear();
		for (unsigned int j = 0; j < depth; j++)
			assumptions.push_back(CMSat::Lit(selectors[j], !xorRhs(mask, m, j)));

		int res = solveWith(assumptions, m_nodeBudget);
		if (res == 10) {
			m_satAnswers++;
			const unsigned long freeBeforeNode = m_freeVariables;
			filterWithModel(candidates);
			m_freeByXor += m_freeVariables - freeBeforeNode;
			return { XorNodeAnswer::SAT };
		}
		if (res == 20) {
			m_unsatAnswers++;
			/* The conflict holds the negations of the assumptions it uses: the path down to its deepest XOR has no
			 * model, every node below it is pruned */
			const auto& conflict = m_solver->get_conflict();
			if (conflict.empty()) {
				formulaUnsat = true;
				return { XorNodeAnswer::STOP };
			}
			unsigned int deepest = 0;
			for (CMSat::Lit lit : conflict) {
				assert(lit.var() >= selectors[0] && lit.var() < selectors[0] + depth);
				deepest = std::max(deepest, lit.var() - selectors[0]);
			}
			return { XorNodeAnswer::UNSAT, deepest };
		}
		return { m_stop ? XorNodeAnswer::STOP : XorNodeAnswer::UNKNOWN };
	};
	xorDfs(m, m_xorOptions.adaptive, m_xorOptions.leaves, solveNode, m_dfsStats);

	if (formulaUnsat) {
		LOGSTAT("DiverseBackboneSearch %d: formula is UNSATISFIABLE", this->getSolverId());
		return BackboneResult::UNSAT;
	}
	if (m_stop)
		return BackboneResult::UNKNOWN;

	LOG1("DiverseBackboneSearch %d: round %lu done, %lu free variables found, %zu candidates left",
		 this->getSolverId(),
		 m_xorRounds,
		 m_freeVariables - freeBefore,
		 candidates.size());
	return BackboneResult::COMPLETE;
}

BackboneResult
DiverseBackboneSearch::completeBackbone(std::vector<int>& candidates)
{
	/* Same chunk policy as CadiBackBase: K = 0 all candidates, K > 0 reset to 1 after SAT, times K after UNSAT */
	const size_t infinite = std::numeric_limits<size_t>::max();
	size_t chunkSize = (m_chunkRate == 0) ? infinite : 1;
	std::vector<CMSat::Lit> clause, assumption(1);
	std::vector<int> chunk;

	while (!candidates.empty()) {
		if (m_stop)
			return BackboneResult::UNKNOWN;

		syncWithBoard(candidates);
		extractFixed(candidates);
		if (candidates.empty())
			break;

		chunk.assign(candidates.begin(), candidates.begin() + std::min(chunkSize, candidates.size()));

		/* CaDiCaL 'constrain' emulated by the clause (-a v -l_1 v ... v -l_k) of a fresh activation literal a,
		 * retired by the unit -a after the call */
		m_solver->new_var();
		uint32_t activation = m_solver->nVars() - 1;
		clause.clear();
		clause.push_back(CMSat::Lit(activation, true));
		for (int lit : chunk)
			clause.push_back(toCms(-lit));
		m_solver->add_clause(clause);
		assumption[0] = CMSat::Lit(activation, false);

		int res = solveWith(assumption, 0);
		std::vector<CMSat::Lit> conflict;
		if (res == 20)
			conflict = m_solver->get_conflict();
		m_solver->add_clause({ CMSat::Lit(activation, true) });

		if (res == 10) {
			m_satAnswers++;
			filterWithModel(candidates);
			if (m_chunkRate != 0)
				chunkSize = 1;
		} else if (res == 20) {
			m_unsatAnswers++;
			if (conflict.empty()) {
				LOGSTAT("DiverseBackboneSearch %d: formula is UNSATISFIABLE", this->getSolverId());
				return BackboneResult::UNSAT;
			}
			/* No model falsifies a literal of the chunk: all of them are backbone literals */
			for (int lit : chunk)
				commitBackbone(lit);
			candidates.erase(candidates.begin(), candidates.begin() + chunk.size());
			if (m_chunkRate != 0)
				chunkSize = (chunkSize > infinite / m_chunkRate) ? infinite : chunkSize * m_chunkRate;
		} else {
			return BackboneResult::UNKNOWN;
		}
	}
	return BackboneResult::COMPLETE;
}

/* Candidates */

void
DiverseBackboneSearch::filterWithModel(std::vector<int>& candidates)
{
	struct Timer
	{
		double& total;
		double start = seconds();
		~Timer() { total += seconds() - start; }
	} timer{ m_timeFilter };
	const auto& model = m_solver->get_model();
	for (unsigned int var = 1; var <= m_nbVars; var++) {
		m_model[var] = (model[var - 1] == CMSat::l_False) ? -1 : 1;
		/* The model is a model of the formula: the negation of each of its literals is not backbone */
		int falsified = (m_model[var] > 0) ? -(int)var : (int)var;
		m_notBackbone[litIndex(falsified)] = 1;
		if (!m_free[var] && m_notBackbone[litIndex(-falsified)])
			markFree(var);
	}

	/* A flippable literal can be flipped in this model without falsifying a clause: its negation is in a model */
	const bool useFlip = !__globalParameters__.backboneNoFlip && !m_clauses.empty();
	candidates.erase(std::remove_if(candidates.begin(),
									candidates.end(),
									[this, useFlip](int lit) {
										int var = std::abs(lit);
										if (!m_free[var] && useFlip && flippable(lit))
											markFree(var);
										return m_free[var] != 0;
									}),
					 candidates.end());
}

bool
DiverseBackboneSearch::flippable(int lit) const
{
	assert(modelValue(lit));
	for (size_t offset : m_occurrenceLists[litIndex(lit)]) {
		bool otherTrue = false;
		for (size_t i = offset; m_clauses[i] != 0 && !otherTrue; i++)
			otherTrue = (m_clauses[i] != lit) && modelValue(m_clauses[i]);
		if (!otherTrue)
			return false;
	}
	return true;
}

void
DiverseBackboneSearch::markFree(int lit)
{
	unsigned int var = std::abs(lit);
	m_free[var] = 1;
	m_notBackbone[litIndex(var)] = 1;
	m_notBackbone[litIndex(-(int)var)] = 1;
	m_freeVariables++;
	if (m_publishCandidates && m_candidateBoard->publishFree(var))
		m_publishedFree++;
}

void
DiverseBackboneSearch::extractFixed(std::vector<int>& candidates)
{
	/* Root-level values of CryptoMiniSat are implied by the formula (the fresh variables are only definitions) */
	for (CMSat::Lit lit : m_solver->get_zero_assigned_lits())
		if (lit.var() < m_nbVars)
			m_fixed[lit.var() + 1] = lit.sign() ? -1 : 1;

	size_t kept = 0;
	for (int lit : candidates) {
		int8_t value = m_fixed[std::abs(lit)];
		if (value == 0) {
			candidates[kept++] = lit;
		} else if ((value > 0) == (lit > 0)) {
			addBackboneLiteral(lit);
			m_fixedFound++;
		}
		/* fixed to false: dropped, as fix_candidate of cadiback.cpp */
	}
	candidates.resize(kept);
}

void
DiverseBackboneSearch::syncWithBoard(std::vector<int>& candidates)
{
	if (!m_consumeCandidates)
		return;

	size_t kept = 0;
	for (int lit : candidates) {
		int status = m_candidateBoard->status(lit);
		unsigned int var = std::abs(lit);
		if (status == 0) {
			candidates[kept++] = lit;
		} else if (status == CandidateBoard::FREE) {
			m_free[var] = 1;
			m_notBackbone[litIndex(var)] = 1;
			m_notBackbone[litIndex(-(int)var)] = 1;
			m_boardFree++;
		} else {
			/* A backbone literal is true in every model, hence in the one that made it a candidate here */
			assert(status == lit);
			/* Not exported again as a unit: the solver that proved it already did (if its mask allows it) */
			m_backbone.push_back(lit);
			m_unitExported[var] = 1;
			m_solver->add_clause({ toCms(lit) });
			m_boardBackbone++;
		}
	}
	candidates.resize(kept);
}

void
DiverseBackboneSearch::commitBackbone(int lit)
{
	addBackboneLiteral(lit);
	m_solver->add_clause({ toCms(lit) });
}

void
DiverseBackboneSearch::addBackboneLiteral(int lit)
{
	m_backbone.push_back(lit);

	if (m_publishCandidates && m_candidateBoard->publishBackbone(lit))
		m_publishedBackbone++;

	/* Marked even when the mask disables the export, so that exportLearntClauses does not export it either */
	unsigned int var = std::abs(lit);
	if (m_unitExported[var])
		return;
	m_unitExported[var] = 1;
	if (!m_shareBackboneUnits)
		return;
	std::vector<int> unit{ lit };
	if (this->exportClause(ClauseExchange::create(unit, 0, this->getSharingId())))
		LOGDEBUG2("DiverseBackboneSearch %d exported backbone literal %d", this->getSolverId(), lit);
}

/* Sharing */

void
DiverseBackboneSearch::importPendingClauses()
{
	ClauseExchangePtr clause;
	std::vector<CMSat::Lit> lits;
	while (m_clausesToImport->getOneClause(clause)) {
		lits.clear();
		bool valid = true;
		for (int lit : *clause) {
			if ((unsigned int)std::abs(lit) > m_nbVars) {
				valid = false;
				break;
			}
			lits.push_back(toCms(lit));
		}
		if (valid)
			m_solver->add_clause(lits);
	}
	m_clausesToImport->shrinkDatabase();
}

void
DiverseBackboneSearch::exportOnce(std::vector<int>& clause, int lbd)
{
	std::sort(clause.begin(), clause.end());
	if (!m_exportedHashes.insert(hashClause(clause)).second)
		return;
	if (this->exportClause(ClauseExchange::create(clause, lbd, this->getSharingId())))
		m_exported++;
}

void
DiverseBackboneSearch::exportLearntClauses()
{
	if (m_unitExported.empty())
		return; /* before solve() set up the arrays */

	uint64_t conflicts = m_solver->get_sum_conflicts();
	if (conflicts - m_conflictsAtLastExport < EXPORT_INTERVAL)
		return;
	m_conflictsAtLastExport = conflicts;

	/* Units: root-level values over the original variables */
	std::vector<int> clause;
	for (CMSat::Lit lit : m_solver->get_zero_assigned_lits()) {
		if (lit.var() >= m_nbVars || m_unitExported[lit.var() + 1])
			continue;
		m_unitExported[lit.var() + 1] = 1;
		clause.assign(1, fromCms(lit));
		if (this->exportClause(ClauseExchange::create(clause, 0, this->getSharingId())))
			m_exported++;
	}

	/* Learnt clauses (and equivalences) without fresh variable: implied by the formula alone. Off by default: without
	 * the glue of CryptoMiniSat they get a guessed lbd, pass the filters of the sharing strategies and slow down the
	 * other workers */
	if (!m_xorOptions.exportLearnt)
		return;
	const uint32_t maxLength = std::max(2, __globalParameters__.maxClauseSize);
	std::vector<CMSat::Lit> lits;
	for (uint32_t glue : EXPORT_GLUES) {
		m_solver->start_getting_small_clauses(maxLength, glue, true, false, false);
		while (m_solver->get_next_small_clause(lits)) {
			if (lits.empty() || std::any_of(lits.begin(), lits.end(), [this](CMSat::Lit l) {
					return l.var() >= m_nbVars;
				}))
				continue;
			clause.clear();
			for (CMSat::Lit lit : lits)
				clause.push_back(fromCms(lit));
			int lbd = (clause.size() == 1) ? 0 : (int)std::min<size_t>(glue, clause.size());
			exportOnce(clause, lbd);
		}
		m_solver->end_getting_small_clauses();
	}
}

bool
DiverseBackboneSearch::importClause(const ClauseExchangePtr& clause)
{
	assert(clause->size > 0);
	m_clausesToImport->addClause(clause);
	return true;
}

void
DiverseBackboneSearch::importClauses(const std::vector<ClauseExchangePtr>& clauses)
{
	for (auto& clause : clauses)
		importClause(clause);
}

/* Control */

std::vector<int>
DiverseBackboneSearch::getBackbone()
{
	return m_backbone;
}

void
DiverseBackboneSearch::setSolverInterrupt()
{
	m_stop = true;
	m_solver->interrupt_asap();
	LOGDEBUG1("Asking DiverseBackboneSearch (%d, %u) to end", this->getSolverId(), this->getSolverTypeId());
}

void
DiverseBackboneSearch::unsetSolverInterrupt()
{
	m_stop = false;
}

void
DiverseBackboneSearch::diversify(const SeedGenerator& getSeed)
{
	m_seed = getSeed(this);
	m_rng.seed(m_seed);
	m_solver->set_seed(m_seed);

	/* The XORs (seeded draw, -bb-xor-*) are the main diversification, the polarity mode cycles over the workers */
	static const CMSat::PolarityMode modes[] = { CMSat::PolarityMode::polarmode_automatic,
												 CMSat::PolarityMode::polarmode_rnd,
												 CMSat::PolarityMode::polarmode_stable,
												 CMSat::PolarityMode::polarmode_best };
	m_solver->set_polarity_mode(modes[this->getSolverTypeId() % 4]);

	LOGDEBUG1("Diversification of DiverseBackboneSearch (%d,%u): seed %u, xor m=%u %s, rounds %lu, nodes %lu, "
			  "budget %s",
			  this->getSolverId(),
			  this->getSolverTypeId(),
			  m_seed,
			  m_xorOptions.count,
			  xorDensityName(m_xorOptions.density).c_str(),
			  m_xorOptions.rounds,
			  m_xorOptions.leaves,
			  xorBudgetName(m_xorOptions.budget).c_str());
}

/* Formula */

void
DiverseBackboneSearch::loadFormula(const char* filename)
{
	std::vector<simpleClause> clauses;
	unsigned int nbVars = 0;
	if (!Parsers::parseCNF(filename, clauses, &nbVars)) {
		LOGERROR("DiverseBackboneSearch %d: cannot parse %s", this->getSolverId(), filename);
		exit(PERR_PARSING);
	}
	addInitialClauses(clauses, nbVars);
}

void
DiverseBackboneSearch::addInitialClauses(const std::vector<simpleClause>& clauses, unsigned int nbVars)
{
	m_nbVars = nbVars;
	m_solver->new_vars(nbVars);
	m_occurrences.reset(nbVars);

	const bool keepClauses = !__globalParameters__.backboneNoFlip;
	if (keepClauses)
		m_occurrenceLists.assign(2 * (nbVars + 1), {});

	std::vector<CMSat::Lit> lits;
	for (const auto& clause : clauses) {
		lits.clear();
		size_t offset = m_clauses.size();
		for (int lit : clause) {
			lits.push_back(toCms(lit));
			m_occurrences.count(lit);
			if (keepClauses) {
				m_clauses.push_back(lit);
				m_occurrenceLists[litIndex(lit)].push_back(offset);
			}
		}
		if (keepClauses)
			m_clauses.push_back(0);
		m_solver->add_clause(lits);
	}
	this->setInitialized(true);
	LOG2("The DiverseBackboneSearch Solver %d loaded all the %zu clauses with %u variables",
		 this->getSolverId(),
		 clauses.size(),
		 nbVars);
}

void
DiverseBackboneSearch::addInitialClauses(const lit_t* literals, unsigned int clsCount, unsigned int nbVars)
{
	std::vector<simpleClause> clauses(clsCount);
	for (unsigned int c = 0; c < clsCount; c++, literals++)
		for (; *literals; literals++)
			clauses[c].push_back(*literals);
	addInitialClauses(clauses, nbVars);
}

unsigned int
DiverseBackboneSearch::getVariablesCount()
{
	return m_nbVars;
}

void
DiverseBackboneSearch::setPhase(const unsigned int var, const bool phase)
{
	/* CryptoMiniSat 5.11 has no per-variable phase in its API */
	LOGDEBUG2("DiverseBackboneSearch %d ignores the phase %d of variable %u", this->getSolverId(), phase, var);
}

/* Statistics */

void
DiverseBackboneSearch::printStatistics()
{
	std::cout << "c" << std::left << std::setw(15) << ("| DiverseXor" + std::to_string(this->getSolverTypeId()))
			  << std::setw(20) << ("| " + std::to_string(m_solver->get_sum_conflicts())) << std::setw(20)
			  << ("| " + std::to_string(m_solver->get_sum_propagations())) << std::setw(17)
			  << ("| " + std::to_string(m_satCalls)) << std::setw(20) << ("| " + std::to_string(m_backbone.size()))
			  << std::setw(20) << "|"
			  << "\n";
	/* Printed under the logger lock taken by BackboneSolverFactory::printStats, so std::cout and not LOGSTAT */
	std::cout << "c|   xor, solver " << this->getSolverId() << " (m=" << m_xorOptions.count << " "
			  << xorDensityName(m_xorOptions.density) << ", budget " << xorBudgetName(m_xorOptions.budget) << " = "
			  << m_nodeBudget << "): " << m_xorRounds << " rounds, " << m_xorsAdded
			  << " XORs (" << m_xorsDependent << " dependent drawn), " << (m_xorOptions.adaptive ? "adaptive" : "leaves")
			  << " DFS, nodes " << m_dfsStats.sat << " SAT / " << m_dfsStats.unsat << " UNSAT / "
			  << m_dfsStats.unknown << " unknown, " << m_dfsStats.pruned << " leaves pruned, mean SAT depth "
			  << (m_dfsStats.sat ? (double)m_dfsStats.depthSum / m_dfsStats.sat : 0.0) << ", "
			  << m_freeByXor << " free found by XORs, " << m_exported << " clauses exported\n";
	std::cout << std::fixed << std::setprecision(2) << "c|   time, solver " << this->getSolverId() << ": solve "
			  << m_timeSolve << " s, export " << m_timeExport << " s, import " << m_timeImport << " s, filter "
			  << m_timeFilter << " s\n"
			  << std::defaultfloat;
	if (m_publishCandidates || m_consumeCandidates)
		std::cout << "c|   board, solver " << this->getSolverId() << " (DiverseXor, order "
				  << candidateOrderName(m_candidateOrder) << ", chunk " << m_chunkRate << "): published first "
				  << m_publishedFree << " free / " << m_publishedBackbone << " backbone, took " << m_boardFree
				  << " free / " << m_boardBackbone << " backbone\n";
}

void
DiverseBackboneSearch::printWinningLog()
{
	BackboneSolverInterface::printWinningLog();
	LOGSTAT("The winner is DiverseBackboneSearch(%d, %d), xor m=%u %s, order %s, chunk %lu: %lu SAT calls (%lu SAT, "
			"%lu UNSAT), %lu XOR rounds, %lu free variables found by XORs, %lu fixed literals, backbone size %zu",
			this->getSolverId(),
			this->getSolverTypeId(),
			m_xorOptions.count,
			xorDensityName(m_xorOptions.density).c_str(),
			candidateOrderName(m_candidateOrder),
			m_chunkRate,
			m_satCalls,
			m_satAnswers,
			m_unsatAnswers,
			m_xorRounds,
			m_freeByXor,
			m_fixedFound,
			m_backbone.size());
	if (m_consumeCandidates)
		LOGSTAT("From the candidate board: %lu free variables dropped, %lu backbone literals taken",
				m_boardFree,
				m_boardBackbone);
}
