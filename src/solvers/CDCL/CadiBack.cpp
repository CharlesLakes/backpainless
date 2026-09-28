#include "solvers/CDCL/CadiBack.hpp"

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
CadiBack::learning(int size, int glue)
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
CadiBack::learn(int lit)
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
CadiBack::hasClauseToImport()
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
CadiBack::getClauseToImport(std::vector<int>& clause, int& glue)
{
	assert((tempClauseToImport->size > 1 && tempClauseToImport->lbd > 0) ||
		   (tempClauseToImport->size == 1 && tempClauseToImport->lbd >= 0));

	assert(clause.empty());
	clause.insert(clause.end(), tempClauseToImport->begin(), tempClauseToImport->end());

	glue = tempClauseToImport->lbd;
	LOGCLAUSE2(clause.data(), clause.size(), "CadiBack %d will import Clause (lbd:%u)", this->getSolverId(), glue);
}

/*----------------------Main Class------------------------*/

CadiBack::CadiBack(int id, const std::shared_ptr<ClauseDatabase>& clauseDB)
	: BackboneSolverInterface(id, clauseDB, BackboneSolverType::CADIBACK)
	, stopSolver(false)
{
	solver = std::make_unique<CaDiCaL::Solver>();
	solver->connect_learner(this);
	solver->connect_terminator(this);
	this->initCadicalOptions();

	initializeTypeId<CadiBack>();
}

CadiBack::~CadiBack()
{
	solver->terminate(); /* just in case */
	solver->disconnect_learner();
	solver->disconnect_terminator();
}

/* Backbone extraction */

