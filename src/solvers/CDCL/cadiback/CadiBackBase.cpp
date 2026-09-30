#include "solvers/CDCL/cadiback/CadiBackBase.hpp"

#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/Parameters.hpp"

#include "cadical/src/stats.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>

/*------------------------Learner-------------------------*/

bool
CadiBackBase::learning(int size, int glue)
{
	if (size > 0) {
		LOGDEBUG3("CadiBack %d will export clause of size %d, glue %d", this->getSolverId(), size, glue);
		tempClause.reserve(size);
		this->lbd = glue;
		return true;
	} else {
		return false;
	}
}

void
CadiBackBase::learn(int lit)
{
	if (lit)
		tempClause.push_back(lit);
	else {
		assert(tempClause.size() > 0 && this->lbd >= 0);
		auto exportedClause = ClauseExchange::create(tempClause, this->lbd, this->getSharingId());

		assert(tempClause.size() == exportedClause->size);
		assert((exportedClause->size > 1 && exportedClause->lbd > 0) ||
			   (exportedClause->size == 1 && exportedClause->lbd >= 0));

		/* filtering defined by a sharing strategy, done here in case it checks its literals */
		if (this->exportClause(exportedClause)) {
			LOGCLAUSE2(exportedClause->lits,
					   exportedClause->size,
					   "CadiBack %d exported Clause %p for sharing",
					   this->getSolverId(),
					   exportedClause.get());
		}
		tempClause.clear();
	}
}

bool
CadiBackBase::hasClauseToImport()
{
	if (this->m_clausesToImport->getOneClause(tempClauseToImport)) {
		LOGDEBUG3("CadiBack %u will import clause %s", this->getSharingId(), tempClauseToImport->toString().c_str());
		return true;
	} else {
		m_clausesToImport->shrinkDatabase();
		return false;
	}
}

void
CadiBackBase::getClauseToImport(std::vector<int>& clause, int& glue)
{
	assert((tempClauseToImport->size > 1 && tempClauseToImport->lbd > 0) ||
		   (tempClauseToImport->size == 1 && tempClauseToImport->lbd >= 0));

	assert(clause.empty());
	clause.insert(clause.end(), tempClauseToImport->begin(), tempClauseToImport->end());

	glue = tempClauseToImport->lbd;
	LOGCLAUSE2(clause.data(), clause.size(), "CadiBack %d will import Clause (lbd:%u)", this->getSolverId(), glue);
}

/*----------------------Main Class------------------------*/

CadiBackBase::CadiBackBase(int id, const std::shared_ptr<ClauseDatabase>& clauseDB, BackboneSolverType type)
	: BackboneSolverInterface(id, clauseDB, type)
	, stopSolver(false)
{
	solver = std::make_unique<CaDiCaL::Solver>();
	solver->connect_learner(this);
	solver->connect_terminator(this);
	this->initCadicalOptions();
	/* initializeTypeId is called by each variant, so that the diversification cycles over the workers of a variant */
}

CadiBackBase::~CadiBackBase()
{
	solver->terminate(); /* just in case */
	solver->disconnect_learner();
	solver->disconnect_terminator();
}

/* Backbone extraction */

int
CadiBackBase::solveUnderConstraint(const std::vector<int>& constraint)
{
	/* The interrupt flag is not reset here: an interrupt arriving between two incremental calls must not be lost */
	if (!constraint.empty()) {
		for (int lit : constraint)
			solver->constrain(lit);
		solver->constrain(0);
	}

	m_satCalls++;
	return solver->solve();
}

