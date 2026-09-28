
#include "working/PortfolioSimple.hpp"
#include "painless.hpp"
#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/MpiUtils.hpp"
#include "utils/Parameters.hpp"
#include "utils/System.hpp"
#include "working/SequentialWorker.hpp"
#include <memory>
#include <thread>

#include "containers/ClauseDatabases/ClauseDatabaseFactory.hpp"
#include "sharing/GlobalStrategies/MallobSharing.hpp"

#include "preprocessors/GaspiInitializer.hpp"
#include "sharing/SharingStrategyFactory.hpp"
#include "solvers/BackboneSolverFactory.hpp"
#include "utils/Parsers.hpp"

PortfolioSimple::PortfolioSimple() {}

PortfolioSimple::~PortfolioSimple()
{
	// Wait for sharers in order to have stats and mpi_winner if dist
	for (int i = 0; i < sharers.size(); i++) {
		sharers[i]->join();
	}

	BackboneSolverFactory::printStats(this->solvers);

#ifndef NDEBUG
	for (size_t i = 0; i < slaves.size(); i++) {
		delete slaves[i];
	}
	LOGDEBUG1("PortfolioSimple After Buffer Clearing");
#endif
}

void
PortfolioSimple::solve(const std::vector<int>& cube)
{
	LOG0(">> PortfolioSimple");

	LOGWARN("New version was not tested on distirbuted mode, yet");

	strategyEnding = false;

	std::vector<simpleClause> initClauses;
	unsigned int varCount = 0;
	int receivedFinalResultBcast = 0;

	// TODO: merge these threads with sequential workers in next version, for less OS intensive calls
	std::vector<std::thread> solverInitializers;

	if (mpi_rank <= 0) {
		if (!Parsers::parseCNF(__globalParameters__.filename.c_str(), initClauses, &varCount)) {
			PABORT(PERR_PARSING, "Error at parsing!");
		}
		receivedFinalResultBcast = static_cast<int>(finalResult.load());
	}

	// Send instance via MPI from leader 0 to workers.
	if (dist) {
		TESTRUNMPI(MPI_Bcast(&receivedFinalResultBcast, 1, MPI_INT, 0, MPI_COMM_WORLD));

		if (receivedFinalResultBcast != 0) {
			mpi_winner = 0;
			finalResult = static_cast<BackboneResult>(receivedFinalResultBcast);
			globalEnding = true;
			LOGDEBUG1("It is the mpi end: %d", receivedFinalResultBcast);
			mutexGlobalEnd.lock();
			condGlobalEnd.notify_all();
			mutexGlobalEnd.unlock();
			return;
		} else // send formula if not solved while parsing
			mpiutils::sendFormula(initClauses, &varCount, 0);
	}

	unsigned int clausesCount = initClauses.size();

	// Init Database Factory For Solvers (Is it better to put this in the BackboneSolverFactory as for SharingFactory ?)
	ClauseDatabaseFactory::initialize(__globalParameters__.maxClauseSize, __globalParameters__.importDBCap, 2, 1);

	BackboneSolverFactory::createSolvers(
		__globalParameters__.cpus, __globalParameters__.importDB.c_str()[0], __globalParameters__.solver, solvers);

	IDScaler globalIDScaler;
	IDScaler typeIDScaler;
	if (dist) {
		globalIDScaler = [rank = mpi_rank,
						  size = __globalParameters__.cpus](const std::shared_ptr<BackboneSolverInterface>& solver) {
			return rank * size + solver->getSolverId();
		};
		typeIDScaler = [rank = mpi_rank,
						size = __globalParameters__.cpus](const std::shared_ptr<BackboneSolverInterface>& solver) {
			return rank * solver->getSolverTypeCount() + solver->getSolverTypeId();
		};
	} else {
		globalIDScaler = [](const std::shared_ptr<BackboneSolverInterface>& solver) { return solver->getSolverId(); };
		typeIDScaler = [](const std::shared_ptr<BackboneSolverInterface>& solver) {
			return solver->getSolverTypeId();
		};
	}
	BackboneSolverFactory::diversification(solvers, globalIDScaler, typeIDScaler);

	LOG0("Diversified all solvers");

	/* Sharing */
	/* ------- */
	if (__globalParameters__.enableMallob && dist) {
		// only global strategy
		SharingStrategyFactory::instantiateGlobalStrategies(2, globalStrategies);
		for (auto& solver : solvers) {
			globalStrategies.back()->addProducer(solver);
			globalStrategies.back()->addClient(solver);
			globalStrategies.back()->connectProducer(solver);
		}
	} else {
		SharingStrategyFactory::instantiateLocalStrategies(
			__globalParameters__.sharingStrategy, this->localStrategies, solvers);

		if (dist) {
			SharingStrategyFactory::instantiateGlobalStrategies(__globalParameters__.globalSharingStrategy,
																globalStrategies);
		}

		/* This is part of the sharing strategy: how local and global are connected */
		for (auto& lstrat : this->localStrategies) {
			for (auto& gstrat : this->globalStrategies) {
				/* Gstrat is a producer (pushes clauses) and consumer (get clauses) of lstrat*/
				lstrat->addProducer(gstrat);
				lstrat->addClient(gstrat);
				lstrat->connectProducer(gstrat); // This adds lstrat as a client to gstrat
			}
		}
	}

	std::vector<std::shared_ptr<SharingStrategy>> sharingStrategiesConcat;

	/* Launch sharers */
	for (auto lstrat : localStrategies) {
		sharingStrategiesConcat.push_back(lstrat);
	}
	for (auto gstrat : globalStrategies) {
		sharingStrategiesConcat.push_back(gstrat);
	}

	if (globalEnding) {
		this->setSolverInterrupt();
		return;
	}

	/* Phase initialization with GaspiInitializer (optional) */
	/* ----------------------------------------------------- */
	/* Computed before the solvers are launched: phases can only be set while a solver is not solving */
	std::unique_ptr<saga::GeneticAlgorithm> gaInitializer;
	if (__globalParameters__.gaInitPeriod) {
		if (__globalParameters__.gaPopSize < solvers.size())
			__globalParameters__.gaPopSize = solvers.size();

		LOG0("GA Initialized");
		gaInitializer = std::make_unique<saga::GeneticAlgorithm>(__globalParameters__.gaPopSize,
																 varCount,
																 __globalParameters__.gaMaxGen,
																 __globalParameters__.gaMutRate,
																 __globalParameters__.gaCrossRate,
																 __globalParameters__.gaSeed,
																 clausesCount,
																 varCount,
																 initClauses);
		gaInitializer->solve();
		LOG0("GA Finished");
	}

	/* Solving */
	// Load formula in solvers in parallel using solverInitializers
	for (auto& solver : solvers) {
		SequentialWorker* myworker = new SequentialWorker(solver);
		this->addSlave(myworker);
		solverInitializers.emplace_back([myworker, &cube, solver, &initClauses, varCount, &gaInitializer] {
			solver->addInitialClauses(initClauses, varCount);

			// One solver in gaInitPeriod gets its phases from the GaspiInitializer.
			// !! Warning !! The getters return references: each solver reads a different solution.
			if (gaInitializer && !(solver->getSolverId() % __globalParameters__.gaInitPeriod)) {
				// 0 is the best solution, the first solver gets it
				unsigned solIdx = solver->getSolverId() / __globalParameters__.gaInitPeriod;
				LOGDEBUG1("Solver %u receiving solution %u", solver->getSolverId(), solIdx);
				saga::Solution& initPhases = gaInitializer->getNthSolution(solIdx);

				// Todo fix the +1 on solution size, it is confusing
				for (unsigned int i = 1; i < varCount; i++) {
					solver->setPhase(i, initPhases[i]);
				}
				LOGDEBUG1("Phases set for solver %u", solver->getSolverId());
			}

			myworker->solve(cube);
		});
	}

	// Wait for solver initialization
	for (auto& initializer : solverInitializers)
		initializer.join();

	LOG0("All solvers are fully initialized and launched");

	SharingStrategyFactory::launchSharers(sharingStrategiesConcat, this->sharers);

	initClauses.clear();
}

