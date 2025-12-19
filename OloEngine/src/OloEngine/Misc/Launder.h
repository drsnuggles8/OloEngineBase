// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

// @file Launder.h
// @brief Provides std::launder wrapper for pointer laundering
// 
// std::launder is used to prevent compiler optimizations that could break
// type punning through placement new. This is particularly important for
// type-erased storage patterns like TTaskDelegate.
// 
// Since OloEngine targets C++20/23, std::launder is always available.

#include <new> // IWYU pragma: export

// @def OLO_LAUNDER
// @brief Wrapper for std::launder to prevent pointer optimization issues
// 
// Use this when accessing memory through a pointer after placement new
// has been used to construct a different type at that location.
// 
// @param x Pointer to launder
// @return Laundered pointer safe to dereference
#define OLO_LAUNDER(x) std::launder(x)