BackboneResult
CadiBackBase::solve(const std::vector<int>& cube)
{
	if (!this->isInitialized()) {
		LOGWARN("CadiBack %d was not initialized to be launched!", this->getSolverId());
		return BackboneResult::UNKNOWN;
	}
	if (!cube.empty()) {
		LOGWARN("CadiBack %d ignores the given cube of size %zu", this->getSolverId(), cube.size());
	}

	m_backbone.clear();

	m_chunkRate = __globalParameters__.backboneChunkRate;

	/* (res, sigma) <- SAT(phi) */
	int res = solveUnderConstraint({});

	if (res == 20) {
		LOGSTAT("%s %d: formula is UNSATISFIABLE", variantName(), this->getSolverId());
		return BackboneResult::UNSAT;
	}
	if (res != 10 || stopSolver)
		return BackboneResult::UNKNOWN;

	m_satAnswers++;

	/* Lambda <- {l in sigma | not flippable(l, sigma)} */
	std::vector<int> candidates;
	candidates.reserve(m_nbVars);
	for (int var = 1; var <= (int)m_nbVars; var++)
		candidates.push_back((solver->val(var) > 0) ? var : -var);
	filterWithModel(candidates);

	if (m_occurrences.empty() &&
		(m_candidateOrder == CandidateOrder::OCC || m_candidateOrder == CandidateOrder::OCC_REV))
		LOGWARN("%s %d: no clause occurrences (formula loaded from file), -bb-order=%s falls back to natural",
				variantName(),
				this->getSolverId(),
				candidateOrderName(m_candidateOrder));
	sortCandidates(candidates, m_candidateOrder, m_occurrences, m_seed);
	initCandidates(candidates);

	LOG1("%s %d: %zu initial candidates out of %u variables, order %s",
		 variantName(),
		 this->getSolverId(),
		 candidates.size(),
		 m_nbVars,
		 candidateOrderName(m_candidateOrder));

	/* As in cadiback.cpp: without chunking (K = 0) the constraint always holds all remaining candidates. With chunking
	 * the size is reset to 1 after a SAT answer and multiplied by K after an UNSAT answer (K = 1: one-by-one) */
	m_chunkSize = (m_chunkRate == 0) ? std::numeric_limits<size_t>::max() : 1;
	std::vector<int> chunk, constraint;

	while (!candidates.empty()) {
		if (stopSolver)
			return BackboneResult::UNKNOWN;

		/* Candidates decided by the other solvers (-bb-share-cand), then B <- B u F, Lambda <- Lambda \ F */
		syncWithBoard(candidates);
		extractFixed(candidates);
		if (candidates.empty())
			break;

		/* Gamma <- selected chunk of Lambda, rho <- OR of the negations */
		chunk.clear();
		selectChunk(candidates, chunk);
		assert(!chunk.empty() && chunk.size() <= candidates.size());
		constraint.clear();
		for (int lit : chunk)
			constraint.push_back(-lit);

		res = solveUnderConstraint(constraint);

		if (res == 10) {
			/* The new model disagrees with at least one literal of the chunk */
			m_satAnswers++;
			onSat(candidates, chunk);
		} else if (res == 20) {
			/* No model falsifies a literal of the chunk: all of them are backbone literals */
			m_unsatAnswers++;
			onUnsat(candidates, chunk);
		} else {
			return BackboneResult::UNKNOWN; /* interrupted */
		}
	}

	std::sort(m_backbone.begin(), m_backbone.end(), [](int a, int b) { return std::abs(a) < std::abs(b); });

	LOGSTAT("%s %d: formula is SATISFIABLE, backbone size %zu / %u variables (%lu SAT calls)",
			variantName(),
			this->getSolverId(),
			m_backbone.size(),
			m_nbVars,
			m_satCalls);

	return BackboneResult::COMPLETE;
}

/* Default hooks: the algorithm of the paper */

void
CadiBackBase::initCandidates(std::vector<int>& /* candidates */)
{
	/* The order is the one of -bb-order (natural by default, as cadiback.cpp) */
}

void
CadiBackBase::selectChunk(const std::vector<int>& candidates, std::vector<int>& chunk)
{
	size_t chunkSize = std::min(m_chunkSize, candidates.size());
	chunk.assign(candidates.begin(), candidates.begin() + chunkSize);
}

void
CadiBackBase::onSat(std::vector<int>& candidates, const std::vector<int>& /* chunk */)
{
	filterWithModel(candidates);
	if (m_chunkRate != 0)
		m_chunkSize = 1;
}

void
CadiBackBase::onUnsat(std::vector<int>& candidates, const std::vector<int>& chunk)
{
	for (int lit : chunk)
		commitBackbone(lit);
	removeCandidates(candidates, chunk);

	if (m_chunkRate != 0) {
		const size_t infinite = std::numeric_limits<size_t>::max();
		m_chunkSize = (m_chunkSize > infinite / m_chunkRate) ? infinite : m_chunkSize * m_chunkRate;
	}
}

/* Helpers */

void
CadiBackBase::commitBackbone(int lit)
{
	/* cadiback.cpp does not add the backbone literals to the solver (CaDiCaL learns them implicitly); adding the units
	 * is sound and makes them fixed */
	addBackboneLiteral(lit);
	solver->add(lit);
	solver->add(0);
}

