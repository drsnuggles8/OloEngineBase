#include "OloEnginePCH.h"
#include "JoltMaterial.h"

namespace OloEngine {

	// Define the static friction policy with default value (UseMaximum)
	FrictionCombinePolicy JoltMaterial::s_FrictionPolicy = FrictionCombinePolicy::UseMaximum;

}
