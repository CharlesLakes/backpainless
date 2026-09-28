#pragma once

#include "solvers/BackboneSolverInterface.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Maps a backbone solver to an id (used to scale ids in distributed mode).
 * @ingroup solving
 */
using IDScaler = std::function<unsigned(const std::shared_ptr<BackboneSolverInterface>&)>;

/**
 * @brief Factory creating and diversifying the backbone solvers of the portfolio.
 * @ingroup solving
 *
 * Portfolio characters (-solver=<string>): 'c' CadiBack (CaDiCaL).
 */
class BackboneSolverFactory
{
  public:
	/**
	 * @brief Creates one backbone solver.
	 * @param type Portfolio character of the solver.
	 * @param importDBType Character of the import database type (see ClauseDatabaseFactory).
	 * @return The created solver, nullptr when the maximum number of solvers is reached.
	 */
	static std::shared_ptr<BackboneSolverInterface> createSolver(char type, char importDBType);

	/**
	 * @brief Creates 'count' solvers by cycling through the characters of 'portfolio'.
	 */
	static void createSolvers(int count,
							  char importDBType,
							  const std::string& portfolio,
							  std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers);

	/// @brief Prints the statistics table of the solvers.
	static void printStats(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers);

	/// @brief Sets the ids of the solvers using the scalers, then calls their native diversification.
	static void diversification(
		const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
		const IDScaler& generalIdScaler =
			[](const std::shared_ptr<BackboneSolverInterface>& solver) { return solver->getSolverId(); },
		const IDScaler& typeIdScaler =
			[](const std::shared_ptr<BackboneSolverInterface>& solver) { return solver->getSolverTypeId(); });

  public:
	static std::atomic<int> currentIdSolver;
};