void
CadiBackBase::removeCandidates(std::vector<int>& candidates, const std::vector<int>& lits)
{
	/* Fast path of the default selectChunk: the literals are a prefix of the candidates */
	if (lits.size() <= candidates.size() && std::equal(lits.begin(), lits.end(), candidates.begin())) {
		candidates.erase(candidates.begin(), candidates.begin() + lits.size());
		return;
	}

	std::vector<char> removed(m_nbVars + 1, 0);
	for (int lit : lits)
		removed[std::abs(lit)] = 1;
	candidates.erase(std::remove_if(candidates.begin(),
									candidates.end(),
									[&removed](int lit) { return removed[std::abs(lit)]; }),
					 candidates.end());
}

void
CadiBackBase::extractFixed(std::vector<int>& candidates)
{
	/* Same as fix_candidate in cadiback.cpp: fixed true -> backbone, fixed false -> dropped */
	size_t kept = 0;
	for (int lit : candidates) {
		int value = solver->fixed(lit);
		if (value > 0) {
			addBackboneLiteral(lit);
			m_fixedFound++;
		} else if (value == 0) {
			candidates[kept++] = lit;
		}
	}
	candidates.resize(kept);
}

void
CadiBackBase::addBackboneLiteral(int lit)
{
	m_backbone.push_back(lit);

	if (m_publishCandidates)
		m_candidateBoard->publishBackbone(lit);

	if (!m_shareBackboneUnits)
		return;

	/* A backbone literal is implied by the formula alone (the constraint is only used for decisions), so the unit is
	 * sound for every other solver. The ones added by 'solver->add' are original clauses for CaDiCaL and the ones fixed
	 * by root propagation are never learnt, hence neither reaches the Learner hook. Duplicates of learnt or imported
	 * units are harmless: each solver exports a literal at most once, since it leaves the candidates afterwards */
	std::vector<int> unit{ lit };
	if (this->exportClause(ClauseExchange::create(unit, 0, this->getSharingId())))
		LOGDEBUG2("CadiBack %d exported backbone literal %d", this->getSolverId(), lit);
}

void
CadiBackBase::addFreeVariable(int lit)
{
	if (m_publishCandidates)
		m_candidateBoard->publishFree(lit);
}

void
CadiBackBase::filterWithModel(std::vector<int>& candidates)
{
	/* filter_candidates and try_to_flip_remaining of cadiback.cpp */
	const bool useFlip = !__globalParameters__.backboneNoFlip;

	/* A candidate was true in an earlier model; false or flippable in this model (a model of the formula, the
	 * constraint only acts on decisions) means that both values of its variable appear in models: it is free */
	candidates.erase(std::remove_if(candidates.begin(),
									candidates.end(),
									[this, useFlip](int lit) {
										bool drop = solver->val(lit) <= 0 || (useFlip && solver->flippable(lit));
										if (drop)
											addFreeVariable(lit);
										return drop;
									}),
					 candidates.end());
}

void
CadiBackBase::syncWithBoard(std::vector<int>& candidates)
{
	if (!m_consumeCandidates)
		return;

	size_t kept = 0;
	for (int lit : candidates) {
		int status = m_candidateBoard->status(lit);
		if (status == 0) {
			candidates[kept++] = lit;
		} else if (status == CandidateBoard::FREE) {
			m_boardFree++;
			LOGDEBUG2("CadiBack %d took free variable %d from the board", this->getSolverId(), std::abs(lit));
		} else {
			/* A backbone literal is true in every model, hence in the one that made it a candidate here */
			assert(status == lit);
			/* Not exported again as a unit: the solver that proved it already did (if its mask allows it) */
			m_backbone.push_back(lit);
			solver->add(lit);
			solver->add(0);
			m_boardBackbone++;
			LOGDEBUG2("CadiBack %d took backbone literal %d from the board", this->getSolverId(), lit);
		}
	}
	candidates.resize(kept);
}

std::vector<int>
CadiBackBase::getBackbone()
{
	return m_backbone;
}

void
CadiBackBase::setSolverInterrupt()
{
	this->stopSolver = true;
	LOGDEBUG1("Asking %s (%d, %u) to end", variantName(), this->getSolverId(), this->getSolverTypeId());
}

void
CadiBackBase::unsetSolverInterrupt()
{
	this->stopSolver = false;
}

/* Options and diversification */

