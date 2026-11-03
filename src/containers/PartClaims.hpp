#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief Owner of every block of candidates of the d workers (CadiBackPartition, -bb-part-mode=steal).
 * @ingroup solving
 *
 * The variables are split into parts and every part into blocks, the same on every worker (see CadiBackPartition).
 * A d worker only constrains the candidates of the blocks it claimed here, so that no candidate is checked by two
 * workers; the candidates of the other blocks are decided by their owners and taken from the candidate board.
 *
 * An entry is 0 while the block is free, else the code of its owner (@ref code: the lower the d index, the higher the
 * code). Inside a process a claim is a compare-and-swap from 0, so it is exclusive. With -dist the table is exchanged
 * with the board (an MPI_Allreduce with MPI_MAX, merged with @ref mergeMax): when two ranks claimed the same block in
 * one round, every rank keeps the code of the lower d index, and the other owner gives the block up at its next chunk.
 * The work done twice is therefore bounded by one block per sharing round. Memory order is relaxed: a claim carries no
 * other data, reading it late only costs work done twice.
 */
class PartClaims
{
  public:
	using Code = int16_t;

	explicit PartClaims(size_t blocks)
		: m_size(blocks)
		, m_owner(new std::atomic<Code>[blocks]())
	{
	}

	/// @brief Highest number of d workers sharing a table (the codes must stay positive).
	static constexpr unsigned int MAX_WORKERS = 32767;

	/// @brief Code stored for the d worker of index @p index (0 = free, so codes start at 1).
	static Code code(unsigned int index) { return (Code)(MAX_WORKERS - index); }

	size_t size() const { return m_size; }

	/// @brief Code of the owner of @p block, 0 if free.
	Code owner(size_t block) const { return m_owner[block].load(std::memory_order_relaxed); }

	/// @brief Claims @p block for @p code. Returns true if it was free (or already claimed by @p code).
	bool tryClaim(size_t block, Code code)
	{
		Code expected = 0;
		return m_owner[block].compare_exchange_strong(expected, code, std::memory_order_relaxed) || expected == code;
	}

	/// @brief Copies the owners into @p out[0..size-1].
	void snapshot(Code* out) const
	{
		for (size_t b = 0; b < m_size; b++)
			out[b] = m_owner[b].load(std::memory_order_relaxed);
	}

	/// @brief Keeps for every block the highest of its owner and @p in[block] (the result of the MPI_MAX reduction).
	/// Returns the number of blocks whose owner changed.
	unsigned int mergeMax(const Code* in)
	{
		unsigned int changed = 0;
		for (size_t b = 0; b < m_size; b++) {
			Code current = m_owner[b].load(std::memory_order_relaxed);
			while (in[b] > current) {
				if (m_owner[b].compare_exchange_weak(current, in[b], std::memory_order_relaxed)) {
					changed++;
					break;
				}
			}
		}
		return changed;
	}

  private:
	size_t m_size;

	std::unique_ptr<std::atomic<Code>[]> m_owner;
};
