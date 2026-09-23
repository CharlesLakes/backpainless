#pragma once

#include "utils/Parameters.hpp"
#include "working/WorkingStrategy.hpp"

#include "solvers/BackboneSolverInterface.hpp"

#include "sharing/Sharer.hpp"

#include "sharing/GlobalStrategies/GlobalSharingStrategy.hpp"
#include "sharing/SharingStrategy.hpp"

#include <condition_variable>
#include <mutex>

/**
 * @brief A Simple Implementation of WorkingStrategy for the portfolio parallel strategy
 * This strategy uses the different factories BackboneSolverFactory and SharingStrategyFactory in order to instantiate the
 * different needed components specified by the parameters.
 * @ingroup working
 */
class PortfolioSimple : public WorkingStrategy
{
  public:
	PortfolioSimple();

	~PortfolioSimple();

	void solve(const std::vector<int>& cube) override;

	void join(WorkingStrategy* strat, BackboneResult res, const std::vector<int>& backbone) override;

	void setSolverInterrupt() override;

	void unsetSolverInterrupt() override;

	void waitInterrupt() override;

  protected:
	std::atomic<bool> strategyEnding;

	// Solvers
	//--------
	std::vector<std::shared_ptr<BackboneSolverInterface>> solvers;

	// Sharing
	//--------

	std::vector<std::shared_ptr<SharingStrategy>> localStrategies;
	std::vector<std::shared_ptr<GlobalSharingStrategy>> globalStrategies;
	std::vector<std::unique_ptr<Sharer>> sharers;
};