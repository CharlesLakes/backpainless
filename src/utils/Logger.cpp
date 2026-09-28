#include "utils/Logger.hpp"
#include "utils/System.hpp"

#include "MpiUtils.hpp"
#include "utils/Parameters.hpp"
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

#include <atomic>
#include <cmath>
#include <mutex>

static std::recursive_mutex logMutex;

std::atomic<bool> quiet(false);

void
lockLogger()
{
	logMutex.lock();
}
void
unlockLogger()
{
	logMutex.unlock();
}

static int verbosityLevelSetting = 0;

void
setVerbosityLevel(int level)
{
	verbosityLevelSetting = level;
}

/**
 * Logging function
 * @param verbosityLevel to filter out some logs using program arguments
 * @param color in certain cases (debug and error) use different color for better readability
 * @param issuer the function name that logged this message (for debug and error)
 * @param fmt the message logged
 */
void
logDebug(int verbosityLevel, const char* color, const char* issuer, const char* fmt...)
{
	if (verbosityLevel <= verbosityLevelSetting && !quiet) {
		std::lock_guard<std::recursive_mutex> lockLog(logMutex);

		va_list args;

		va_start(args, fmt);

		printf("c%s", color);

		printf("[%.2f] ", SystemResourceMonitor::getRelativeTimeSeconds());

		if (__globalParameters__.enableDistributed)
			printf("[mpi:%d] ", mpi_rank);

		printf("%s(%s) %s%s%s", FUNC_STYLE, issuer, RESET, color, ERROR_STYLE);

		vprintf(fmt, args);

		va_end(args);

		printf("%s\n", RESET);

		fflush(stdout);
	}
}

void
log(int verbosityLevel, const char* color, const char* fmt...)
{
	if (verbosityLevel <= verbosityLevelSetting && !quiet) {
		std::lock_guard<std::recursive_mutex> lockLog(logMutex);

		va_list args;

		va_start(args, fmt);

		printf("c%s", color);

		printf("[%.2f] ", SystemResourceMonitor::getRelativeTimeSeconds());

		if (__globalParameters__.enableDistributed)
			printf("[mpi:%d] ", mpi_rank);

		vprintf(fmt, args);

		va_end(args);

		printf("%s\n", RESET);

		fflush(stdout);
	}
}

void
logClause(int verbosityLevel, const char* color, const int* lits, unsigned int size, const char* fmt...)
{
	if (verbosityLevel <= verbosityLevelSetting && !quiet) {

		va_list args;

		va_start(args, fmt);

		printf("cc%s", color);

		if (__globalParameters__.enableDistributed)
			printf("[mpi:%d] ", mpi_rank);

		vprintf(fmt, args);

		printf(" [%u] ", size);

		for (unsigned int i = 0; i < size; i++) {
			printf("%d ", lits[i]);
		}

		va_end(args);

		printf("%s", RESET);

		printf("\n");

		fflush(stdout);
	}
}

void
logSolution(const char* string)
{
	std::lock_guard<std::recursive_mutex> lockLog(logMutex);
	printf("s %s\n", string);
}

void
logBackbone(const std::vector<int>& backbone)
{
	/* CadiBack format: one 'b <lit>' line per backbone literal, terminated by 'b 0' */
	std::lock_guard<std::recursive_mutex> lockLog(logMutex);

	for (int lit : backbone) {
		printf("b %d\n", lit);
	}

	printf("b 0\n");
	fflush(stdout);
}