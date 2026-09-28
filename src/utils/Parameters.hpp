#pragma once

#include "Logger.hpp"
#include <iostream>
#include <sstream>
#include <string>

/**
 * @defgroup utils Utilities
 * @brief Different Utilities
 * @{
 */

// Define the macro for parameters, TODO: add max and min values for a better user experience
// name, type, parsed_name, default, description
#define PARAMETERS                                                                                                     \
	/* General options */                                                                                              \
	PARAM(help, bool, "help", false, "Prints this help")                                                               \
	PARAM(details, std::string, "details", "", "Get detailed information about a specific category")                   \
	PARAM(filename, std::string, "input.cnf ", "", "Input CNF file")                                                   \
	PARAM(cpus, int, "c", 0, "Number of solver threads to launch (0 = std::thread::hardware_concurrency)")             \
	PARAM(timeout, int, "t", -1, "Timeout in seconds")                                                                 \
	PARAM(verbosity, int, "v", 0, "Verbosity level")                                                                   \
	PARAM(noBackbone, bool, "no-backbone", false, "Only report the status of the formula, not the backbone")              \
	PARAM(enableDistributed, bool, "dist", false, "Enable distributed solving, thus initializes MPI")                     \
                                                                                                                       \
	CATEGORY("Backbone")                                                                                                  \
	PARAM(backboneChunkRate,                                                                                              \
		  int,                                                                                                               \
		  "bb-chunk",                                                                                                        \
		  0,                                                                                                                 \
		  "Chunk rate K of the constraint (0 = all candidates, 1 = one-by-one, 10 = cadiback --chunking)")                   \
	PARAM(backboneNoFlip, bool, "bb-no-flip", false, "Do not drop flippable literals from the backbone candidates")       \
                                                                                                                       \
	CATEGORY("Portfolio")                                                                                                 \
	PARAM(solver, std::string, "solver", "c", "Portfolio of backbone solvers")                                            \
	PARAM(enableMallob, bool, "mallob", false, "Emulate Mallob's Sharing Strategy In PortfolioSimple")                    \
	PARAM(defaultClauseBufferSize, int, "default-clsbuff-size", 1000, "Default ClauseBuffer size")                        \
	PARAM(gaInitPeriod,                                                                                                \
		  int,                                                                                                         \
		  "ga-init",                                                                                                   \
		  0,                                                                                                           \
		  "Use GaspiInitializer for phase initialization. (0) disabled, (n) all id where (id %% n) == 0")              \
	PARAM(gaSeed, int, "ga-seed", 0, "The seed to use in the random engine")                                           \
	PARAM(gaPopSize, int, "ga-pop-size", 50, "The number of candidates in each generation")                            \
	PARAM(gaMaxGen, int, "ga-max-gen", 100, "The maximum number of generations (iterations) to run")                   \
	PARAM(                                                                                                             \
		gaMutRate, float, "ga-mut-rate", 0.88f, "The mutation rate (probability), chances to randomly assign a genome") \
	PARAM(gaCrossRate,                                                                                                 \
		  float,                                                                                                       \
		  "ga-cross-rate",                                                                                             \
		  0.5f,                                                                                                        \
		  "The crossover rate (probability), i.e chances to create a crossover point")                                 \
                                                                                                                       \
	CATEGORY("Sharing")                                                                                                \
	PARAM(maxClauseSize, int, "max-cls-size", 60, "Maximum size of clauses to be added in ClauseDatabase")             \
	PARAM(initSleep, int, "init-sleep", 10'000, "Initial sleep time in microseconds for a Sharer")                     \
	PARAM(sharingStrategy, int, "shr-strat", 1, "Strategy selection for local sharing (ongoing re-organization)")      \
	PARAM(globalSharingStrategy, int, "gshr-strat", -1, "Global sharing strategy")                                      \
	PARAM(sharingSleep, int, "shr-sleep", 500'000, "Sleep time for sharer after each round")                           \
	PARAM(globalSharingSleep, int, "gshr-sleep", 600'000, "Sleep time for sharer after each round of global sharing")  \
	PARAM(oneSharer, bool, "one-sharer", false, "Use only one sharer")                                                 \
	PARAM(globalSharedLiterals, int, "gshr-lit", 2000, "Number of literals shared globally")                           \
	PARAM(sharedLiteralsPerProducer,                                                                                   \
		  int,                                                                                                         \
		  "shr-lit-per-prod",                                                                                          \
		  1500,                                                                                                        \
		  "Number of literals shared per producer. It is mainly used for local sharing")                               \
	PARAM(simpleShareLimit, int, "simple-limit", 10, "Simple share clause size limit")                                 \
	PARAM(importDB, std::string, "importDB", "d", "Solver import dabatase type")                                       \
	PARAM(importDBCap, unsigned, "importDB-cap", 10'000, "Solver import dabatase capacity")                            \
	PARAM(localSharingDB, std::string, "lshrDB", "d", "Local Sharing Strategy import dabatase type")                   \
	PARAM(globalSharingDB, std::string, "gshrDB", "m", "Global Sharing Strategy import dabatase type")                 \
                                                                                                                       \
	SUBCATEGORY("Hordesat")                                                                                            \
	PARAM(hordeInitialLbdLimit, unsigned, "horde-initial-lbd", 2, "Initial LBD value for producers")                   \
	PARAM(hordeInitRound, unsigned, "horde-init-round", 1, "Rounds before HordesatSharingAlt starts")                  \
                                                                                                                       \
	SUBCATEGORY("Mallob")                                                                                              \
	PARAM(mallobSharingsPerSecond, int, "mallob-shr-per-sec", 2, "Number of shares per second")                        \
	PARAM(mallobMaxBufferSize, int, "mallob-gshr-max-lit", 250'000, "Maximum number of literals shared globally")      \
	PARAM(mallobResharePeriod,                                                                                         \
		  int,                                                                                                         \
		  "mallob-reshare-period-us",                                                                                  \
		  15'000'000,                                                                                                  \
		  "Reshare period in microseconds for ExactFilter")                                                            \
	PARAM(mallobLBDLimit, int, "mallob-lbd-limit", 60, "Mallob LBD limit")                                             \
	PARAM(mallobSizeLimit, int, "mallob-size-limit", 60, "Mallob size limit")                                          \
	PARAM(mallobMaxCompensation, float, "max-mallob-comp", 5.0f, "Maximum Mallob compensation")

// Structure to hold all parameters
struct Parameters
{
#define PARAM(name, type, parsed_name, default_value, description) type name = default_value;
#define CATEGORY(description)
#define SUBCATEGORY(description)
	PARAMETERS
#undef PARAM
#undef CATEGORY
#undef SUBCATEGORY

	static void init(int argc, char** argv);
	static void printHelp();
	static void printDetailedHelp(std::string& category);
	static void printParams();
};

extern Parameters __globalParameters__;

#define DETAILED_HELP_DATABASES                                                                                        \
	" " BOLD "s" RESET " - SingleBuffer database\n"                                                                    \
	" " BOLD "m" RESET " - Mallob database\n"                                                                          \
	" " BOLD "d" RESET " - PerSize database (default)\n"                                                               \
	" " BOLD "e" RESET " - A Buffer Per Source (.from attribute) Database\n"

#define DETAILED_HELP_PORTFOLIO                                                                                        \
	BLUE "The solver parameter " YELLOW "(-solver=<string>)" BLUE " accepts the following characters:\n" RESET         \
		 " " BOLD "c" RESET " - CadiBack backbone solver (CaDiCaL)\n"                                                  \
		 "\n" BLUE "Import Database Types " YELLOW "(-importDB=<char>)" RESET " :\n" DETAILED_HELP_DATABASES "\n"      \
		 "Working strategy:\n" RESET " " BOLD "Simple Portfolio" RESET                                                 \
		 ": Run backbone solvers in parallel with diversified configurations, sharing learnt clauses\n"               \
		 "\n" BOLD "Example:" RESET " " YELLOW "-solver=c -c=8" RESET                                                  \
		 " creates a portfolio of 8 diversified CadiBack solvers\n"

#define DETAILED_HELP_BACKBONE                                                                                         \
	BLUE "Backbone extraction (CadiBack algorithm):\n" RESET "  " YELLOW "-bb-chunk" RESET                             \
		 ": Number of candidates negated in each constraint\n"                                                        \
		 "    " BOLD "0" RESET ": all remaining candidates (default, as cadiback)\n"                                  \
		 "    " BOLD "1" RESET ": one-by-one\n"                                                                       \
		 "    " BOLD "K" RESET ": reset to 1 after a SAT answer, multiplied by K after an UNSAT answer\n"             \
		 "  " YELLOW "-bb-no-flip" RESET ": Do not use flippable literals to drop candidates\n"                       \
		 "\n" BLUE "Output:\n" RESET "  'b <lit>' lines followed by 'b 0' (disable with " YELLOW "-no-backbone" RESET \
		 ")\n"

#define DETAILED_HELP_SHARING                                                                                          \
	BLUE "Local Sharing Strategies " YELLOW "(-shr-strat)" BLUE ":\n" RESET "  " BOLD "1" RESET                        \
		 ": HordeSat sharing with per-entity buffer (default)\n"                                                       \
		 "  " BOLD "1" RESET ": HordeSat sharing\n"                                                                    \
		 "  " BOLD "2" RESET ": HordeSat sharing with 2 groups of producers\n"                                         \
		 "  " BOLD "3" RESET ": Simple sharing \n"                                                                     \
		 "\n" BLUE "Global Sharing Strategies " YELLOW "(-gshr-strat)" BLUE ":\n" RESET "  " BOLD "1" RESET            \
		 ": AllGatherSharing - Exchange clauses using MPI_Allgather (default)\n"                                       \
		 "  " BOLD "2" RESET ": MallobSharing - Mallob-based exchange algorithm (adaptive)\n"                          \
		 "  " BOLD "3" RESET ": GenericGlobalSharing (Ring topology)\n"                                                \
		 "\n" BLUE "Clause Database Types " YELLOW "(-lshrDB, -gshrDB)" RESET ":\n" DETAILED_HELP_DATABASES "\n"       \
		 "Size and quality limits:\n" RESET "  " YELLOW "-max-cls-size" RESET ": Maximum clause size to share\n"       \
		 "  " YELLOW "-shr-lit-per-prod" RESET ": Literals per producer for local sharing\n"                           \
		 "  " YELLOW "-gshr-lit" RESET ": Number of literals shared globally\n"

#define DETAILED_HELP_GLOBAL                                                                                           \
	BLUE "General parameters:\n" RESET "  " YELLOW "-c" RESET ": Number of solver threads to launch (default: " GREEN  \
		 "32" RESET ")\n"                                                                                              \
		 "  " YELLOW "-t" RESET ": Timeout in seconds (" GREEN "-1" RESET " = no timeout)\n"                           \
		 "  " YELLOW "-v" RESET ": Verbosity level (" GREEN "0-5" RESET ")\n"                                          \
		 "\n" BLUE "Distributed solving:\n" RESET "  " YELLOW "-dist" RESET ": Enable distributed solving using MPI\n" \
		 "  Each node runs its own solvers and participates in global clause sharing\n"                                \
		 "\n" BLUE "Output options:\n" RESET "  " YELLOW "-no-backbone" RESET                                          \
		 ": Only report the status of the formula, not the backbone\n"
/**
 * @} // end of utils group
 */