// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Physics3D/ColliderMaterial.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Scene/Components.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <new>
#include <type_traits>

// =============================================================================
// BitwiseEqualLayoutTest — every type compared as a WHOLE OBJECT through
// Math::BitwiseEqual must have no padding bytes (issue #1019).
//
// Why: BitwiseEqual is a memcmp over sizeof(T). Padding bytes are unspecified
// after member stores — a constructor never writes them, and GCC 14 -O3 was
// observed re-materialising them (MathBitwiseEqualTest.IntegerTypesAlsoWork
// failed on the GCC nightly). Roughly thirty engine types implement operator==
// as `Math::BitwiseEqual(*this, other)`; the editor's undo (SceneHierarchyPanel
// DrawComponent<T>, value-comparison tier) and the Play/stop change detection
// run those operators, so a padded type can compare UNEQUAL when logically
// equal. `std::has_unique_object_representations_v` cannot be the guard: it is
// false for any type holding a float on every compiler.
//
// How it proves the layout: two buffers are filled with 0x00 and 0xFF, a
// default-constructed T is placement-newed into each, and the two objects must
// compare bit-equal. A padded type fails because the constructor left the fill
// pattern in the padding bytes.
//
// How to find the types to list here:
//     git grep -n "BitwiseEqual(\*this" OloEngine/src
// plus every BitwiseEqual call whose arguments are nested structs rather than
// scalars / glm types (e.g. `BitwiseEqual(Config, other.Config)` in
// AudioSourceColdData, and GPUScene::RecordsEqual over the GPU record structs).
// Adding a type is one line in the OLO_BITWISE_EQUAL_TYPES list below.
//
// Not listed: OceanFFTField::H0Key is private (a static_assert on its sizeof
// next to the definition covers it).
// =============================================================================

namespace
{
    using OloEngine::Math::BitwiseEqual;

    template<typename T>
    bool DefaultObjectsAreBitEqualRegardlessOfFill()
    {
        static_assert(std::is_trivially_copyable_v<T>, "BitwiseEqual requires a trivially-copyable type");

        alignas(T) std::byte zeroBuf[sizeof(T)];
        alignas(T) std::byte onesBuf[sizeof(T)];
        std::memset(zeroBuf, 0x00, sizeof(T));
        std::memset(onesBuf, 0xFF, sizeof(T));

        // DEFAULT-initialised, no parentheses. `T()` would VALUE-initialise,
        // and for a class whose default constructor is implicitly defined
        // (which every component here is -- they use default member
        // initialisers, not a written ctor) value-initialisation zero-fills
        // the whole object first, padding included. That would erase the very
        // bytes this probe exists to catch and the test would pass for a
        // padded type.
        T* a = ::new (static_cast<void*>(zeroBuf)) T;
        T* b = ::new (static_cast<void*>(onesBuf)) T;
        const bool equal = BitwiseEqual(*a, *b);
        a->~T();
        b->~T();
        return equal;
    }