void
CadiBackBase::initCadicalOptions()
{
#ifndef NDEBUG
	cadicalOptions.insert({ "quiet", 0 });
#else
	cadicalOptions.insert({ "quiet", 1 });
#endif
	cadicalOptions.insert({ "stabilize", 1 });
	cadicalOptions.insert({ "stabilizeonly", 0 });
	/*
	 * target = 1: in stable phase only
	 * target = 2: always choose target
	 */
	cadicalOptions.insert({ "target", 1 });

	cadicalOptions.insert({ "elimreleff", 1e3 });
	cadicalOptions.insert({ "subsumereleff", 1e3 });

	// Shuffling
	cadicalOptions.insert({ "shuffle", 0 });	   /* shuffle variables */
	cadicalOptions.insert({ "shufflequeue", 0 });  /* shuffle variable queue */
	cadicalOptions.insert({ "shufflescores", 0 }); /* shuffle variable queue */
	cadicalOptions.emplace("shufflerandom", 0);

	// Random Walks
	cadicalOptions.insert({ "walkredundant", 0 });
	cadicalOptions.insert({ "walknonstable", 1 });
	cadicalOptions.insert({ "walk", 1 });

	// Required by the backbone algorithm: with reimplication enabled, 'flippable' of the vendored CaDiCaL 1.9.1 can
	// answer true for literals whose flip falsifies the formula, which drops real backbone literals. Never change it
	// in diversify.
	cadicalOptions.insert({ "reimply", 0 });

	// Search Configuration
	cadicalOptions.insert({ "chrono", 1 });
	cadicalOptions.insert({ "chronoalways", 0 });
	cadicalOptions.insert({ "chronolevelim", 100 });

	// Restart Management
	cadicalOptions.insert({ "restart", 1 });
	cadicalOptions.insert({ "restartint", 1 });

	// Decision
	cadicalOptions.insert({ "score", 1 }); // 1: VSIDS, 0: no score computing

	// Phase
	cadicalOptions.insert({ "phase", 1 });
	cadicalOptions.insert({ "rephase", 1 });
	cadicalOptions.insert({ "rephaseint", 1e3 });
	cadicalOptions.insert({ "forcephase", 0 });

	// Simplification Techniques
	cadicalOptions.insert({ "block", 0 });	   /* Blocked clause elimination */
	cadicalOptions.insert({ "elim", 1 });	   /* Bounded Variable elimination */
	cadicalOptions.insert({ "otfs", 1 });	   /* on the fly subsumption */
	cadicalOptions.emplace("condition", 0);	   /* globally blocked clause elimination */
	cadicalOptions.emplace("cover", 0);		   /* covered clause elimination */
	cadicalOptions.emplace("inprocessing", 1); /* enable inprocessing (search is stopped, simplification is resumed)*/

	// Learnt Clauses
	cadicalOptions.insert({ "reducetier1glue", 2 });
	cadicalOptions.insert({ "reducetier2glue", 6 });

	// random seed
	cadicalOptions.insert({ "seed", 0 });

	applyCadicalOptions();
}

void
CadiBackBase::applyCadicalOptions()
{
	for (auto& opt : cadicalOptions) {
		/* not inside an assert: it would be removed with -DNDEBUG */
		bool ok = solver->set(opt.first.c_str(), opt.second);
		if (!ok)
			LOGWARN("CadiBack %d: invalid CaDiCaL option %s=%d", this->getSolverId(), opt.first.c_str(), opt.second);
	}
}

static std::mt19937 engine;
static std::uniform_int_distribution<unsigned> uniform(0, 100);

void
CadiBackBase::diversify(const SeedGenerator& getSeed)
{
	unsigned int typeId = this->getSolverTypeId();
	unsigned int generalSeed = getSeed(this);
	m_seed = generalSeed;

	cadicalOptions.at("seed") = generalSeed;
	cadicalOptions.at("phase") = generalSeed % 2;

	LOGDEBUG1("Diversification of CadiBack (%d,%d)", this->getSolverId(), typeId);
	// From Mallob Native Diversification
	switch (typeId % 10) {
		case 0:
			cadicalOptions.at("phase") = 0;
			break;
		case 1:
			solver->configure("sat");
			// 25% chances
			if (uniform(engine) < 25) {
				cadicalOptions.at("walk") = 1;
				cadicalOptions.at("target") = 2;
				cadicalOptions.at("chrono") = 1;
				cadicalOptions.at("chronoalways") = 1;
				cadicalOptions.at("walkredundant") = 1;
				cadicalOptions.at("reducetier1glue") = 2;
				cadicalOptions.at("reducetier2glue") = 3 + generalSeed % 2;
				cadicalOptions.at("restartint") = 90 + (1 + typeId % 10);
			}
			break;
		case 2:
			cadicalOptions.at("elim") = 0;
			break;
		case 3:
			solver->configure("unsat");
			cadicalOptions.at("restartint") = 1;
			if (uniform(engine) < 25) {
				cadicalOptions.at("target") = 0;
				cadicalOptions.at("chrono") = 0;
			}
			break;
		case 4:
			cadicalOptions.at("condition") = 1;
			break;
		case 5:
			cadicalOptions.at("walk") = 0;
			break;
		case 6:
			cadicalOptions.at("restartint") = 100;
			break;
		case 7:
			cadicalOptions.at("cover") = 1;
			break;
		case 8:
			cadicalOptions.at("shuffle") = 1;
			cadicalOptions.at("shufflerandom") = 1;
			break;
		case 9:
			cadicalOptions.at("inprocessing") = 0;
			break;
	}

	applyCadicalOptions();
}

