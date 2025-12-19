// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEngine/Templates/RefCounting.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine::Private
{
    void CheckRefCountIsNonZero()
    {
        OLO_CORE_ASSERT(false, "Release() was called on an object which is already at a zero refcount.");
    }
} // namespace OloEngine::Private