    // One entry per type whose operator== (or an engine call) compares the
    // whole object through Math::BitwiseEqual. Keep sorted by header.
#define OLO_BITWISE_EQUAL_TYPES(X)                          \
    /* Animation */                                         \
    X(OloEngine::FootIKComponent)                           \
    X(OloEngine::IKTargetComponent)                         \
    X(OloEngine::NoiseAnimationComponent)                   \
    X(OloEngine::SpringBoneComponent)                       \
    /* Audio (nested in AudioSourceColdData::operator==) */ \
    X(OloEngine::AudioSourceConfig)                         \
    /* Physics3D */                                         \
    X(OloEngine::ColliderMaterial)                          \
    /* Renderer/GPUScene (GPUScene::RecordsEqual) */        \
    X(OloEngine::GPUSceneGeometry)                          \
    X(OloEngine::GPUSceneInstance)                          \
    X(OloEngine::GPUSceneMaterial)                          \
    X(OloEngine::GPUSceneLight)                             \
    X(OloEngine::GPUSceneEnvironment)                       \
    /* Scene/Components.h */                                \
    X(OloEngine::BoxCollider3DComponent)                    \
    X(OloEngine::SphereCollider3DComponent)                 \
    X(OloEngine::CapsuleCollider3DComponent)                \
    X(OloEngine::MeshCollider3DComponent)                   \
    X(OloEngine::ConvexMeshCollider3DComponent)             \
    X(OloEngine::TriangleMeshCollider3DComponent)           \
    X(OloEngine::CharacterController3DComponent)            \
    X(OloEngine::DebrisComponent)                           \
    X(OloEngine::DirectionalLightComponent)                 \
    X(OloEngine::PointLightComponent)                       \
    X(OloEngine::SpotLightComponent)                        \
    X(OloEngine::SphereAreaLightComponent)                  \
    X(OloEngine::WeatherPreset)                             \
    X(OloEngine::LightProbeComponent)                       \
    X(OloEngine::SnowDeformerComponent)                     \
    X(OloEngine::VirtualMeshComponent)                      \
    X(OloEngine::FluidComponent)                            \
    X(OloEngine::FluidEmitterComponent)                     \
    X(OloEngine::FluidKillVolumeComponent)                  \
    X(OloEngine::FogVolumeComponent)                        \
    X(OloEngine::NetworkInterestComponent)                  \
    X(OloEngine::NameplateComponent)

#define OLO_STATIC_ASSERT_TRIVIAL(T) static_assert(std::is_trivially_copyable_v<T>, #T " must stay trivially copyable for Math::BitwiseEqual");
    OLO_BITWISE_EQUAL_TYPES(OLO_STATIC_ASSERT_TRIVIAL)
#undef OLO_STATIC_ASSERT_TRIVIAL

    template<typename T>
    class BitwiseEqualLayoutTest : public ::testing::Test
    {
    };

#define OLO_COMMA_TYPE(T) , T
    using BitwiseEqualTypes = ::testing::Types<void OLO_BITWISE_EQUAL_TYPES(OLO_COMMA_TYPE)>;
#undef OLO_COMMA_TYPE

    // gtest needs a non-empty leading element for the comma trick above; strip
    // the placeholder `void` again.
    template<typename List>
    struct DropFirst;
    template<typename First, typename... Rest>
    struct DropFirst<::testing::Types<First, Rest...>>
    {
        using type = ::testing::Types<Rest...>;
    };

#define OLO_TYPE_NAME_STRING(T) #T,
    constexpr const char* kTypeNames[] = { OLO_BITWISE_EQUAL_TYPES(OLO_TYPE_NAME_STRING) };
#undef OLO_TYPE_NAME_STRING

    // gtest names typed tests by index; use the listed type name instead
    // (identifier characters only — "OloEngine::FootIKComponent" -> "FootIKComponent").
    struct TypeNamer
    {
        template<typename T>
        static std::string GetName(int index)
        {
            std::string name = kTypeNames[index];
            if (const auto pos = name.rfind("::"); pos != std::string::npos)
                name = name.substr(pos + 2);
            return name;
        }
    };

    TYPED_TEST_SUITE(BitwiseEqualLayoutTest, DropFirst<BitwiseEqualTypes>::type, TypeNamer);

    TYPED_TEST(BitwiseEqualLayoutTest, DefaultConstructedObjectsHaveNoPaddingBytes)
    {
        EXPECT_TRUE(DefaultObjectsAreBitEqualRegardlessOfFill<TypeParam>())
            << "sizeof=" << sizeof(TypeParam) << " alignof=" << alignof(TypeParam)
            << ": the type has padding bytes, so Math::BitwiseEqual(*this, other) compares "
               "unspecified memory. Reorder members or add explicit OLO_SERIALIZE(Skip) Pad fields "
               "(issue #1019).";
    }
} // namespace
