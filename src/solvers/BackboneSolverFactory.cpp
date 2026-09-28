#include "solvers/BackboneSolverFactory.hpp"
#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/Parameters.hpp"

#include "solvers/CDCL/CadiBack.hpp"

#include "containers/ClauseDatabases/ClauseDatabaseFactory.hpp"

#include <iomanip>
#include <iostream>

std::atomic<int> BackboneSolverFactory::currentIdSolver(0);

void
BackboneSolverFactory::diversification(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
									   const IDScaler& gIDScaler,
									   const IDScaler& typeIDScaler)
{
	for (auto solver : solvers) {
		solver->setSolverId(gIDScaler(solver));
		solver->setSolverTypeId(typeIDScaler(solver));
		LOGDEBUG1("Computing Id %d,%d", solver->getSolverId(), solver->getSolverTypeId());
	}

	for (auto solver : solvers) {
		solver->diversify();
	}

	LOG0("Diversification done");
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
