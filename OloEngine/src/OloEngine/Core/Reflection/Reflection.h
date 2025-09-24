#pragma once

/**
 * OloEngine Reflection System
 * 
 * A template-based compile-time reflection system that provides:
 * - Member pointer introspection
 * - Runtime member access by name
 * - Automatic member name extraction from source code
 * - Type-safe dynamic member operations
 * - Zero-runtime-cost reflection through templates
 * 
 * Usage:
 * 1. Define your class with members
 * 2. Use OLO_DESCRIBE macro to create reflection data
 * 3. Access members by name or index at runtime
 * 
 * Example:
 *   struct MyClass {
 *       float value = 0.0f;
 *       int count = 0;
 *   };
 *   
 *   OLO_DESCRIBE(MyClass, &MyClass::value, &MyClass::count);
 *   
 *   MyClass obj;
 *   using Provider = DescriptionProvider<Description<MyClass>, MyClass>;
 *   Provider::SetMemberValueByName("value", 42.0f, obj);
 */

#include "TypeUtils.h"
#include "StringUtils.h"
#include "MemberList.h"
#include "TypeDescriptor.h"

namespace OloEngine::Core::Reflection {
	
	// Re-export commonly used types for convenience
	using StringUtils::RemovePrefixAndSuffix;
	
	// Helper alias for easy provider access
	template<typename T, typename TTag = DummyTag>
	using Provider = DescriptionProvider<Description<T, TTag>, T>;

} // namespace OloEngine::Core::Reflection