#pragma once

// @file FunctionRef.h
// @brief Backward compatibility header - forwards to Function.h
// 
// This header exists for backward compatibility. All function types
// (TFunctionRef, TFunction, TUniqueFunction) are now defined in Function.h
// which provides inline storage optimization.
// 
// New code should include "OloEngine/Templates/Function.h" directly.

#include "OloEngine/Templates/Function.h"
