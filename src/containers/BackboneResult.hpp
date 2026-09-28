#pragma once

/**
 * @brief Result of a backbone computation.
 * @ingroup solving
 *
 * The numeric values are part of the protocol: they are the process exit code, they are exchanged as MPI_INT by
 * the global sharing strategies (packed with the winner rank in the upper 16 bits) and UNKNOWN must stay 0.
 */
enum class BackboneResult
{
	UNKNOWN = 0,   /**< Not finished (interrupted or not started) */
	COMPLETE = 10, /**< The formula is satisfiable and its backbone was fully computed (it may be empty) */
	UNSAT = 20,	   /**< The formula is unsatisfiable, it has no backbone */
	TIMEOUT = 30   /**< Timeout occurred */
};
