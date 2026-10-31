#include "solvers/BackboneSolverFactory.hpp"
#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/MpiUtils.hpp"
#include "utils/Parameters.hpp"

#include "solvers/CDCL/cadiback/CadiBack.hpp"
#include "solvers/CDCL/cadiback/CadiBackPartition.hpp"
#include "solvers/CDCL/cadiback/CadiBackSqrt.hpp"
#include "solvers/CDCL/cadiback/MaxSatBackbone.hpp"
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
	const std::string& learntMask = __globalParameters__.backboneShareLearnt;
	std::vector<CandidateOrder> orders;
	parseCandidateOrderList(__globalParameters__.backboneOrder, orders);
	std::vector<unsigned long> chunkRates;
	parseChunkRateList(__globalParameters__.backboneChunkRate, chunkRates);
	std::vector<unsigned long> maxSatRounds;
	parseChunkRateList(__globalParameters__.backboneMaxSatRounds, maxSatRounds);
	std::vector<unsigned long> maxSatPatience;
	parseChunkRateList(__globalParameters__.backboneMaxSatPatience, maxSatPatience);
	XorOptionLists xorOptions;
	std::string xorError;
	parseXorOptionLists(xorOptions, xorError);

	for (auto solver : solvers) {
		solver->setSolverId(gIDScaler(solver));
		solver->setSolverTypeId(typeIDScaler(solver));
		/* Indexed by the global id, so that the mask also applies across MPI ranks */
		solver->setShareBackboneUnits(shareMask[solver->getSolverId() % shareMask.size()] == '1');
		solver->setShareLearnt(learntMask[solver->getSolverId() % learntMask.size()] == '1');
		solver->setCandidateOrder(orders[solver->getSolverId() % orders.size()]);
		solver->setChunkRate(chunkRates[solver->getSolverId() % chunkRates.size()]);
		solver->setMaxSatRounds(maxSatRounds[solver->getSolverId() % maxSatRounds.size()]);
		solver->setMaxSatPatience(maxSatPatience[solver->getSolverId() % maxSatPatience.size()]);
		LOGDEBUG1("Computing Id %d,%d, share backbone units: %d, candidate order: %s, chunk rate: %lu",
				  solver->getSolverId(),
				  solver->getSolverTypeId(),
				  solver->getShareBackboneUnits(),
				  candidateOrderName(solver->getCandidateOrder()),
				  solver->getChunkRate());
		solver->setXorOptions(xorOptions.forSolver(solver->getSolverId()));
	}

	setPartitions(solvers);

	for (auto solver : solvers) {
		solver->diversify();
	}

	LOG0("Diversification done");
}

void
BackboneSolverFactory::setPartitions(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers)
{
	/* The parts are split among the d workers that share a board: all the ranks when the board is exchanged, else the
	 * process. -solver is cycled over the global ids (PortfolioSimple), so the d workers of every rank are known */
	const std::string& portfolio = __globalParameters__.solver;
	const bool global = __globalParameters__.enableDistributed && !__globalParameters__.backboneShareCandidatesLocal &&
						mpi_world_size > 1;
	unsigned int total = 0;
	if (global) {
		for (int id = 0; id < __globalParameters__.cpus * mpi_world_size; id++)
			total += portfolio[id % portfolio.size()] == 'd';
	} else {
		for (auto solver : solvers)
			total += solver->getSolverType() == BackboneSolverType::CADIBACK_PARTITION;
	}

	unsigned int localIndex = 0;
	for (auto solver : solvers) {
		auto partition = std::dynamic_pointer_cast<CadiBackPartition>(solver);
		if (!partition)
			continue;
		unsigned int index = localIndex++;
		if (global) {
			index = 0;
			for (unsigned int id = 0; id < solver->getSolverId(); id++)
				index += portfolio[id % portfolio.size()] == 'd';
		}
		partition->setPartition(index, total);
		LOGDEBUG1("Solver %d owns part %u of %u", solver->getSolverId(), index, total);
	}
}

std::shared_ptr<CandidateBoard>
BackboneSolverFactory::connectCandidateBoard(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
											 unsigned int nbVars)
{
	/* Validated by Parameters::init: characters in 0, 1, p (publish), c (consume) */
	const std::string& candMask = __globalParameters__.backboneShareCandidates;
	if (candMask.find_first_not_of('0') == std::string::npos)
		return nullptr;

	auto board = std::make_shared<CandidateBoard>(nbVars);
	for (auto solver : solvers) {
		/* Indexed by the global id, like -bb-share-units. With -dist the global strategy exchanges the board */
		char mode = candMask[solver->getSolverId() % candMask.size()];
		solver->setCandidateBoard(board, mode == '1' || mode == 'p', mode == '1' || mode == 'c');
		LOGDEBUG1("Solver %d candidate board: publish %d, consume %d",
				  solver->getSolverId(),
				  solver->getPublishCandidates(),
				  solver->getConsumeCandidates());
	}
	return board;
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
		case 'd':
			return std::make_shared<CadiBackPartition>(id, importDB);
		case 'x':
			return std::make_shared<DiverseBackboneSearch>(id, importDB);
		case 'm':
			return std::make_shared<MaxSatBackbone>(id, importDB);

		default:
			LOGERROR("The backbone solver type '%c' specified is not available!", type);
			exit(PERR_UNKNOWN_SOLVER);
	}
}

void
BackboneSolverFactory::createSolvers(int maxSolvers,
									 char importDBType,
									 const std::string& portfolio,
									 std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
									 int firstId)
{
	unsigned int typeCount = portfolio.size();
	LOGDEBUG1("Portfolio is '%s', of size %u", portfolio.c_str(), typeCount);
	for (size_t i = 0; i < maxSolvers && typeCount > 0; i++) {
		auto solver = createSolver(portfolio.at((firstId + i) % typeCount), importDBType);
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
