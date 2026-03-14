#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    class FArchive;
    struct TransformComponent;
    struct Rigidbody2DComponent;
    struct Rigidbody3DComponent;

    // Type-erased component serialization function.
    // The void* MUST point to the concrete component type matching the registered name.
    // Passing a mismatched type leads to undefined behavior.
    // Safety: Only call via the type-safe Serialize() overloads or
    // through the generated RegisterDefaults() bindings, which guarantee correct casts.
    using ComponentSerializeFn = std::function<void(FArchive&, void*)>;

    class ComponentReplicator
    {
      public:
        // Convenience overloads for built-in component types.
        static void Serialize(FArchive& ar, TransformComponent& component);
        static void Serialize(FArchive& ar, Rigidbody2DComponent& component);
        static void Serialize(FArchive& ar, Rigidbody3DComponent& component);

        // Register the three built-in component serializers.
        static void RegisterDefaults();

        // Register a custom component serializer by name.
        static void Register(const std::string& componentName, ComponentSerializeFn serializer);

        // Check whether a component name has a registered serializer.
        [[nodiscard]] static bool IsRegistered(const std::string& componentName);

        // Get the full registry (name → serializer).
        [[nodiscard]] static const std::unordered_map<std::string, ComponentSerializeFn>& GetRegistry();

        // Look up a serializer by name.  Returns nullptr if not registered.
        [[nodiscard]] static const ComponentSerializeFn* GetSerializer(const std::string& componentName);

        // Remove all registered serializers (useful for test isolation).
        static void ClearRegistry();

      private:
        static std::unordered_map<std::string, ComponentSerializeFn> s_Registry;
    };
} // namespace OloEngine
