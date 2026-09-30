#include "solvers/CandidateOrder.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <sstream>

bool
parseCandidateOrder(const std::string& name, CandidateOrder& order)
{
	if (name == "natural")
		order = CandidateOrder::NATURAL;
	else if (name == "reverse")
		order = CandidateOrder::REVERSE;
	else if (name == "random")
		order = CandidateOrder::RANDOM;
	else if (name == "occ")
		order = CandidateOrder::OCC;
	else if (name == "occ-rev")
		order = CandidateOrder::OCC_REV;
	/* NEW ORDER (2/6): else if (name == "<name>") order = CandidateOrder::<VALUE>; */
	else
		return false;
	return true;
}

bool
parseCandidateOrderList(const std::string& list, std::vector<CandidateOrder>& orders)
{
	orders.clear();
	std::stringstream stream(list);
	std::string name;
	/* getline drops a trailing empty field, so "natural," is rejected by the size check below */
	size_t fields = std::count(list.begin(), list.end(), ',') + 1;
	while (std::getline(stream, name, ',')) {
		CandidateOrder order;
		if (!parseCandidateOrder(name, order))
			return false;
		orders.push_back(order);
	}
	return !orders.empty() && orders.size() == fields;
}

const char*
candidateOrderName(CandidateOrder order)
{
	switch (order) {
		case CandidateOrder::NATURAL:
			return "natural";
		case CandidateOrder::REVERSE:
			return "reverse";
		case CandidateOrder::RANDOM:
			return "random";
		case CandidateOrder::OCC:
			return "occ";
		case CandidateOrder::OCC_REV:
			return "occ-rev";
		/* NEW ORDER (3/6): case CandidateOrder::<VALUE>: return "<name>"; */
	}
	return "unknown";
}

void
sortCandidates(std::vector<int>& candidates,
			   CandidateOrder order,
			   const LiteralOccurrences& occurrences,
			   unsigned int seed)
{
	auto byVar = [](int a, int b) { return std::abs(a) < std::abs(b); };
	/* Candidates are built by variable index, sorting first makes the ties independent of any earlier change */
	std::sort(candidates.begin(), candidates.end(), byVar);

	switch (order) {
		case CandidateOrder::NATURAL:
			break;
		case CandidateOrder::REVERSE:
			std::reverse(candidates.begin(), candidates.end());
			break;
		case CandidateOrder::RANDOM: {
			std::mt19937 engine(seed);
			std::shuffle(candidates.begin(), candidates.end(), engine);
			break;
		}
		case CandidateOrder::OCC:
		case CandidateOrder::OCC_REV: {
			if (occurrences.empty())
				break;
			const bool decreasing = (order == CandidateOrder::OCC);
			std::stable_sort(candidates.begin(), candidates.end(), [&occurrences, decreasing](int a, int b) {
				return decreasing ? occurrences[a] > occurrences[b] : occurrences[a] < occurrences[b];
			});
			break;
		}
		/* NEW ORDER (4/6): case CandidateOrder::<VALUE>: reorder 'candidates' (already sorted by variable, so a
		 * std::stable_sort keeps the variable order for ties); every candidate must stay */
	}
}
