#include "solvers/CDCL/cadiback/CadiBack.hpp"

CadiBack::CadiBack(int id, const std::shared_ptr<ClauseDatabase>& clauseDB)
	: CadiBackBase(id, clauseDB, BackboneSolverType::CADIBACK)
{
	initializeTypeId<CadiBack>();
}
