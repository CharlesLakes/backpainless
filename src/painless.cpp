#include "painless.hpp"

#include "utils/ErrorCodes.hpp"
#include "utils/Logger.hpp"
#include "utils/MpiUtils.hpp"
#include "utils/Parameters.hpp"
#include "utils/Parsers.hpp"
#include "utils/System.hpp"

#include "working/PortfolioSimple.hpp"

#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <random>
#include <thread>
#include <unistd.h>

// -------------------------------------------
// Signal Handling
// -------------------------------------------
#include <csignal>

// Cleanup function to be called at exit
void
cleanup()
{
	SystemResourceMonitor::printProcessResourceUsage();
}
// Signal handler function
void
signalHandler(int signum)
{
	// Call cleanup directly for immediate signals
	cleanup();

	// Re-raise the signal after cleaning up
	std::signal(signum, SIG_DFL); /* Use defailt signal handler */
	std::raise(signum);
}

// Crash handler: prints the signal, the memory of the process and a backtrace on stderr, then dies with the same
// signal. Only async-signal-safe calls (write, open, read, close, strlen, strncmp, backtrace_symbols_fd); the
// backtrace library is loaded in advance by setupExitHandlers. The frames are "binary(+offset)":
// addr2line -f -C -e <binary> <offset> names them.
void
crashHandler(int signum)
{
	auto put = [](const char* s, size_t n) { ssize_t r = write(STDERR_FILENO, s, n); (void)r; };
	auto puts = [&put](const char* s) { put(s, strlen(s)); };
	char num[16];
	int len = 0;
	for (int v = signum; v > 0 || len == 0; v /= 10)
		num[len++] = '0' + v % 10;
	puts("\nc CRASH: signal ");
	while (len)
		put(&num[--len], 1);
	puts(signum == SIGSEGV ? " (segmentation fault)\n" : "\n");

	/* VmPeak / VmHWM / VmRSS: a crash after running out of memory shows here */
	int fd = open("/proc/self/status", O_RDONLY);
	if (fd >= 0) {
		char buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf));
		close(fd);
		for (ssize_t start = 0, i = 0; i < n; i++) {
			if (buf[i] != '\n')
				continue;
			const char* l = buf + start;
			if (!strncmp(l, "VmPeak", 6) || !strncmp(l, "VmHWM", 5) || !strncmp(l, "VmRSS", 5)) {
				puts("c CRASH: ");
				put(l, i + 1 - start);
			}
			start = i + 1;
		}
	}

	void* frames[64];
	int count = backtrace(frames, 64);
	puts("c CRASH: backtrace\n");
	backtrace_symbols_fd(frames, count, STDERR_FILENO);

	std::signal(signum, SIG_DFL);
	std::raise(signum);
}

// Function to set up exit handlers
void
setupExitHandlers()
{
	// Register cleanup function to be called at normal program termination
	std::atexit(cleanup);

	// Set up signal handlers
	std::signal(SIGINT, signalHandler);
	std::signal(SIGTERM, signalHandler);
	std::signal(SIGABRT, signalHandler);

	/* backtrace() loads libgcc on its first call, which must not happen inside the handler */
	void* frame;
	backtrace(&frame, 1);
	for (int signum : { SIGSEGV, SIGBUS, SIGFPE, SIGILL })
		std::signal(signum, crashHandler);
}

// -------------------------------------------
// Declaration of global variables
// -------------------------------------------

std::atomic<bool> globalEnding(false);
std::mutex mutexGlobalEnd;
std::condition_variable condGlobalEnd;

// int nSharers = 0;

WorkingStrategy* working = NULL;

std::atomic<bool> dist = false;

std::atomic<BackboneResult> finalResult = BackboneResult::UNKNOWN;

std::vector<int> finalBackbone;

