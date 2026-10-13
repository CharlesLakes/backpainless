#include "solvers/BackboneSolverFactory.hpp"
#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/Parameters.hpp"

#include "solvers/CDCL/cadiback/CadiBack.hpp"
#include "solvers/CDCL/cadiback/CadiBackSqrt.hpp"
#include "solvers/CDCL/cryptominisat/DiverseBackboneSearch.hpp"
#include "solvers/XorOptions.hpp"

#include "containers/ClauseDatabases/ClauseDatabaseFactory.hpp"

#include <iomanip>
#include <iostream>

std::atomic<int> BackboneSolverFactory::currentIdSolver(0);

void
BackboneSolverFactory::diversification(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
									   const IDScaler& gIDScaler,
									   const IDScaler& typeIDScaler)
{
	/* Validated by Parameters::init */
	const std::string& shareMask = __globalParameters__.backboneShareUnits;
	std::vector<CandidateOrder> orders;
	parseCandidateOrderList(__globalParameters__.backboneOrder, orders);
	std::vector<unsigned long> chunkRates;
	parseChunkRateList(__globalParameters__.backboneChunkRate, chunkRates);
	XorOptionLists xorOptions;
	std::string xorError;
	parseXorOptionLists(xorOptions, xorError);

	for (auto solver : solvers) {
		solver->setSolverId(gIDScaler(solver));
		solver->setSolverTypeId(typeIDScaler(solver));
		/* Indexed by the global id, so that the mask also applies across MPI ranks */
		solver->setShareBackboneUnits(shareMask[solver->getSolverId() % shareMask.size()] == '1');
		solver->setCandidateOrder(orders[solver->getSolverId() % orders.size()]);
		solver->setChunkRate(chunkRates[solver->getSolverId() % chunkRates.size()]);
		LOGDEBUG1("Computing Id %d,%d, share backbone units: %d, candidate order: %s, chunk rate: %lu",
				  solver->getSolverId(),
				  solver->getSolverTypeId(),
				  solver->getShareBackboneUnits(),
				  candidateOrderName(solver->getCandidateOrder()),
				  solver->getChunkRate());
		solver->setXorOptions(xorOptions.forSolver(solver->getSolverId()));
	}

	for (auto solver : solvers) {
		solver->diversify();
	}

	LOG0("Diversification done");
}

void
BackboneSolverFactory::connectCandidateBoard(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
											 unsigned int nbVars)
{
	/* Validated by Parameters::init: characters in 0, 1, p (publish), c (consume) */
	const std::string& candMask = __globalParameters__.backboneShareCandidates;
	if (candMask.find_first_not_of('0') == std::string::npos)
		return;

	auto board = std::make_shared<CandidateBoard>(nbVars);
	for (auto solver : solvers) {
		/* Indexed by the global id, like -bb-share-units. The board itself is local to the process */
		char mode = candMask[solver->getSolverId() % candMask.size()];
		solver->setCandidateBoard(board, mode == '1' || mode == 'p', mode == '1' || mode == 'c');
		LOGDEBUG1("Solver %d candidate board: publish %d, consume %d",
				  solver->getSolverId(),
				  solver->getPublishCandidates(),
				  solver->getConsumeCandidates());
	}
}

std::shared_ptr<BackboneSolverInterface>
BackboneSolverFactory::createSolver(char type, char importDBType)
{
	int id = currentIdSolver.fetch_add(1);
	LOGDEBUG1("Creating Solver %d, type %c, importDB %c", id, type, importDBType);

	if (id >= __globalParameters__.cpus) {
		LOGWARN("Solver of type '%c' will not be instantiated, the number of solvers %d reached the maximum %d.",
				type,
				id,
				__globalParameters__.cpus);
		return nullptr;
	}

	std::shared_ptr<ClauseDatabase> importDB = ClauseDatabaseFactory::createDatabase(importDBType);

	switch (type) {
		case 'c':
			return std::make_shared<CadiBack>(id, importDB);
		case 's':
			return std::make_shared<CadiBackSqrt>(id, importDB);
		case 'x':
			return std::make_shared<DiverseBackboneSearch>(id, importDB);

		default:
			LOGERROR("The backbone solver type '%c' specified is not available!", type);
			exit(PERR_UNKNOWN_SOLVER);
	}
}

void
BackboneSolverFactory::createSolvers(int maxSolvers,
									 char importDBType,
									 const std::string& portfolio,
									 std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers)
{
	unsigned int typeCount = portfolio.size();
	LOGDEBUG1("Portfolio is '%s', of size %u", portfolio.c_str(), typeCount);
	for (size_t i = 0; i < maxSolvers && typeCount > 0; i++) {
		auto solver = createSolver(portfolio.at(i % typeCount), importDBType);
		if (solver)
			solvers.push_back(solver);
	}
}

void
BackboneSolverFactory::printStats(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers)
{
	lockLogger();
	std::cout << "c" << std::string(93, '-') << "\n";
	std::cout << "c" << std::left << std::setw(15) << "| ID" << std::setw(20) << "| Conflicts" << std::setw(20)
			  << "| Propagations" << std::setw(17) << "| SAT calls" << std::setw(20) << "| Backbone size"
			  << "|\n";
	std::cout << "c" << std::string(93, '-') << "\n";

	for (auto s : solvers) {
		s->printStatistics();
	}
	unlockLogger();
}
