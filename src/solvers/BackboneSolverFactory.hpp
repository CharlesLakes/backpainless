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
 * Portfolio characters (-solver=<string>): 'c' CadiBack (CaDiCaL), 's' CadiBackSqrt (CadiBack block by block),
 * 'd' CadiBackPartition (one block of variables per d worker), 'x' DiverseBackboneSearch (random XOR exploration,
 * CryptoMiniSat), 'm' MaxSatBackbone (rounds of MaxSAT).
 */
class BackboneSolverFactory
{
  public:
	/// @brief The portfolio characters createSolver accepts, checked by Parameters::init before any thread starts.
	static constexpr const char* SOLVER_TYPES = "csdxm";

	/**
	 * @brief Creates one backbone solver.
	 * @param type Portfolio character of the solver.
	 * @param importDBType Character of the import database type (see ClauseDatabaseFactory).
	 * @return The created solver, nullptr when the maximum number of solvers is reached.
	 */
	static std::shared_ptr<BackboneSolverInterface> createSolver(char type, char importDBType);

	/**
	 * @brief Creates 'count' solvers by cycling through the characters of 'portfolio'.
	 * @param firstId Global id of the first solver (rank * count with MPI): the portfolio is indexed by the global id,
	 * like the other per-worker options.
	 */
	static void createSolvers(int count,
							  char importDBType,
							  const std::string& portfolio,
							  std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
							  int firstId = 0);

	/// @brief Prints the statistics table of the solvers.
	static void printStats(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers);

	/// @brief Creates the board of decided variables and connects the solvers selected by -bb-share-cand, by global
	/// id (call after diversification). Returns the board, nullptr when every worker has mode 0.
	static std::shared_ptr<CandidateBoard> connectCandidateBoard(
		const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers,
		unsigned int nbVars);

	/// @brief Gives every d worker (CadiBackPartition) its part: its index among the d workers sharing a board, and
	/// their number (call after the ids are set).
	static void setPartitions(const std::vector<std::shared_ptr<BackboneSolverInterface>>& solvers);

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
