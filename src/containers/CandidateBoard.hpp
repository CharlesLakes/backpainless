#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>

/**
 * @brief Status of every variable, shared by the backbone solvers of a process (-bb-share-cand).
 * @ingroup solving
 *
 * Sharing clauses cannot express that a literal is NOT backbone: that is witnessed by a model, not implied by the
 * formula. The board carries both answers so that a solver can drop the candidates already decided by another one:
 * - backbone: the variable has a backbone literal, stored with its sign;
 * - free: the variable has no backbone literal, i.e. both of its values appear in models of the formula.
 *
 * A status only goes from unknown to a final value, the first writer wins. Every model found by a solver, even under
 * a chunk constraint, is a model of the formula, so the statuses published by any solver are sound for all of them.
 * Memory order is relaxed: a status carries no other data and reading it late only costs a SAT call.
 */
class CandidateBoard
{
  public:
	explicit CandidateBoard(unsigned int nbVars)
		: m_nbVars(nbVars)
		, m_status(new std::atomic<int8_t>[nbVars + 1]())
	{
	}

	/// @brief Records that @p lit is a backbone literal. Returns true if the status was unknown.
	bool publishBackbone(int lit) { return publish(std::abs(lit), (lit > 0) ? POSITIVE : NEGATIVE); }

	/// @brief Records that @p var has no backbone literal. Returns true if the status was unknown.
	bool publishFree(int var) { return publish(std::abs(var), FREE_VAR); }

	/// @brief 0 if unknown or out of range, the backbone literal of @p var, or @ref FREE.
	int status(int var) const
	{
		unsigned int v = std::abs(var);
		if (v == 0 || v > m_nbVars)
			return 0;
		switch (m_status[v].load(std::memory_order_relaxed)) {
			case POSITIVE:
				return v;
			case NEGATIVE:
				return -(int)v;
			case FREE_VAR:
				return FREE;
			default:
				return 0;
		}
	}

	/// @brief Value returned by status() for a variable without backbone literal (never a valid literal).
	static constexpr int FREE = 0x7fffffff;

  private:
	enum : int8_t
	{
		UNKNOWN = 0,
		POSITIVE = 1,
		NEGATIVE = 2,
		FREE_VAR = 3
	};

	bool publish(unsigned int var, int8_t value)
	{
		if (var == 0 || var > m_nbVars)
			return false;
		int8_t expected = UNKNOWN;
		if (m_status[var].compare_exchange_strong(expected, value, std::memory_order_relaxed))
			return true;
		/* Two sound solvers cannot disagree: a backbone variable is never free, its backbone literal is unique */
		assert(expected == value);
		return false;
	}

	unsigned int m_nbVars;

	std::unique_ptr<std::atomic<int8_t>[]> m_status;
};