/* Formula */

void
CadiBackBase::loadFormula(const char* filename)
{
	int nbVars;
	int strict = 2;
	solver->read_dimacs(filename, nbVars, strict);
	m_nbVars = nbVars;
	this->setInitialized(true);
}

void
CadiBackBase::addInitialClauses(const std::vector<simpleClause>& clauses, unsigned int nbVars)
{
	solver->reserve(nbVars);
	m_occurrences.reset(nbVars);

	for (auto& clause : clauses) {
		solver->clause(clause);
		for (int lit : clause)
			m_occurrences.count(lit);
	}
	m_nbVars = nbVars;
	this->setInitialized(true);
	LOG2("The CadiBack Solver %d loaded all the %u clauses with %u variables",
		 this->getSolverId(),
		 clauses.size(),
		 nbVars);
}

void
CadiBackBase::addInitialClauses(const lit_t* literals, unsigned int clsCount, unsigned int nbVars)
{
	solver->reserve(nbVars);
	m_occurrences.reset(nbVars);

	unsigned int clausesCount = 0;
	int lit;
	for (lit = *literals; clausesCount < clsCount; literals++, lit = *literals) {
		solver->add(lit);
		m_occurrences.count(lit);
		if (!lit)
			clausesCount++;
	}
	m_nbVars = nbVars;
	this->setInitialized(true);
	LOG2("The CadiBack Solver %d loaded all the %u clauses with %u variables", this->getSolverId(), clsCount, nbVars);
}

unsigned int
CadiBackBase::getVariablesCount()
{
	return m_nbVars;
}

void
CadiBackBase::setPhase(const unsigned int var, const bool phase)
{
	solver->phase((phase) ? var : -var);
}

/* Sharing */

bool
CadiBackBase::importClause(const ClauseExchangePtr& clause)
{
	assert(clause->size > 0);
	m_clausesToImport->addClause(clause);
	return true;
}

void
CadiBackBase::importClauses(const std::vector<ClauseExchangePtr>& clauses)
{
	for (auto cls : clauses) {
		importClause(cls);
	}
}

/* Statistics */

void
CadiBackBase::printStatistics()
{
	CaDiCaL::Stats* cstats = solver->getStatistics();

	std::cout << "c" << std::left << std::setw(15) << ("| " + std::string(variantName()) + std::to_string(this->getSolverTypeId()))
			  << std::setw(20) << ("| " + std::to_string(cstats->conflicts)) << std::setw(20)
			  << ("| " + std::to_string(cstats->propagations.search)) << std::setw(17)
			  << ("| " + std::to_string(m_satCalls)) << std::setw(20) << ("| " + std::to_string(m_backbone.size()))
			  << std::setw(20) << "|"
			  << "\n";
}

void
CadiBackBase::printWinningLog()
{
	BackboneSolverInterface::printWinningLog();
	LOGSTAT("The winner is %s(%d, %d), order %s: %lu SAT calls (%lu SAT, %lu UNSAT), %lu fixed literals, backbone "
			"size %zu",
			variantName(),
			this->getSolverId(),
			this->getSolverTypeId(),
			candidateOrderName(m_candidateOrder),
			m_satCalls,
			m_satAnswers,
			m_unsatAnswers,
			m_fixedFound,
			m_backbone.size());
	if (m_consumeCandidates)
		LOGSTAT("From the candidate board: %lu free variables dropped, %lu backbone literals taken",
				m_boardFree,
				m_boardBackbone);
}
