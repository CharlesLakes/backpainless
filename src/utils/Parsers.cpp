#include "utils/Parsers.hpp"

#include <cassert>
#include <ctype.h>
#include <math.h>
#include <stdio.h>

#include "ErrorCodes.hpp"
#include "Logger.hpp"
#include "NumericConstants.hpp"
#include "Parameters.hpp"
#include "painless.hpp"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace Parsers {
bool
RedundancyFilter::initMembers(unsigned int varCount, unsigned int clauseCount)
{
	return true;
}

bool
RedundancyFilter::operator()(simpleClause& clause)
{
	std::sort(clause.begin(), clause.end());
	auto newLast = std::unique(clause.begin(), clause.end());
	clause.erase(newLast, clause.end());
	return clauseCache.insert(clause).second;
}

bool
TautologyFilter::initMembers(unsigned int varCount, unsigned int clauseCount)
{
	return true;
}

bool
TautologyFilter::operator()(simpleClause& clause)
{
	std::unordered_set<int> literals;
	for (int lit : clause) {
		if (literals.count(-lit) != 0)
			return false;
		literals.insert(lit);
	}
	return true;
}

// Parser Helpers
// Utility functions
inline char
skipWhitespace(FILE* f)
{
	char c;
	while (isspace(c = fgetc(f)))
		;
	return c;
}

inline void
skipLine(FILE* f)
{
	char c;
	while ((c = fgetc(f)) != '\n' && c != EOF)
		;
}

inline int
parseNumber(FILE* f, int firstDigit)
{
	int num = firstDigit - '0';
	char c;
	while (isdigit(c = fgetc(f))) {
		num = num * 10 + (c - '0');
	}
	ungetc(c, f); // Put back the non-digit character
	return num;
}

// Parse the problem definition line
bool
parseCNFParameters(FILE* f, unsigned int& varCount, unsigned int& clauseCount)
{
	char c;
	while ((c = skipWhitespace(f)) != EOF) {
		if (c == 'c') {
			skipLine(f);
			continue;
		}
		if (c == 'p') {
			// Skip "\s*c"
			c = skipWhitespace(f);
			// Skip "nf"
			for (int i = 0; i < 2; i++) {
				if (fgetc(f) == EOF) {
					LOGERROR("EOF Detected to early");
					return false;
				}
			}

			c = skipWhitespace(f);
			if (!isdigit(c)) {
				LOGERROR("Unexpected character, %c", c);
				return false;
			}
			varCount = parseNumber(f, c);

			c = skipWhitespace(f);
			if (!isdigit(c)) {
				LOGERROR("Unexpected character, %c", c);
				return false;
			}
			clauseCount = parseNumber(f, c);

			return true;
		}
	}

	LOGERROR("p character not detected");
	return false;
}

// Parse a single clause
static bool
parseClause(FILE* f, simpleClause& cls)
{
	cls.clear();
	char c;
	bool neg = false;

	while ((c = skipWhitespace(f)) != EOF) {
		if (c == 'c') {
			skipLine(f);
			continue;
		}
		if (c == '-') {
			neg = true;
			continue;
		}
		if (isdigit(c)) {
			int num = parseNumber(f, c);
			if (neg) {
				num = -num;
				neg = false;
			}
			if (num == 0) {
				LOGCLAUSE2(&cls[0], cls.size(), "Parsed the clause:");
				return true; // End of clause
			}
			cls.push_back(num);
		} else {
			LOGERROR("Unexpected character, %c", c);
			return false;
		}
	}
	return false;
}

bool
parseCNF(const char* filename, Formula& parsedFormula, const std::vector<std::unique_ptr<ClauseProcessor>>& processors)
{
	FILE* f = fopen(filename, "r");
	if (f == NULL) {
		LOGERROR("Couldn't open file: %s", filename);
		return false;
	}

	unsigned int parsedClauseCount = 0, parsedVarCount = 0, filteredOutCount = 0;
	if (!parseCNFParameters(f, parsedVarCount, parsedClauseCount)) {
		fclose(f);
		return false;
	}

	parsedFormula.setVarCount(parsedVarCount);

	for (auto& processor : processors) {
		if (!processor->initMembers(parsedVarCount, parsedClauseCount)) {
			LOGERROR("Error at member initialization of processor %s", typeid(*processor).name());
			exit(PERR_PARSING);
		}
	}

	simpleClause cls;
	while (parseClause(f, cls)) {
		if (!cls.empty()) {
			bool keepClause = true;
			for (auto& processor : processors) {
				if (!(keepClause = processor->operator()(cls))) {
					filteredOutCount++;
					break;
				}
			}
			if (keepClause && !parsedFormula.push_clause(std::move(cls))) {
				finalResult = BackboneResult::UNSAT;
				LOGDEBUG1("Parse stopping because of UNSAT");
				fclose(f);
				return true;
			}
		}
	}

	fclose(f);

	assert(parsedClauseCount - filteredOutCount == parsedFormula.getAllClauseCount());
	LOG0("Successfully parsed %u clauses (filtered out: %u) with %u variables in %s.",
		 parsedClauseCount,
		 filteredOutCount,
		 parsedVarCount,
		 filename);

	return true;
}

