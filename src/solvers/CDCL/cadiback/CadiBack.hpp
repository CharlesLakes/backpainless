#pragma once

#include "solvers/CDCL/cadiback/CadiBackBase.hpp"

/**
 * @brief Baseline variant: the CadiBack algorithm of the paper, with the default hooks of CadiBackBase.
 * @ingroup solving
 */
class CadiBack : public CadiBackBase
{
  public:
	CadiBack(int id, const std::shared_ptr<ClauseDatabase>& clauseDB);

	const char* variantName() const override { return "CadiBack"; }
};