void
PortfolioSimple::join(WorkingStrategy* strat, BackboneResult res, const std::vector<int>& backbone)
{
	if (res == BackboneResult::UNKNOWN || strategyEnding)
		return;

	strategyEnding = true;

	setSolverInterrupt();

	if (parent == NULL) { // If it is the top strategy
		finalResult = res;
		globalEnding = true;

		if (res == BackboneResult::COMPLETE) {
			finalBackbone = backbone;
		}

		if (strat != this) {
			SequentialWorker* winner = (SequentialWorker*)strat;
			winner->solver->printWinningLog();
		}

		mutexGlobalEnd.lock();
		condGlobalEnd.notify_all();
		mutexGlobalEnd.unlock();
		LOGDEBUG1("Broadcasted the end");
	} else { // Else forward the information to the parent strategy
		parent->join(this, res, backbone);
	}
}

void
PortfolioSimple::setSolverInterrupt()
{
	for (size_t i = 0; i < slaves.size(); i++) {
		LOGDEBUG1("Interrupting slave %u", i);
		slaves[i]->setSolverInterrupt();
	}
}

void
PortfolioSimple::unsetSolverInterrupt()
{
	for (size_t i = 0; i < slaves.size(); i++) {
		slaves[i]->unsetSolverInterrupt();
	}
}

void
PortfolioSimple::waitInterrupt()
{
	for (size_t i = 0; i < slaves.size(); i++) {
		slaves[i]->waitInterrupt();
	}
}
