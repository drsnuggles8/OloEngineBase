// OLO_TEST_LAYER: L8
// =============================================================================
// MaterialOverridesVisualEvidenceTest.cpp
//
// On-screen evidence for issue #1533's MaterialOverridesComponent: a patch of
// ONE imported material reaches the frame, through the real Scene render path,
// on the rigid MeshComponent loop AND the skinned (MeshComponent +
// SkeletonComponent) loop — the one a furred dog's body is drawn by — under
// both the Forward and the Deferred path.
//
// Why a frame and not just the resolver test: on the Deferred path the draw
// takes its material from the GPU Scene RECORD its draw link names, not from
// the material the loop resolved. A patched copy keyed to the shared Imported
// record would resolve green on the CPU and still shade red on screen. Only a
// frame can see that.
//
// The scene: a two-quad mesh whose imported materials are RED (left) and BLUE
// (right), and a skinned cube whose one imported material is RED. Each entity
// carries a MaterialOverridesComponent that patches the red material's base
// colour to GREEN. Predictions, per pixel region:
//   * patched  -> left quad and cube are green-dominant, right quad stays blue;
//   * control  -> with the component removed, left quad and cube are red again.
// The control is what makes the prediction a claim about the override rather
// than about the lighting.
//
// PNGs: OloEditor/assets/tests/visual/MaterialOverrides_<path>_<angle>_<state>.png.
// SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialOverride.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;
        constexpr glm::vec3 kFocus{ 0.0f, 0.0f, 0.0f };

        // The quads stand in the z = 0 plane above the origin; the cube sits below.
        constexpr f32 kQuadBottom = 0.3f;
        constexpr f32 kQuadTop = 2.1f;
        constexpr f32 kLeftX0 = -2.6f;
        constexpr f32 kLeftX1 = -0.4f;
        constexpr f32 kRightX0 = 0.4f;
        constexpr f32 kRightX1 = 2.6f;
        constexpr glm::vec3 kCubeCentre{ 0.0f, -1.7f, 0.0f };

        const glm::vec3 kRed{ 0.85f, 0.08f, 0.06f };
        const glm::vec3 kBlue{ 0.06f, 0.10f, 0.85f };
        const glm::vec4 kGreen{ 0.08f, 0.85f, 0.10f, 1.0f };

        Ref<MeshSource> MakeTwoQuadSource()
        {
            TArray<Vertex> vertices;
            TArray<u32> indices;
            for (const auto [x0, x1] : { std::pair{ kLeftX0, kLeftX1 }, std::pair{ kRightX0, kRightX1 } })
            {
                const u32 base = static_cast<u32>(vertices.Num());
                const glm::vec3 normal(0.0f, 0.0f, 1.0f);
                vertices.Add(Vertex(glm::vec3(x0, kQuadBottom, 0.0f), normal, glm::vec2(0.0f, 0.0f)));
                vertices.Add(Vertex(glm::vec3(x1, kQuadBottom, 0.0f), normal, glm::vec2(1.0f, 0.0f)));
                vertices.Add(Vertex(glm::vec3(x1, kQuadTop, 0.0f), normal, glm::vec2(1.0f, 1.0f)));
                vertices.Add(Vertex(glm::vec3(x0, kQuadTop, 0.0f), normal, glm::vec2(0.0f, 1.0f)));
                for (const u32 i : { 0u, 1u, 2u, 2u, 3u, 0u })
                {
                    indices.Add(base + i);
                }
            }
            auto source = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
            for (u32 s = 0; s < 2; ++s)
            {
                Submesh submesh;
                submesh.m_BaseVertex = 0;
                submesh.m_BaseIndex = s * 6;
                submesh.m_VertexCount = 8;
                submesh.m_IndexCount = 6;
                submesh.m_MaterialIndex = s;
                source->AddSubmesh(submesh);
            }
            source->SetImportedMaterials(TArray<Ref<Material>>{ Material::CreatePBR("LeftCoat", kRed, 0.0f, 0.85f),
                                                                Material::CreatePBR("RightCoat", kBlue, 0.0f, 0.85f) });
            source->Build();
            return source;
        }

        struct Rect
        {
            i32 X0 = 0, X1 = 0, Y0 = 0, Y1 = 0;
            bool Valid = false;
        };

        // World-space box -> pixel rectangle, shrunk to its inner half so edges,
        // anti-aliasing and the silhouette never enter the colour sample. The
        // GL readback is bottom-up, so NDC y maps straight to the row index.
        Rect ProjectInner(const glm::mat4& viewProjection, const glm::vec3& lo, const glm::vec3& hi)
        {
            f32 minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 p((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z);
                const glm::vec4 clip = viewProjection * glm::vec4(p, 1.0f);
                if (clip.w <= 0.0f)
                {
                    return {};
                }
                const f32 px = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(kWidth);
                const f32 py = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<f32>(kHeight);
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
            }
            const f32 insetX = (maxX - minX) * 0.25f;
            const f32 insetY = (maxY - minY) * 0.25f;
            Rect rect;
            rect.X0 = static_cast<i32>(std::clamp(minX + insetX, 0.0f, static_cast<f32>(kWidth - 1)));
            rect.X1 = static_cast<i32>(std::clamp(maxX - insetX, 0.0f, static_cast<f32>(kWidth - 1)));
            rect.Y0 = static_cast<i32>(std::clamp(minY + insetY, 0.0f, static_cast<f32>(kHeight - 1)));
            rect.Y1 = static_cast<i32>(std::clamp(maxY - insetY, 0.0f, static_cast<f32>(kHeight - 1)));
            rect.Valid = rect.X1 > rect.X0 && rect.Y1 > rect.Y0;
            return rect;
        }

        glm::vec3 MeanColour(const std::vector<u8>& rgba, const Rect& rect)
        {
            glm::dvec3 sum(0.0);
            u32 count = 0;
            for (i32 y = rect.Y0; y <= rect.Y1; ++y)
            {
                for (i32 x = rect.X0; x <= rect.X1; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + static_cast<sizet>(x)) * 4;
                    sum += glm::dvec3(rgba[i], rgba[i + 1], rgba[i + 2]);
                    ++count;
                }
            }
            return count > 0 ? glm::vec3(sum / static_cast<f64>(count)) : glm::vec3(0.0f);
        }

        enum class Hue
        {
            Red,
            Green,
            Blue,
            Neither
        };

        const char* HueName(Hue hue)
        {
            switch (hue)
            {
                case Hue::Red:
                    return "red";
                case Hue::Green:
                    return "green";
                case Hue::Blue:
                    return "blue";
                case Hue::Neither:
                    break;
            }
            return "no dominant hue";
        }

        // A channel dominates when it clears both others by a margin — a lit
        // saturated base colour does, a grey or an unlit region does not.
        Hue Dominant(const glm::vec3& c)
        {
            constexpr f32 kMargin = 20.0f;
            if (c.r > c.g + kMargin && c.r > c.b + kMargin)
                return Hue::Red;
            if (c.g > c.r + kMargin && c.g > c.b + kMargin)
                return Hue::Green;
            if (c.b > c.r + kMargin && c.b > c.g + kMargin)
                return Hue::Blue;
            return Hue::Neither;
        }

        void WritePng(const std::string& name, const std::vector<u8>& rgba)
        {
            std::vector<u8> flipped(rgba.size()); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4;
            for (u32 y = 0; y < kHeight; ++y)
            {
                std::memcpy(flipped.data() + static_cast<sizet>(y) * rowBytes,
                            rgba.data() + static_cast<sizet>(kHeight - 1 - y) * rowBytes, rowBytes);
            }
            std::filesystem::create_directories("assets/tests/visual");
            const std::string path = "assets/tests/visual/" + name;
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       flipped.data(), static_cast<int>(rowBytes)),
                      0)
                << "failed to write evidence PNG " << path;
        }
    } // namespace

    class MaterialOverridesVisualEvidence : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            Entity sun = scene.CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(0.25f, -0.35f, -1.0f));
            light.m_Color = glm::vec3(1.0f);
            light.m_Intensity = 3.0f;
            light.m_CastShadows = false;

            m_Quads = scene.CreateEntity("Quads");
            m_QuadSource = MakeTwoQuadSource();
            m_Quads.AddComponent<MeshComponent>(m_QuadSource);

            // The skinned loop's subject: a multi-bone cube whose one submesh
            // gets an imported material to patch, like the dog body's nose.
            m_SkinnedMesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
            if (m_SkinnedMesh)
            {
                // Non-const: Ref<T> propagates const, and the table is written below.
                Ref<MeshSource> skinnedSource = m_SkinnedMesh->GetMeshSource();
                skinnedSource->SetImportedMaterials(TArray<Ref<Material>>{ Material::CreatePBR("CubeCoat", kRed, 0.0f, 0.85f) });
                m_Cube = scene.CreateEntity("SkinnedCube");
                m_Cube.GetComponent<TransformComponent>().Translation = kCubeCentre;
                m_Cube.AddComponent<MeshComponent>(skinnedSource);
                const Skeleton* sourceSkeleton = skinnedSource->GetSkeleton();
                if (sourceSkeleton)
                {
                    auto skeleton = Ref<Skeleton>::Create(sourceSkeleton->m_LocalTransforms.size());
                    static_cast<SkeletonData&>(*skeleton) = static_cast<const SkeletonData&>(*sourceSkeleton);
                    m_Cube.AddComponent<SkeletonComponent>(skeleton);
                }
            }

            AttachOverrides();
        }

        void AttachOverrides()
        {
            MaterialOverride quadPatch;
            quadPatch.MaterialName = "LeftCoat";
            quadPatch.OverrideBaseColor = true;
            quadPatch.BaseColor = kGreen;
            m_Quads.AddComponent<MaterialOverridesComponent>().m_Overrides.Add(quadPatch);

            if (m_Cube)
            {
                MaterialOverride cubePatch;
                cubePatch.MaterialName = "CubeCoat";
                cubePatch.OverrideBaseColor = true;
                cubePatch.BaseColor = kGreen;
                m_Cube.AddComponent<MaterialOverridesComponent>().m_Overrides.Add(cubePatch);
            }
        }

        void DetachOverrides()
        {
            m_Quads.RemoveComponent<MaterialOverridesComponent>();
            if (m_Cube)
            {
                m_Cube.RemoveComponent<MaterialOverridesComponent>();
            }
        }

        struct Frame
        {
            std::vector<u8> Rgba;
            glm::mat4 ViewProjection{ 1.0f };
        };

        Frame Capture(const std::string& name, f32 yawDegrees, f32 pitchDegrees)
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(kFocus, 8.5f, glm::radians(yawDegrees), glm::radians(pitchDegrees));
            RunEditorFrames(camera, 4);

            Frame frame;
            frame.ViewProjection = camera.GetViewProjection();
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(frame.Rgba, w, h))
            {
                ADD_FAILURE() << "composite readback unavailable for " << name;
                return {};
            }
            EXPECT_EQ(w, kWidth);
            EXPECT_EQ(h, kHeight);
            WritePng("MaterialOverrides_" + name + ".png", frame.Rgba);
            return frame;
        }

        void ExpectHue(const Frame& frame, const char* region, const glm::vec3& lo, const glm::vec3& hi, Hue expected)
        {
            const Rect rect = ProjectInner(frame.ViewProjection, lo, hi);
            ASSERT_TRUE(rect.Valid) << region << " projects to no pixels";
            const glm::vec3 mean = MeanColour(frame.Rgba, rect);
            EXPECT_EQ(Dominant(mean), expected)
                << region << " should be " << HueName(expected) << " but its mean colour is (" << mean.r << ", "
                << mean.g << ", " << mean.b << ") — " << HueName(Dominant(mean));
        }

        void ExpectFrame(const Frame& frame, bool patched)
        {
            ASSERT_FALSE(frame.Rgba.empty());
            ExpectHue(frame, "left quad", { kLeftX0, kQuadBottom, 0.0f }, { kLeftX1, kQuadTop, 0.0f },
                      patched ? Hue::Green : Hue::Red);
            ExpectHue(frame, "right quad (never patched)", { kRightX0, kQuadBottom, 0.0f }, { kRightX1, kQuadTop, 0.0f },
                      Hue::Blue);
            if (m_Cube)
            {
                // The extent from the VERTICES: the skinned primitive's cached
                // bounding box is not the cube's, and a rect built from it
                // clamped to the whole frame and averaged every object in it.
                glm::vec3 lo(1.0e9f);
                glm::vec3 hi(-1.0e9f);
                for (const Vertex& v : m_SkinnedMesh->GetMeshSource()->GetVertices())
                {
                    lo = glm::min(lo, v.Position);
                    hi = glm::max(hi, v.Position);
                }
                const glm::vec3 centre = kCubeCentre + (lo + hi) * 0.5f;
                const glm::vec3 half = (hi - lo) * 0.25f; // the cube's middle, not its silhouette
                ExpectHue(frame, "skinned cube", centre - half, centre + half, patched ? Hue::Green : Hue::Red);
            }
        }

        Entity m_Quads;
        Entity m_Cube;
        Ref<MeshSource> m_QuadSource;
        Ref<Mesh> m_SkinnedMesh;
    };

    TEST_F(MaterialOverridesVisualEvidence, APatchedImportedMaterialReachesTheFrameOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_QuadSource && m_QuadSource->HasVertexBuffer()) << "the two-quad mesh did not build";
        ASSERT_TRUE(m_Cube) << "the skinned primitive is unavailable, so the skinned loop would go untested";

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::Deferred })
        {
            const std::string pathName = path == RenderingPath::Forward ? "Forward" : "Deferred";
            SCOPED_TRACE(pathName);
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Two angles, so the result is not one lucky view.
            ExpectFrame(Capture(pathName + "_Front_Patched", 0.0f, 8.0f), /*patched=*/true);
            ExpectFrame(Capture(pathName + "_Side_Patched", 28.0f, 16.0f), /*patched=*/true);

            // The control: the same scene without the component shades the
            // imported red again, so the green above is the override's doing.
            DetachOverrides();
            ExpectFrame(Capture(pathName + "_Front_Control", 0.0f, 8.0f), /*patched=*/false);
            AttachOverrides();
        }
    }
} // namespace OloEngine::Tests
