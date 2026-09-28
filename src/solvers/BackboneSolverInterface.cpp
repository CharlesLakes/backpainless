#include "BackboneSolverInterface.hpp"

void
BackboneSolverInterface::printStatistics()
{
	LOGWARN("printStatistics is not implemented");
}

void
BackboneSolverInterface::printWinningLog()
{
	LOGSTAT("The winner is backbone solver %u (type %d, typeId %u)",
			this->getSolverId(),
			static_cast<int>(this->getSolverType()),
			this->getSolverTypeId());
}

BackboneSolverInterface::BackboneSolverInterface(int solverId,
												 const std::shared_ptr<ClauseDatabase>& clauseDB,
												 BackboneSolverType solverType)
	: SharingEntity()
	, m_clausesToImport(clauseDB)
	, m_solverType(solverType)
	, m_initialized(false)
	, m_solverId(solverId)
	, m_solverTypeId(0)
{
}

BackboneSolverInterface::~BackboneSolverInterface()
{
	auto it = s_instanceCounts.find(std::type_index(typeid(*this)));
	if (it != s_instanceCounts.end())
		it->second--;
}
