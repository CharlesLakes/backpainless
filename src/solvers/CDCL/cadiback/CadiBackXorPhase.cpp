/* XOR exploration of CadiBackBase before the CadiBack loop (-bb-xor-pre), see xorPrePhase in CadiBackBase.hpp */

#include "solvers/CDCL/cadiback/CadiBackBase.hpp"

#include "utils/Logger.hpp"

#include "cadical/src/stats.hpp"

#include <algorithm>
#include <chrono>
#include <climits>

BackboneResult
CadiBackBase::xorPrePhase(std::vector<int>& candidates)
{
	auto start = std::chrono::steady_clock::now();
	m_xorRng.seed(m_seed);
	m_lastVariable = std::max<int>(m_nbVars, solver->vars());

	/* A relative budget follows the hardness of the instance: the first model took the conflicts done so far */
	const XorBudget& budget = m_xorOptions.budget;
	m_xorBudget = (budget.factor > 0.0)
					  ? std::max<unsigned long>(MIN_XOR_BUDGET, budget.factor * solver->getStatistics()->conflicts)
					  : budget.conflicts;

	BackboneResult result = BackboneResult::COMPLETE;
	for (unsigned long round = 0; round < m_xorOptions.rounds && !candidates.empty(); round++) {
		if (stopSolver) {
			result = BackboneResult::UNKNOWN;
			break;
		}
		syncWithBoard(candidates);
		extractFixed(candidates);
		if (candidates.empty())
			break;
		result = xorRound(candidates);
		if (result != BackboneResult::COMPLETE)
			break;
	}

	m_xorSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	LOG1("%s %d: XOR exploration done in %.2f s, %lu free variables found, %zu candidates left",
		 variantName(),
		 this->getSolverId(),
		 m_xorSeconds,
		 m_freeByXor,
		 candidates.size());
	return result;
}

BackboneResult
CadiBackBase::xorRound(std::vector<int>& candidates)
{
	std::vector<int> pool;
	pool.reserve(candidates.size());
	for (int lit : candidates)
		pool.push_back(std::abs(lit));

	std::vector<std::vector<int>> xors;
	m_xorsDependent += drawRandomXors(pool, m_xorOptions.count, m_xorOptions.density, m_xorRng, xors);
	if (xors.empty())
		return BackboneResult::COMPLETE;

	/* Round literal e: every clause of the round contains -e, it is assumed during the round and fixed to false at
	 * the end, which satisfies (and lets CaDiCaL delete) all of them. XOR j: x_1 ^ ... ^ x_k ^ s_j = 0 defines the
	 * fresh selector s_j, assuming s_j = b means parity = b. The fresh variables are only definitions or guarded, so
	 * the formula keeps its models over the original variables (and the Learner drops the clauses that use them) */
	const unsigned int m = xors.size();
	const int guard = newVariable();
	solver->freeze(guard);
	std::vector<int> selectors(m);
	for (unsigned int j = 0; j < m; j++) {
		selectors[j] = newVariable();
		solver->freeze(selectors[j]);
		xors[j].push_back(selectors[j]);
		addXorChain(xors[j], guard);
	}
	m_xorRounds++;
	m_xorsAdded += m;

	bool formulaUnsat = false;
	auto solveNode = [&](unsigned int depth, uint64_t mask) -> XorNodeResult {
		if (stopSolver || candidates.empty())
			return { XorNodeAnswer::STOP };

		solver->assume(guard);
		for (unsigned int j = 0; j < depth; j++)
			solver->assume(xorRhs(mask, m, j) ? selectors[j] : -selectors[j]);
		if (m_xorBudget)
			solver->limit("conflicts", (int)std::min<unsigned long>(m_xorBudget, INT_MAX));
		m_satCalls++;
		int res = solver->solve();

		if (res == 10) {
			m_satAnswers++;
			size_t before = candidates.size();
			filterWithModel(candidates);
			m_freeByXor += before - candidates.size();
			return { XorNodeAnswer::SAT };
		}
		if (res == 20) {
			m_unsatAnswers++;
			/* The failed assumptions: the path down to the deepest failed selector has no model */
			int deepest = -1;
			for (unsigned int j = 0; j < depth; j++)
				if (solver->failed(xorRhs(mask, m, j) ? selectors[j] : -selectors[j]))
					deepest = j;
			if (deepest < 0) {
				/* Only the round literal (or nothing): the formula itself is unsatisfiable */
				formulaUnsat = true;
				return { XorNodeAnswer::STOP };
			}
			return { XorNodeAnswer::UNSAT, (unsigned int)deepest };
		}
		return { stopSolver ? XorNodeAnswer::STOP : XorNodeAnswer::UNKNOWN };
	};
	xorDfs(m, m_xorOptions.adaptive, m_xorOptions.leaves, solveNode, m_xorDfs);

	/* Retire the round: -e satisfies every clause of the round */
	solver->add(-guard);
	solver->add(0);
	solver->melt(guard);
	for (int selector : selectors)
		solver->melt(selector);

	if (formulaUnsat) {
		LOGSTAT("%s %d: formula is UNSATISFIABLE", variantName(), this->getSolverId());
		return BackboneResult::UNSAT;
	}
	if (stopSolver)
		return BackboneResult::UNKNOWN;
	return BackboneResult::COMPLETE;
}

void
CadiBackBase::addXorChain(const std::vector<int>& lits, int guard)
{
	/* l_0 ^ ... ^ l_{n-1} = 0 as a_1 = l_0 ^ l_1 ^ l_2, a_2 = a_1 ^ l_3 ^ l_4, ...: each link is the 4-literal XOR
	 * prev ^ x ^ y ^ a = 0 with a fresh a, the last link closes the chain with the remaining (at most 4) literals */
	if (lits.size() <= 4) {
		addSmallXor(lits.data(), lits.size(), guard);
		return;
	}
	int link[4] = { lits[0], lits[1], lits[2], newVariable() };
	addSmallXor(link, 4, guard);
	size_t i = 3;
	while (lits.size() - i > 3) {
		int next[4] = { link[3], lits[i], lits[i + 1], newVariable() };
		addSmallXor(next, 4, guard);
		link[3] = next[3];
		i += 2;
	}
	int last[4] = { link[3], 0, 0, 0 };
	unsigned int n = 1;
	for (; i < lits.size(); i++)
		last[n++] = lits[i];
	addSmallXor(last, n, guard);
}

void
CadiBackBase::addSmallXor(const int* lits, unsigned int n, int guard)
{
	/* Forbid every assignment of odd parity: for each one, the clause falsified exactly by it */
	for (unsigned int assignment = 0; assignment < (1u << n); assignment++) {
		if (__builtin_popcount(assignment) % 2 == 0)
			continue;
		solver->add(-guard);
		for (unsigned int i = 0; i < n; i++)
			solver->add(((assignment >> i) & 1) ? -lits[i] : lits[i]);
		solver->add(0);
		m_xorClauses++;
	}
}
