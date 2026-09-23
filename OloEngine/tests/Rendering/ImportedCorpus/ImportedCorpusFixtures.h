#pragma once

// Shared helpers for the imported-asset fixture corpus (issue #1350).
//
// Two kinds of fixture live side by side, on purpose (criterion 4 of #1350):
//
//   * REAL imported assets -- the committed Khronos glTF-Sample-Assets listed in
//     OloEditor/assets/tests/imported-corpus/corpus.yaml. They are what real content looks
//     like, and they are what a regression in the import path actually breaks.
//   * EXACT MINIMAL cases -- glTF files authored here, one adversarial property each, so a
//     failure localises to one vertex class or one node. No redistributable real asset
//     carries a NaN bone weight or a rigid attachment under a joint, and the four rigged
//     Khronos samples in the corpus were checked for zero-weight vertices and have none, so
//     those cases exist ONLY as minimal fixtures. The corpus test says so rather than
//     implying real coverage it does not have.
//
// Every minimal fixture goes through the production importer (AnimatedModel / Model via
// Assimp) exactly like a committed asset; only the bytes are generated.

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    class MeshSource;
    class Skeleton;
    struct Submesh;
} // namespace OloEngine

namespace OloEngine::Tests::ImportedCorpus
{
    // ── Corpus paths ─────────────────────────────────────────────────────────────────
    //
    // Absolute, rooted at OLO_TEST_EDITOR_ROOT, so a test does not depend on which
    // directory the binary was launched from.
    [[nodiscard]] std::filesystem::path EditorRoot();
    [[nodiscard]] std::filesystem::path CorpusManifestPath();
    [[nodiscard]] std::filesystem::path ModelPath(const char* relativeToModels);

    // Drop BOTH .omesh namespaces (static and animated) so the next import is cold.
    void MakeColdImport(const std::filesystem::path& path);

    // ── A minimal glTF 2.0 writer ─────────────────────────────────────────────────────
    //
    // Accessors are appended to one external .bin; the caller supplies the scene graph,
    // meshes, skins, animations and materials as JSON text. Only what the authored
    // fixtures need -- this is not a general exporter.
    class GltfBuilder
    {
      public:
        // componentType: 5126 float, 5123 u16, 5121 u8, 5125 u32. type: "SCALAR", "VEC2"...
        u32 AddAccessor(const void* data, sizet bytes, u32 componentType, const char* type, u32 count,
                        const std::string& extraJson = {});
        u32 AddFloats(const std::vector<f32>& values, const char* type, bool withMinMax = false);
        u32 AddU16(const std::vector<u16>& values, const char* type);

        // Writes <dir>/<name>.gltf and <dir>/<name>.bin. `bodyJson` is the text that goes
        // after "asset", i.e. `"scene":0,"scenes":[...],"nodes":[...],...` WITHOUT the
        // buffers/bufferViews/accessors, which this class owns.
        [[nodiscard]] std::filesystem::path Write(const std::filesystem::path& dir, const std::string& name,
                                                  const std::string& bodyJson) const;

      private:
        std::vector<u8> m_Bin;
        std::vector<std::string> m_BufferViews;
        std::vector<std::string> m_Accessors;
    };

    // ── Minimal case A: rigid attachment + unweighted vertices on an animated arm ──────
    //
    //   joint 0 "Root"  at the origin
    //   joint 1 "Arm"   child of Root, rest translation (0, 1, 0), animated: rotates about
    //                   +Z from 0 to 90 degrees over kArmClipSeconds (LINEAR)
    //   mesh "Body"     skinned: a 0.2-wide strip from y = 0 to y = 2. Bottom row weighted
    //                   to Root, middle row half/half, top row to Arm. PLUS a detached
    //                   "flap" quad at x = 0.6..0.8, y = 0..0.2 whose four vertices carry
    //                   ALL-ZERO weights -- the unweighted-vertex class.
    //   mesh "Blade"    RIGID (no skin): a quad in the XY plane, child of Arm with local
    //                   translation (kBladeOffset, 0, 0). A sword in a hand.
    //
    // Expected semantics, which the rigid-attachment tests pin:
    //   * the blade follows the Arm joint exactly -- at 90 degrees its local +X offset has
    //     swung to world +Y above the pivot;
    //   * the unweighted flap stays at its rest position whatever the arm does.
    inline constexpr f32 kArmClipSeconds = 1.0f;
    inline constexpr f32 kArmPivotY = 1.0f;
    inline constexpr f32 kBladeOffset = 0.5f;
    inline constexpr f32 kBladeHalfSize = 0.1f;
    inline constexpr f32 kFlapMinX = 0.6f;
    inline constexpr f32 kFlapMaxX = 0.8f;
    inline constexpr f32 kFlapMaxY = 0.2f;

    [[nodiscard]] std::filesystem::path AuthorRigidAttachmentFixture(const std::filesystem::path& dir);

    // The blade's rest-pose corners in model space (before any animation).
    [[nodiscard]] std::vector<glm::vec3> BladeRestCorners();
    // Where a model-space rest point ends up when the Arm joint has rotated `radians`
    // about +Z around its pivot -- the reference the attachment tests compare against.
    [[nodiscard]] glm::vec3 RotateAboutArmPivot(const glm::vec3& restPoint, f32 radians);

    // ── Minimal case B: one vertex per invalid-influence class ─────────────────────────
    //
    // A strip of kInfluenceCaseCount vertex PAIRS (so it forms real triangles and survives
    // import), two joints. Pair i's two vertices carry the SAME influence, class i below.
    // Each class is what a real exporter has been seen to emit, or what a hand-edited
    // file can contain; glTF-Validator rejects most of them, Assimp imports all of them.
    enum class InfluenceCase : u32
    {
        Valid = 0,      // (Root 1.0)                     -> unchanged
        NaNWeight,      // (Root 1.0, Arm NaN)            -> NaN slot dropped: (Root 1.0)
        NegativeWeight, // (Root 1.5, Arm -0.5)           -> negative dropped: (Root 1.0)
        ZeroSum,        // all four weights 0             -> unweighted (rest pose)
        Unnormalised,   // (Root 2.0, Arm 2.0)            -> (0.5, 0.5)
        AllNaN,         // (Root NaN)                     -> unweighted (rest pose)
        InfiniteWeight, // (Root +inf, Arm 1.0)           -> inf dropped: (Arm 1.0)
        Count
    };
    inline constexpr u32 kInfluenceCaseCount = static_cast<u32>(InfluenceCase::Count);
    [[nodiscard]] const char* ToString(InfluenceCase c);

    // x coordinate of pair i -- lets a test find a case's vertices after the importer has
    // reordered them (MeshOptimization reorders for vertex cache locality).
    [[nodiscard]] f32 InfluenceCaseX(u32 caseIndex);

    [[nodiscard]] std::filesystem::path AuthorInvalidInfluenceFixture(const std::filesystem::path& dir);

    // Two material slots share geometric edge positions but deliberately have
    // different UVs and normals there. The importer must keep both vertices.
    [[nodiscard]] std::filesystem::path AuthorSeamFixture(const std::filesystem::path& dir);

    // ── CPU skinning with the production convention ────────────────────────────────────
    //
    // Mirrors SkeletalDeformation.glsl (OloSkinMatrix): an unweighted vertex passes
    // through with the identity, and position.w is the total weight. Used to ask "where
    // would the GPU put this vertex" without a readback.
    [[nodiscard]] glm::vec3 SkinPosition(const MeshSource& source, u32 vertexIndex,
                                         const std::vector<glm::mat4>& finalBoneMatrices);
} // namespace OloEngine::Tests::ImportedCorpus
