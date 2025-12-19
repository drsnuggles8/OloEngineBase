#pragma once

#include "OloEngine/Core/Base.h"
#include "JoltUtils.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

namespace OloEngine
{

    // Physics layer constants
    constexpr u32 INVALID_LAYER_ID = static_cast<u32>(-1);
    constexpr u32 INVALID_BIT_MASK = static_cast<u32>(-1);
    constexpr u32 NO_COLLISION_BITS = 0;
    constexpr u32 NO_PREVIOUS_LAYER_ID = static_cast<u32>(-1);

    struct PhysicsLayer
    {
        u32 m_LayerID;
        std::string m_Name;
        u32 m_BitValue;
        u32 m_CollidesWith = 0;
        bool m_CollidesWithSelf = false;

        [[nodiscard]] bool IsValid() const
        {
            return !m_Name.empty() && m_BitValue > 0 && m_LayerID != INVALID_LAYER_ID;
        }
    };

    class PhysicsLayerManager
    {
      public:
        static u32 AddLayer(const std::string& name, bool setCollisions = true);
        static void RemoveLayer(u32 layerId);

        static void UpdateLayerName(u32 layerId, const std::string& newName);

        static void SetLayerCollision(u32 layerId, u32 otherLayer, bool shouldCollide);
        static void SetLayerSelfCollision(u32 layerId, bool shouldCollide);
        static void GetLayerCollisions(u32 layerId, std::vector<PhysicsLayer>& outLayers);

        static std::vector<PhysicsLayer> GetLayers(); // Returns a copy for thread safety
        static std::vector<std::string> GetLayerNames();

        static PhysicsLayer GetLayer(u32 layerId);
        static PhysicsLayer GetLayer(const std::string& layerName);
        static PhysicsLayer GetLayer(const std::string_view& layerName);
        static u32 GetLayerCount();

        [[nodiscard]] static bool ShouldCollide(u32 layer1, u32 layer2) noexcept;
        [[nodiscard]] static bool IsLayerValid(u32 layerId) noexcept;
        [[nodiscard]] static bool IsLayerValid(const std::string& layerName);
        [[nodiscard]] static bool IsLayerValid(const std::string_view& layerName);

        static void ClearLayers();

      private:
        static u32 GetNextLayerID();

        // Helper function to rebuild the index map when s_Layers is modified
        static void RebuildLayerIndexMap();

        // Helper function to generate layer bitmask with bounds checking
        [[nodiscard]] static u32 ToLayerMask(u32 layerId) noexcept;

        // Internal unsafe methods for use within locked contexts - do not use externally
        static const PhysicsLayer& GetLayerUnsafe(u32 layerId);
        static const PhysicsLayer& GetLayerUnsafe(const std::string& layerName);

        // Internal mutable accessor for modification operations - use with caution
        static PhysicsLayer& GetLayerMutableUnsafe(u32 layerId);

      private:
        // Internal helper for shared lookup logic
        static const PhysicsLayer& GetLayerImpl(u32 layerId);

        static std::vector<PhysicsLayer> s_Layers;
        static std::unordered_map<u32, std::string> s_LayerNames;
        static std::unordered_map<u32, sizet> s_LayerIndexMap; // Maps LayerID to index in s_Layers for O(1) lookup
        static const PhysicsLayer s_NullLayer;
        static std::shared_mutex s_LayersMutex; // Protects all static containers
    };

} // namespace OloEngine