// -------------------------------------------
// Main of the framework
// -------------------------------------------
int
main(int argc, char** argv)
{

	Parameters::init(argc, argv);

	setupExitHandlers();

	dist = __globalParameters__.enableDistributed;

	// Ram Monitoring

	if (dist) {
		// MPI Initialization
		int provided;
		TESTRUNMPI(MPI_Init_thread(NULL, NULL, MPI_THREAD_SERIALIZED, &provided));
		TESTRUNMPI(MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN));

		LOGDEBUG1("Thread strategy provided is %d", provided);

		if (provided < MPI_THREAD_SERIALIZED) {
			LOGERROR("Wanted MPI initialization is not possible !");
			dist = false;
		} else {
			TESTRUNMPI(MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank));

			char hostname[256];
			gethostname(hostname, sizeof(hostname));
			LOGDEBUG1("PID %d on %s is of rank %d", getpid(), hostname, mpi_rank);

			TESTRUNMPI(MPI_Comm_size(MPI_COMM_WORLD, &mpi_world_size));
		}
	}

	if (mpi_rank <= 0)
		Parameters::printParams();

	// Init timeout detection before starting the solvers and sharers
	std::unique_lock<std::mutex> lock(mutexGlobalEnd);
	// to make sure that the broadcast is done when main has done its wait

	working = new PortfolioSimple();

	// Launch working
	std::vector<int> cube;

	std::thread mainWorker(&WorkingStrategy::solve, (WorkingStrategy*)working, std::ref(cube));

	int wakeupRet = 0;

	if (__globalParameters__.timeout > 0) {
		auto startTime = SystemResourceMonitor::getRelativeTimeSeconds();

		// Wait until end or __globalParameters__.timeout
		while ((unsigned int)SystemResourceMonitor::getRelativeTimeSeconds() < __globalParameters__.timeout &&
			   globalEnding == false) // to manage the spurious wake ups
		{
			auto remainingTime = std::chrono::duration<double>(
				__globalParameters__.timeout - (SystemResourceMonitor::getRelativeTimeSeconds() - startTime));
			auto wakeupStatus = condGlobalEnd.wait_for(lock, remainingTime);

			LOGDEBUG2("main wakeupRet = %s , globalEnding = %d ",
					  (wakeupStatus == std::cv_status::timeout ? "timeout" : "notimeout"),
					  globalEnding.load());
		}

		condGlobalEnd.notify_all();
		lock.unlock();

		if ((unsigned int)SystemResourceMonitor::getRelativeTimeSeconds() >= __globalParameters__.timeout &&
			finalResult ==
				BackboneResult::UNKNOWN) // if __globalParameters__.timeout set globalEnding otherwise a solver woke me up
		{
			globalEnding = true;
			finalResult = BackboneResult::TIMEOUT;
		}
	} else {
		// no __globalParameters__.timeout waiting
		while (globalEnding == false) // to manage the spurious wake ups
		{
			condGlobalEnd.wait(lock);
		}

		condGlobalEnd.notify_all();
		lock.unlock();
	}

	mainWorker.join();

	delete working;

	if (dist) {
		TESTRUNMPI(MPI_Finalize());
	}

	if (mpi_rank == mpi_winner) {
		if (finalResult == BackboneResult::COMPLETE) {
			logSolution("SATISFIABLE");
			LOGSTAT("Backbone size: %zu", finalBackbone.size());

			if (__globalParameters__.noBackbone == false) {
				logBackbone(finalBackbone);
			}
		} else if (finalResult == BackboneResult::UNSAT) {
			logSolution("UNSATISFIABLE");
		} else // if __globalParameters__.timeout or unknown
		{
			logSolution("UNKNOWN");
			finalResult = BackboneResult::UNKNOWN;
		}

		LOGSTAT("Resolution time: %f s", SystemResourceMonitor::getRelativeTimeSeconds());
	} else
		finalResult = BackboneResult::UNKNOWN; /* mpi will be forced to suspend job only by the winner */

	LOGDEBUG1("Mpi process %d returns %d", mpi_rank, static_cast<int>(finalResult.load()));

	return static_cast<int>(finalResult.load());
}
