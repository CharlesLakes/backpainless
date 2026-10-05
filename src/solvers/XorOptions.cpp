#include "solvers/XorOptions.hpp"

#include "solvers/CandidateOrder.hpp"
#include "utils/Parameters.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

XorOptions
XorOptionLists::forSolver(unsigned int id) const
{
	XorOptions options;
	options.count = counts[id % counts.size()];
	options.density = densities[id % densities.size()];
	options.rounds = rounds[id % rounds.size()];
	options.leaves = leaves[id % leaves.size()];
	options.adaptive = adaptive[id % adaptive.size()];
	options.exportLearnt = exportMask[id % exportMask.size()] == '1';
	options.pre = preMask[id % preMask.size()] == '1';
	options.budget = budgets[id % budgets.size()];
	return options;
}

bool
parseXorDensityList(const std::string& list, std::vector<XorDensity>& densities)
{
	densities.clear();
	std::stringstream stream(list);
	std::string field;
	size_t fields = std::count(list.begin(), list.end(), ',') + 1;
	while (std::getline(stream, field, ',')) {
		if (field.empty() || field.size() > 18 || field.find_first_not_of("0123456789.") != std::string::npos ||
			std::count(field.begin(), field.end(), '.') > 1 || field == ".")
			return false;
		XorDensity density;
		if (field.find('.') != std::string::npos) {
			density.probability = std::strtod(field.c_str(), nullptr);
			if (!(density.probability > 0.0 && density.probability <= 1.0))
				return false;
		} else {
			unsigned long length = std::stoul(field);
			if (length == 0 || length > 0xffffffffUL)
				return false;
			density.length = length;
		}
		densities.push_back(density);
	}
	return !densities.empty() && densities.size() == fields;
}

bool
parseXorBudgetList(const std::string& list, std::vector<XorBudget>& budgets)
{
	budgets.clear();
	std::stringstream stream(list);
	std::string field;
	size_t fields = std::count(list.begin(), list.end(), ',') + 1;
	while (std::getline(stream, field, ',')) {
		if (field.empty() || field.size() > 18)
			return false;
		XorBudget budget;
		if (field.back() == 'x') {
			std::string factor = field.substr(0, field.size() - 1);
			if (factor.empty() || factor.find_first_not_of("0123456789.") != std::string::npos ||
				std::count(factor.begin(), factor.end(), '.') > 1 || factor == ".")
				return false;
			budget.factor = std::strtod(factor.c_str(), nullptr);
			if (!(budget.factor > 0.0))
				return false;
		} else {
			if (field.find_first_not_of("0123456789") != std::string::npos)
				return false;
			budget.factor = 0.0;
			budget.conflicts = std::stoul(field);
		}
		budgets.push_back(budget);
	}
	return !budgets.empty() && budgets.size() == fields;
}

std::string
xorBudgetName(const XorBudget& budget)
{
	std::ostringstream name;
	if (budget.factor > 0.0)
		name << budget.factor << "x";
	else
		name << budget.conflicts;
	return name.str();
}

bool
parseXorDfsList(const std::string& list, std::vector<char>& adaptive)
{
	adaptive.clear();
	std::stringstream stream(list);
	std::string field;
	size_t fields = std::count(list.begin(), list.end(), ',') + 1;
	while (std::getline(stream, field, ',')) {
		if (field == "adaptive")
			adaptive.push_back(1);
		else if (field == "leaves")
			adaptive.push_back(0);
		else
			return false;
	}
	return !adaptive.empty() && adaptive.size() == fields;
}

std::string
xorDensityName(const XorDensity& density)
{
	std::ostringstream name;
	if (density.length)
		name << "k=" << density.length;
	else
		name << "p=" << density.probability;
	return name.str();
}

bool
parseXorOptionLists(XorOptionLists& lists, std::string& error)
{
	/* Same syntax as -bb-chunk: comma-separated non-negative integers */
	auto parseIntegers = [&error](const std::string& value, const char* flag, std::vector<unsigned long>& out) {
		if (parseChunkRateList(value, out))
			return true;
		error = std::string("Invalid -") + flag + "=" + value + ": expected a comma-separated list of non-negative integers";
		return false;
	};

	if (!parseIntegers(__globalParameters__.backboneXorCount, "bb-xor-m", lists.counts) ||
		!parseIntegers(__globalParameters__.backboneXorRounds, "bb-xor-rounds", lists.rounds) ||
		!parseIntegers(__globalParameters__.backboneXorLeaves, "bb-xor-leaves", lists.leaves))
		return false;

	if (!parseXorBudgetList(__globalParameters__.backboneXorConflicts, lists.budgets)) {
		error = "Invalid -bb-xor-confl=" + __globalParameters__.backboneXorConflicts +
				": expected a comma-separated list of integers (0 = no limit) or factors '<f>x' (e.g. 2x)";
		return false;
	}

	for (unsigned long count : lists.counts) {
		if (count == 0 || count > MAX_XOR_COUNT) {
			error = "Invalid -bb-xor-m=" + __globalParameters__.backboneXorCount + ": every value must be in 1.." +
					std::to_string(MAX_XOR_COUNT);
			return false;
		}
	}

	if (!parseXorDensityList(__globalParameters__.backboneXorDensity, lists.densities)) {
		error = "Invalid -bb-xor-density=" + __globalParameters__.backboneXorDensity +
				": expected a comma-separated list of probabilities in (0,1] (with a '.') or integers >= 1";
		return false;
	}
	lists.exportMask = __globalParameters__.backboneXorExport;
	if (lists.exportMask.empty() || lists.exportMask.find_first_not_of("01") != std::string::npos) {
		error = "Invalid -bb-xor-export=" + lists.exportMask + ": expected a non empty mask of 0 and 1";
		return false;
	}

	lists.preMask = __globalParameters__.backboneXorPre;
	if (lists.preMask.empty() || lists.preMask.find_first_not_of("01") != std::string::npos) {
		error = "Invalid -bb-xor-pre=" + lists.preMask + ": expected a non empty mask of 0 and 1";
		return false;
	}

	if (!parseXorDfsList(__globalParameters__.backboneXorDfs, lists.adaptive)) {
		error = "Invalid -bb-xor-dfs=" + __globalParameters__.backboneXorDfs +
				": expected a comma-separated list of adaptive, leaves";
		return false;
	}
	return true;
}
