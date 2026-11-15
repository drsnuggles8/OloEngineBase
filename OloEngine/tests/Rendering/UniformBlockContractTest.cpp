// OLO_TEST_LAYER: plumbing
//
// The uniform block binding contract's verdicts (ADR 0025 section 5a, issue
// #1330), without a device. The backends call BindingContract::Check where they
// resolve a binding point's occupant; this pins what it answers.
//
// The cases are the failures the contract exists for, not a truth table: the
// #1437 device loss (a 48-byte block read as a 96-byte one at binding 65), the
// same mistake between two blocks of equal size (which a size check alone would
// pass), and what an undeclared buffer is and is not checked for.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBlockContract.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    namespace
    {
        using BindingContract::BlockDeclaration;
        using BindingContract::BlockRequirement;
        using BindingContract::Check;
        using BindingContract::HashBlockName;
        using BindingContract::Verdict;

        constexpr u32 kRayTracing = ShaderBindingLayout::UBO_RAY_TRACING;

        // The vegetation deform block as its shader declares it: 96 bytes, with
        // the device addresses the shader dereferences at offsets 64 to 87.
        constexpr BlockRequirement kVegetation{ .Binding = kRayTracing,
                                                .Size = 96,
                                                .NameHash = HashBlockName("RayTracingVegetationParams") };

        constexpr BlockDeclaration Declares(std::string_view name, std::string_view alias = {})
        {
            return { .NameHash = HashBlockName(name), .AliasHash = HashBlockName(alias) };
        }
    } // namespace

    // Evaluated at compile time as well: Check is what a static_assert in a
    // producer would use.
    static_assert(Check(kVegetation, 96, Declares("RayTracingVegetationParams")) == Verdict::Satisfied);
    static_assert(Check(kVegetation, 48, Declares("OloSkeletalDeformParams")) == Verdict::Undersized);
    static_assert(HashBlockName("") == 0u, "an empty name is 'no layout declared'");
    static_assert(HashBlockName("Camera") != HashBlockName("CameraMatrices"));

    TEST(UniformBlockContract, TheSkeletalBlockIsRefusedWhereTheVegetationBlockIsRead)
    {
        // #1437 exactly: DeformedSurfaceCache's 48-byte block occupied binding
        // 65 when the vegetation dispatch read its own 96-byte one.
        EXPECT_EQ(Check(kVegetation, 48, Declares("OloSkeletalDeformParams")), Verdict::Undersized);
        // An undeclared 48-byte buffer is refused for the same reason: the byte
        // range needs no declaration.
        EXPECT_EQ(Check(kVegetation, 48, BlockDeclaration{}), Verdict::Undersized);
    }

    TEST(UniformBlockContract, AnotherLayoutOfSufficientSizeIsRefused)
    {
        // The case a size comparison cannot see, and the reason producers on a
        // multi-layout binding declare their block: a tenant at least as large.
        EXPECT_EQ(Check(kVegetation, 96, Declares("ReSTIRDIParams")), Verdict::WrongLayout);
        EXPECT_EQ(Check(kVegetation, 4096, Declares("RayTracingShadowParams")), Verdict::WrongLayout);
    }

    TEST(UniformBlockContract, AnUndeclaredBufferIsJudgedOnItsByteRangeAlone)
    {
        // A buffer that says nothing about itself is a test's ad-hoc buffer:
        // engine buffers on a multi-layout binding are required to declare by
        // UniformBlockContractSourceTest, not by the draw. Its range still counts.
        EXPECT_EQ(Check(kVegetation, 96, BlockDeclaration{}), Verdict::Satisfied);
        EXPECT_EQ(Check(kVegetation, 95, BlockDeclaration{}), Verdict::Undersized);
    }

    TEST(UniformBlockContract, ALargerBufferOfTheRightLayoutAndAnAliasAreAccepted)
    {
        // A buffer may be larger than the block (a shared buffer two shaders
        // declare at two lengths).
        EXPECT_EQ(Check(kVegetation, 128, Declares("RayTracingVegetationParams")), Verdict::Satisfied);
        // One layout, two spellings: the camera block is `CameraMatrices` in
        // the 3D shaders and `Camera` in the 2D and particle ones.
        const BlockRequirement camera2D{ .Binding = ShaderBindingLayout::UBO_CAMERA, .Size = 64, .NameHash = HashBlockName("Camera") };
        EXPECT_EQ(Check(camera2D, 512, Declares("CameraMatrices", "Camera")), Verdict::Satisfied);
        EXPECT_EQ(Check(camera2D, 512, Declares("CameraMatrices")), Verdict::WrongLayout);
    }

    TEST(UniformBlockContract, EveryVerdictHasAReasonAndViolationsAreCounted)
    {
        for (const Verdict verdict : { Verdict::Satisfied, Verdict::Undersized, Verdict::WrongLayout })
            EXPECT_STRNE(BindingContract::ToString(verdict), "unknown");

        const u64 before = BindingContract::ViolationCount();
        // The same refusal twice: logged once, counted twice. The counter is
        // what a test or a frame budget reads; the log is for a person.
        BindingContract::ReportViolation("UniformBlockContractTest", kVegetation, "RayTracingVegetationParams", 48,
                                         "OloSkeletalDeformParams", Verdict::Undersized);
        BindingContract::ReportViolation("UniformBlockContractTest", kVegetation, "RayTracingVegetationParams", 48,
                                         "OloSkeletalDeformParams", Verdict::Undersized);
        EXPECT_EQ(BindingContract::ViolationCount(), before + 2u);
    }
} // namespace OloEngine::Tests