bool
parseCNF(const char* filename,
		 std::vector<simpleClause>& clauses,
		 unsigned int* varCount,
		 const std::vector<std::unique_ptr<ClauseProcessor>>& processors)
{

	FILE* f = fopen(filename, "r");
	if (f == NULL) {
		LOGERROR("Couldn't open file: %s", filename);
		return false;
	}

	unsigned int parsedClauseCount = 0, parsedVarCount = 0, filteredOutCount = 0;
	if (!parseCNFParameters(f, parsedVarCount, parsedClauseCount)) {
		fclose(f);
		return false;
	}

	*varCount = parsedVarCount;

	for (auto& processor : processors) {
		if (!processor->initMembers(parsedVarCount, parsedClauseCount)) {
			LOGERROR("Error at member initialization of processor %s", typeid(*processor).name());
			exit(PERR_PARSING);
		}
	}

	simpleClause cls;
	while (parseClause(f, cls)) {
		if (!cls.empty()) {
			bool keepClause = true;
			for (auto& processor : processors) {
				if (!(keepClause = processor->operator()(cls))) {
					filteredOutCount++;
					break;
				}
			}
			if (keepClause)
				clauses.push_back(std::move(cls));
		} else {
			/* The empty clause makes the formula UNSAT: keep it, the solvers will answer UNSAT */
			clauses.push_back(std::move(cls));
		}

		cls.clear();
	}

	fclose(f);

	assert(parsedClauseCount - filteredOutCount == clauses.size());
	LOG0("Successfully parsed %u clauses (filtered out: %u) with %u variables in %s.",
		 parsedClauseCount,
		 filteredOutCount,
		 parsedVarCount,
		 filename);

	return true;
}

bool
parseCNF(const char* filename,
		 std::vector<lit_t>& literals,
		 unsigned int* varCount,
		 unsigned int* clsCount,
		 const std::vector<std::unique_ptr<ClauseProcessor>>& processors)
{
	FILE* f = fopen(filename, "r");
	if (f == NULL) {
		LOGERROR("Couldn't open file: %s", filename);
		return false;
	}

	unsigned int parsedClauseCount = 0, parsedVarCount = 0, filteredOutCount = 0, clsCount_ = 0;
	size_t litCounts = 0;
	if (!parseCNFParameters(f, parsedVarCount, parsedClauseCount)) {
		fclose(f);
		return false;
	}

	*varCount = parsedVarCount;

	for (auto& processor : processors) {
		if (!processor->initMembers(parsedVarCount, parsedClauseCount)) {
			LOGERROR("Error at member initialization of processor %s", typeid(*processor).name());
			exit(PERR_PARSING);
		}
	}

	simpleClause cls;
	while (parseClause(f, cls)) {
		if (!cls.empty()) {
			bool keepClause = true;
			for (auto& processor : processors) {
				if (!(keepClause = processor->operator()(cls))) {
					filteredOutCount++;
					break;
				}
			}
			if (keepClause) {
				literals.insert(literals.end(), cls.begin(), cls.end());
				literals.push_back(0);
				litCounts+=cls.size();
				clsCount_++;
			}
		}

		cls.clear();
	}

	fclose(f);

	assert(parsedClauseCount - filteredOutCount == clsCount_);

	*clsCount = clsCount_;

	LOG0("Successfully parsed %u clauses (filtered out: %u) with %u variables in %s.",
		 parsedClauseCount,
		 filteredOutCount,
		 parsedVarCount,
		 filename);

	return true;
}

bool
parseModel(const char* filename, unsigned int varCount, std::vector<int>& model)
{
	std::ifstream in(filename);
	if (!in) {
		LOGERROR("Couldn't open model file: %s", filename);
		return false;
	}

	std::vector<int> value(varCount + 1, 0);
	std::string line;
	unsigned long lineNumber = 0;
	bool sawV = false, sawPlain = false;
	while (std::getline(in, line)) {
		lineNumber++;
		size_t start = line.find_first_not_of(" \t\r");
		if (start == std::string::npos)
			continue;
		char first = line[start];
		if (first == 'c')
			continue;
		if (first == 's') {
			if (line.find("UNSAT") != std::string::npos) {
				LOGERROR("Model file %s: the formula is reported unsatisfiable (line %lu)", filename, lineNumber);
				return false;
			}
			continue;
		}
		std::istringstream fields(line.substr(start));
		if (first == 'v') {
			fields.get();
			sawV = true;
		} else if (first == '-' || std::isdigit((unsigned char)first)) {
			sawPlain = true;
		} else {
			LOGERROR("Model file %s: unexpected line %lu: %s", filename, lineNumber, line.c_str());
			return false;
		}
		long lit;
		while (fields >> lit) {
			if (lit == 0)
				continue;
			unsigned long var = std::labs(lit);
			if (var > varCount) {
				LOGERROR("Model file %s: literal %ld out of the %u variables (line %lu)", filename, lit, varCount, lineNumber);
				return false;
			}
			int sign = (lit > 0) ? 1 : -1;
			if (value[var] == -sign) {
				LOGERROR("Model file %s: variable %lu assigned both ways (line %lu)", filename, var, lineNumber);
				return false;
			}
			value[var] = sign;
		}
		if (!fields.eof()) {
			LOGERROR("Model file %s: unexpected token on line %lu: %s", filename, lineNumber, line.c_str());
			return false;
		}
	}
	if (sawV && sawPlain) {
		LOGERROR("Model file %s: mixes 'v' lines and plain literal lines", filename);
		return false;
	}

	model.clear();
	for (unsigned int var = 1; var <= varCount; var++)
		if (value[var])
			model.push_back(value[var] * (int)var);
	LOG0("Parsed a model of %zu out of %u variables in %s", model.size(), varCount, filename);
	return true;
}

} // namespace Parsers