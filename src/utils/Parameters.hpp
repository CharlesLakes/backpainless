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
		  std::string,                                                                                                       \
		  "bb-chunk",                                                                                                        \
		  "0",                                                                                                               \
		  "Chunk rate K of the constraint, comma-separated list cycled over the solver ids (0 = all candidates, "            \
		  "1 = one-by-one, 10 = cadiback --chunking)")                                                                       \
	PARAM(backboneNoFlip, bool, "bb-no-flip", false, "Do not drop flippable literals from the backbone candidates")       \
	PARAM(backboneShareUnits,                                                                                             \
		  std::string,                                                                                                       \
		  "bb-share-units",                                                                                                  \
		  "1",                                                                                                               \
		  "Mask of 0/1 cycled over the solver ids: workers with 1 export their backbone literals as units (10 = even ids)")  \
	PARAM(backboneShareCandidates,                                                                                        \
		  std::string,                                                                                                       \
		  "bb-share-cand",                                                                                                   \
		  "0",                                                                                                               \
		  "Mask cycled over the solver ids for the board of decided variables: 0 off, 1 publish and consume, p publish, "    \
		  "c consume")                                                                                                       \
	/* NEW ORDER (5/6): add the name to this help and to DETAILED_HELP_BACKBONE below */                                \
	PARAM(backboneOrder,                                                                                                  \
		  std::string,                                                                                                       \
		  "bb-order",                                                                                                        \
		  "natural",                                                                                                         \
		  "Initial candidate order, comma-separated list cycled over the solver ids: natural, reverse, random, occ, "        \
		  "occ-rev")                                                                                                         \
	/* DiverseBackboneSearch (-solver=x): every option is a comma-separated list cycled over the solver ids */          \
	PARAM(backboneXorCount,                                                                                               \
		  std::string,                                                                                                       \
		  "bb-xor-m",                                                                                                        \
		  "10",                                                                                                              \
		  "Number m of random XOR constraints per round (1 to 64), the DFS tree has 2^m leaves")                            \
	PARAM(backboneXorDensity,                                                                                             \
		  std::string,                                                                                                       \
		  "bb-xor-density",                                                                                                  \
		  "0.5",                                                                                                             \
		  "Variables of each XOR: a probability in (0,1] (with a '.') per candidate variable, or an integer k >= 1 for a "  \
		  "fixed length")                                                                                                    \
	PARAM(backboneXorRounds,                                                                                              \
		  std::string,                                                                                                       \
		  "bb-xor-rounds",                                                                                                   \
		  "1",                                                                                                               \
		  "Rounds of XOR exploration (new XORs each round) before completing the backbone")                                \
	PARAM(backboneXorLeaves,                                                                                              \
		  std::string,                                                                                                       \
		  "bb-xor-leaves",                                                                                                   \
		  "0",                                                                                                               \
		  "Maximum number of nodes solved per round (0 = no limit)")                                                        \
	PARAM(backboneXorPre,                                                                                                 \
		  std::string,                                                                                                       \
		  "bb-xor-pre",                                                                                                      \
		  "0",                                                                                                               \
		  "Mask of 0/1 cycled over the solver ids: CaDiCaL workers (c, s) with 1 run the XOR exploration before CadiBack")\
	PARAM(backboneXorExport,                                                                                              \
		  std::string,                                                                                                       \
		  "bb-xor-export",                                                                                                   \
		  "0",                                                                                                               \
		  "Mask of 0/1 cycled over the solver ids: x workers with 1 also export their small learnt clauses (fixed "        \
		  "literals are always exported)")                                                                                   \
	PARAM(backboneXorDfs,                                                                                                 \
		  std::string,                                                                                                       \
		  "bb-xor-dfs",                                                                                                      \
		  "adaptive",                                                                                                        \
		  "DFS over the XOR tree: adaptive (solve inner nodes, go down after SAT, skip the subtree after a timeout) or "   \
		  "leaves (solve the 2^m leaves only)")                                                                              \
	PARAM(backboneXorConflicts,                                                                                           \
		  std::string,                                                                                                       \
		  "bb-xor-confl",                                                                                                    \
		  "2x",                                                                                                              \
		  "Conflict budget of each node, an exhausted node is skipped: '<f>x' = f times the conflicts of the first model "  \
		  "(at least 1000), an integer = absolute (0 = no limit)")                                                           \
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
		 " " BOLD "s" RESET " - CadiBackSqrt: CadiBack applied block by block, sqrt(#candidates) blocks\n"             \
		 "\n" BLUE "Import Database Types " YELLOW "(-importDB=<char>)" RESET " :\n" DETAILED_HELP_DATABASES "\n"      \
		 "Working strategy:\n" RESET " " BOLD "Simple Portfolio" RESET                                                 \
		 ": Run backbone solvers in parallel with diversified configurations, sharing learnt clauses\n"               \
		 "\n" BOLD "Example:" RESET " " YELLOW "-solver=c -c=8" RESET                                                  \
		 " creates a portfolio of 8 diversified CadiBack solvers\n"