int
CadiBack::solveUnderConstraint(const std::vector<int>& constraint)
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
CadiBack::solve(const std::vector<int>& cube)
{
	if (!this->isInitialized()) {
		LOGWARN("CadiBack %d was not initialized to be launched!", this->getSolverId());
		return BackboneResult::UNKNOWN;
	}
	if (!cube.empty()) {
		LOGWARN("CadiBack %d ignores the given cube of size %zu", this->getSolverId(), cube.size());
	}

	m_backbone.clear();

	const unsigned long chunkRate = __globalParameters__.backboneChunkRate;
	const size_t infinite = std::numeric_limits<size_t>::max();

	/* (res, sigma) <- SAT(phi) */
	int res = solveUnderConstraint({});

	if (res == 20) {
		LOGSTAT("CadiBack %d: formula is UNSATISFIABLE", this->getSolverId());
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

	LOG1("CadiBack %d: %zu initial candidates out of %u variables", this->getSolverId(), candidates.size(), m_nbVars);

	/* As in cadiback.cpp: without chunking (K = 0) the constraint always holds all remaining candidates. With chunking
	 * the size is reset to 1 after a SAT answer and multiplied by K after an UNSAT answer (K = 1: one-by-one) */
	size_t k = (chunkRate == 0) ? infinite : 1;
	std::vector<int> constraint;

	while (!candidates.empty()) {
		if (stopSolver)
			return BackboneResult::UNKNOWN;

		/* B <- B u F, Lambda <- Lambda \ F */
		extractFixed(candidates);
		if (candidates.empty())
			break;

		/* Gamma <- first min(k, |Lambda|) candidates, rho <- OR of the negations */
		size_t chunkSize = std::min(k, candidates.size());
		constraint.clear();
		for (size_t i = 0; i < chunkSize; i++)
			constraint.push_back(-candidates[i]);

		res = solveUnderConstraint(constraint);

		if (res == 10) {
			/* The new model disagrees with at least one literal of the chunk */
			m_satAnswers++;
			filterWithModel(candidates);
			if (chunkRate != 0)
				k = 1;
		} else if (res == 20) {
			/* No model falsifies a literal of the chunk: all of them are backbone literals. cadiback.cpp does not add
			 * them to the solver (CaDiCaL learns them implicitly); adding the units is sound and makes them fixed */
			m_unsatAnswers++;
			for (size_t i = 0; i < chunkSize; i++) {
				m_backbone.push_back(candidates[i]);
				solver->add(candidates[i]);
				solver->add(0);
			}
			candidates.erase(candidates.begin(), candidates.begin() + chunkSize);

			if (chunkRate != 0)
				k = (k > infinite / chunkRate) ? infinite : k * chunkRate;
		} else {
			return BackboneResult::UNKNOWN; /* interrupted */
		}
	}

	std::sort(m_backbone.begin(), m_backbone.end(), [](int a, int b) { return std::abs(a) < std::abs(b); });

	LOGSTAT("CadiBack %d: formula is SATISFIABLE, backbone size %zu / %u variables (%lu SAT calls)",
			this->getSolverId(),
			m_backbone.size(),
			m_nbVars,
			m_satCalls);

	return BackboneResult::COMPLETE;
}

void
CadiBack::extractFixed(std::vector<int>& candidates)
{
	/* Same as fix_candidate in cadiback.cpp: fixed true -> backbone, fixed false -> dropped */
	size_t kept = 0;
	for (int lit : candidates) {
		int value = solver->fixed(lit);
		if (value > 0) {
			m_backbone.push_back(lit);
			m_fixedFound++;
		} else if (value == 0) {
			candidates[kept++] = lit;
		}
	}
	candidates.resize(kept);
}

void
CadiBack::filterWithModel(std::vector<int>& candidates)
{
	/* filter_candidates and try_to_flip_remaining of cadiback.cpp */
	const bool useFlip = !__globalParameters__.backboneNoFlip;

	candidates.erase(std::remove_if(candidates.begin(),
									candidates.end(),
									[this, useFlip](int lit) {
										return solver->val(lit) <= 0 || (useFlip && solver->flippable(lit));
									}),
					 candidates.end());
}

std::vector<int>
CadiBack::getBackbone()
{
	return m_backbone;
}

void
CadiBack::setSolverInterrupt()
{
	this->stopSolver = true;
	LOGDEBUG1("Asking CadiBack (%d, %u) to end", this->getSolverId(), this->getSolverTypeId());
}

void
CadiBack::unsetSolverInterrupt()
{
	this->stopSolver = false;
}

/* Options and diversification */

void
CadiBack::initCadicalOptions()
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
CadiBack::applyCadicalOptions()
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
CadiBack::diversify(const SeedGenerator& getSeed)
{
	unsigned int typeId = this->getSolverTypeId();
	unsigned int generalSeed = getSeed(this);

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
CadiBack::loadFormula(const char* filename)
{
	int nbVars;
	int strict = 2;
	solver->read_dimacs(filename, nbVars, strict);
	m_nbVars = nbVars;
	this->setInitialized(true);
}

void
CadiBack::addInitialClauses(const std::vector<simpleClause>& clauses, unsigned int nbVars)
{
	solver->reserve(nbVars);

	for (auto& clause : clauses) {
		solver->clause(clause);
	}
	m_nbVars = nbVars;
	this->setInitialized(true);
	LOG2("The CadiBack Solver %d loaded all the %u clauses with %u variables",
		 this->getSolverId(),
		 clauses.size(),
		 nbVars);
}

void
CadiBack::addInitialClauses(const lit_t* literals, unsigned int clsCount, unsigned int nbVars)
{
	solver->reserve(nbVars);

	unsigned int clausesCount = 0;
	int lit;
	for (lit = *literals; clausesCount < clsCount; literals++, lit = *literals) {
		solver->add(lit);
		if (!lit)
			clausesCount++;
	}
	m_nbVars = nbVars;
	this->setInitialized(true);
	LOG2("The CadiBack Solver %d loaded all the %u clauses with %u variables", this->getSolverId(), clsCount, nbVars);
}

unsigned int
CadiBack::getVariablesCount()
{
	return m_nbVars;
}

void
CadiBack::setPhase(const unsigned int var, const bool phase)
{
	solver->phase((phase) ? var : -var);
}

/* Sharing */

bool
CadiBack::importClause(const ClauseExchangePtr& clause)
{
	assert(clause->size > 0);
	m_clausesToImport->addClause(clause);
	return true;
}

void
CadiBack::importClauses(const std::vector<ClauseExchangePtr>& clauses)
{
	for (auto cls : clauses) {
		importClause(cls);
	}
}

/* Statistics */

void
CadiBack::printStatistics()
{
	CaDiCaL::Stats* cstats = solver->getStatistics();

	std::cout << "c" << std::left << std::setw(15) << ("| CB" + std::to_string(this->getSolverTypeId()))
			  << std::setw(20) << ("| " + std::to_string(cstats->conflicts)) << std::setw(20)
			  << ("| " + std::to_string(cstats->propagations.search)) << std::setw(17)
			  << ("| " + std::to_string(m_satCalls)) << std::setw(20) << ("| " + std::to_string(m_backbone.size()))
			  << std::setw(20) << "|"
			  << "\n";
}

void
CadiBack::printWinningLog()
{
	BackboneSolverInterface::printWinningLog();
	LOGSTAT("The winner is CadiBack(%d, %d): %lu SAT calls (%lu SAT, %lu UNSAT), %lu fixed literals, backbone size %zu",
			this->getSolverId(),
			this->getSolverTypeId(),
			m_satCalls,
			m_satAnswers,
			m_unsatAnswers,
			m_fixedFound,
			m_backbone.size());
}
