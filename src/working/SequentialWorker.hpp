#pragma once

#include "solvers/BackboneSolverInterface.hpp"
#include "utils/Threading.hpp"
#include "working/WorkingStrategy.hpp"

#include <vector>

// Main executed by worker threads
static void*
mainWorker(void* arg);

/**
 * @brief Basic Implementation of WorkingStrategy for a sequential execution
 * @ingroup working
 */
class SequentialWorker : public WorkingStrategy
{
  public:
	SequentialWorker(std::shared_ptr<BackboneSolverInterface> solver_);

	~SequentialWorker();

	void solve(const std::vector<int>& cube);

	void join(WorkingStrategy* winner, BackboneResult res, const std::vector<int>& backbone);

	void setSolverInterrupt();

	void unsetSolverInterrupt();

	void waitInterrupt();

	std::shared_ptr<BackboneSolverInterface> solver;

  protected:
	friend void* mainWorker(void* arg);

	Thread* worker;

	std::vector<int> actualCube;

	std::atomic<bool> force;

	std::atomic<bool> waitJob;

	Mutex waitInterruptLock;

	pthread_mutex_t mutexStart;
	pthread_cond_t mutexCondStart;
};
