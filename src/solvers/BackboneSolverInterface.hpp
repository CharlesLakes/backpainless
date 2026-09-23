/**
 * @file BackboneSolverInterface.hpp
 * @brief Interface for backbone solvers.
 */

#pragma once

#include "containers/BackboneResult.hpp"
#include "containers/ClauseDatabase.hpp"
#include "containers/ClauseExchange.hpp"
#include "containers/ClauseUtils.hpp"
#include "sharing/SharingEntity.hpp"
#include "utils/Logger.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <random>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

/**
 * @defgroup solving Backbone Solvers
 * @brief Classes computing the backbone of a CNF formula
 * @{
 */

/* Forward Declaration */
class BackboneSolverInterface;

using SeedGenerator = std::function<int(BackboneSolverInterface*)>;

/// Code for the type of backbone solvers
enum class BackboneSolverType
{
	CADIBACK = 0 ///< CadiBack algorithm on top of CaDiCaL
};

/**
 * @brief Interface of a backbone solver.
 *
 * A backbone solver computes the set of literals that are true in every model of the formula. Each portfolio worker
 * runs one backbone solver. It is also a SharingEntity: learnt clauses (consequences of the formula) are exchanged
 * with the other backbone solvers through the sharing strategies.
 */
class BackboneSolverInterface : public SharingEntity
{
  public:
	/**
	 * @brief Computes the backbone of the loaded formula.
	 * @param cube Kept from the Painless WorkingStrategy API (no Divide-and-Conquer strategy exists), it is ignored.
	 * @return COMPLETE when the formula is satisfiable and the backbone was fully computed, UNSAT when the formula
	 * is unsatisfiable, UNKNOWN when interrupted.
	 */
	virtual BackboneResult solve(const std::vector<int>& cube) = 0;

	/// @brief Backbone literals found by the last solve() that returned COMPLETE, sorted by variable.
	virtual std::vector<int> getBackbone() = 0;

	/// @brief Interrupt the computation, solve() returns UNKNOWN as soon as possible.
	virtual void setSolverInterrupt() = 0;

	/// @brief Remove the interruption request.
	virtual void unsetSolverInterrupt() = 0;

	/// @brief Native diversification of the underlying solver.
	virtual void diversify(const SeedGenerator& getSeed = [](BackboneSolverInterface* s) {
		return s->getSolverId();
	}) = 0;

	/// @brief Load the formula from a DIMACS file.
	virtual void loadFormula(const char* filename) = 0;

	/// @brief Add the clauses of the formula.
	virtual void addInitialClauses(const std::vector<simpleClause>& clauses, unsigned int nbVars) = 0;

	/// @brief Add the clauses of the formula from a flat, zero separated, array of literals.
	virtual void addInitialClauses(const lit_t* literals, unsigned int clsCount, unsigned int nbVars) = 0;

	/// @brief Number of variables of the formula.
	virtual unsigned int getVariablesCount() = 0;

	/// @brief Set the initial phase of a variable (used by the GaspiInitializer).
	virtual void setPhase(const unsigned int var, const bool phase) = 0;

	/// @brief Print solver statistics (lines must start with 'c').
	virtual void printStatistics();

	/// @brief Log information about this solver when it wins.
	virtual void printWinningLog();

	/* Identifiers */

	bool isInitialized() { return this->m_initialized; }

	void setInitialized(bool value) { this->m_initialized = value; }

	BackboneSolverType getSolverType() { return this->m_solverType; }

	unsigned int getSolverTypeId() { return this->m_solverTypeId; }

	void setSolverTypeId(unsigned int typeId) { this->m_solverTypeId = typeId; }

	unsigned int getSolverId() { return this->m_solverId; }

	void setSolverId(unsigned int id) { this->m_solverId = id; }

	/// @brief Number of instances of the concrete type of this solver.
	unsigned int getSolverTypeCount() const
	{
		auto it = s_instanceCounts.find(std::type_index(typeid(*this)));
		return (it != s_instanceCounts.end()) ? it->second.load() : 0;
	}

	/**
	 * @brief Constructor.
	 * @param solverId Global id of the solver.
	 * @param clauseDB Database used to import the clauses received from the sharing strategies.
	 * @param solverType Type of the backbone solver.
	 */
	BackboneSolverInterface(int solverId, const std::shared_ptr<ClauseDatabase>& clauseDB, BackboneSolverType solverType);

	virtual ~BackboneSolverInterface();

  protected:
	template<typename Derived>
	static unsigned int getAndIncrementTypeCount()
	{
		auto [it, inserted] = s_instanceCounts.try_emplace(std::type_index(typeid(Derived)), 0);
		return it->second.fetch_add(1);
	}

	/// @brief Must be called in the constructor of the most derived class to set its type id.
	template<typename Derived>
	void initializeTypeId()
	{
		m_solverTypeId = getAndIncrementTypeCount<Derived>();
		LOGDEBUG1("I am solver of type %s: id %d, typeId: %u", typeid(Derived).name(), m_solverId, m_solverTypeId);
	}

  protected:
	/// @brief Database used to import clauses. Can be common with other solvers.
	std::shared_ptr<ClauseDatabase> m_clausesToImport;

	BackboneSolverType m_solverType;

	bool m_initialized;

	unsigned int m_solverId;

	/// @brief Id of the solver among the solvers of the same type.
	unsigned int m_solverTypeId;

	static inline std::unordered_map<std::type_index, std::atomic<unsigned int>> s_instanceCounts;
};

/** @} */