#define DETAILED_HELP_BACKBONE                                                                                         \
	BLUE "Backbone extraction (CadiBack algorithm):\n" RESET "  " YELLOW "-bb-chunk" RESET                             \
		 ": Number of candidates negated in each constraint, one value per worker (comma-separated)\n"               \
		 "    " BOLD "0" RESET ": always all remaining candidates (default)\n"                                        \
		 "    " BOLD "1" RESET ": one-by-one\n"                                                                       \
		 "    " BOLD "K" RESET ": reset to 1 after a SAT answer, multiplied by K after an UNSAT answer\n"             \
		 "    e.g. " YELLOW "-bb-chunk=0,10" RESET ": even ids use 0, odd ids use 10\n"                                  \
		 "  " YELLOW "-bb-no-flip" RESET ": Do not use flippable literals to drop candidates\n"                       \
		 "  " YELLOW "-bb-share-cand" RESET ": Board of decided variables (backbone or free), one mode per worker\n"   \
		 "    " BOLD "0" RESET ": off (default)   " BOLD "1" RESET ": publish and consume\n"                            \
		 "    " BOLD "p" RESET ": publish only    " BOLD "c" RESET ": consume only\n"                                   \
		 "    The mask is cycled over the solver ids, e.g. " YELLOW "-bb-share-cand=1c" RESET                            \
		 ": even ids publish and consume, odd ids only consume\n"                                                     \
		 "  " YELLOW "-bb-order" RESET ": Order of the initial candidates, one name per worker (comma-separated)\n"     \
		 "    " BOLD "natural" RESET ": variable index (default)   " BOLD "reverse" RESET ": decreasing index\n"       \
		 "    " BOLD "random" RESET ": shuffled with the solver seed\n"                                                \
		 "    " BOLD "occ" RESET " / " BOLD "occ-rev" RESET                                                              \
		 ": most / least frequent candidate literals in the formula first\n"                                         \
		 "\n" BLUE "DiverseBackboneSearch (" YELLOW "-solver=x" BLUE ", CryptoMiniSat):\n" RESET                    \
		 "  Each round draws m linearly independent random XORs over the candidate variables and visits the 2^m\n"      \
		 "  nodes of the tree of their parities (DFS, a conflict prunes the subtree of its deepest XOR); every model\n"  \
		 "  drops candidates (free variables are published on the board). The backbone is then completed as CadiBack\n" \
		 "  (-bb-chunk, -bb-order). Options, one value per worker (comma-separated):\n"                                \
		 "  " YELLOW "-bb-xor-m" RESET ": XORs per round (1 to 64)   " YELLOW "-bb-xor-rounds" RESET ": rounds\n"        \
		 "  " YELLOW "-bb-xor-density" RESET ": " BOLD "0.5" RESET " (default) probability per variable, " BOLD "k" RESET \
		 " integer: fixed length\n"                                                                                   \
		 "  " YELLOW "-bb-xor-leaves" RESET ": nodes per round (0 = no limit)   " YELLOW "-bb-xor-confl" RESET           \
		 ": conflicts per node, " BOLD "2x" RESET " (default) twice the first model, or absolute\n"                                                                       \
		 "  " YELLOW "-bb-xor-dfs" RESET ": " BOLD "adaptive" RESET                                                      \
		 " (default) solves the inner nodes with the first d XORs: SAT goes down, UNSAT prunes, an\n"                    \
		 "    exhausted budget skips the subtree;  " BOLD "leaves" RESET ": solves the 2^m leaves only\n"                \
		 "  " YELLOW "-bb-xor-pre" RESET ": 1 makes a CaDiCaL worker (c, s) run the same exploration first, then the\n"  \
		 "    usual CadiBack (default 0; XORs in CNF, guarded by a round literal removed at the end)\n"                \
		 "  " YELLOW "-bb-xor-export" RESET ": 1 also exports the learnt clauses (default 0: CryptoMiniSat gives no\n"    \
		 "    glue, they flood the other workers; units, backbone and free variables are always shared)\n"              \
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