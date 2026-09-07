#pragma once

// =============================================================================
// ReferenceSceneFixtures.h — the scenes the offline reference path tracer is
// validated on (issue #709).
//
// Kept in ONE place because several tests must trace the *same* geometry:
// the furnace anchor, the Cornell-box determinism gate and the DDGI parity
// test all need to agree on what the room is, or a divergence between them
// would be a fixture difference masquerading as a renderer bug.
//
// Everything is built from literals — no assets, no GL, no Application — so
// every consumer runs headless.
// =============================================================================

#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

namespace OloEngine::Tests::PathTracingFixtures
{
    using namespace OloEngine::PathTracing;

    // -------------------------------------------------------------------------
    // Geometry helpers.
    //
    // Winding matters: ReferenceScene derives a quad's normal from
    // cross(p1-p0, p2-p0), and the integrator uses the GEOMETRIC normal to
    // decide which face of a one-sided emitter is lit. A back-to-front ceiling
    // light is a scene that renders completely black for a reason that looks
    // like an integrator bug, so each helper below states the normal it
    // produces.
    // -------------------------------------------------------------------------

    // Axis-aligned quad on the y = `y` plane, normal +Y.
    inline u32 AddFloorQuad(ReferenceScene& scene, f32 y, f32 minX, f32 maxX, f32 minZ, f32 maxZ, u32 material)
    {
        const u32 geometry = scene.AddQuadGeometry(glm::vec3(minX, y, minZ), glm::vec3(minX, y, maxZ),
                                                   glm::vec3(maxX, y, maxZ), glm::vec3(maxX, y, minZ));
        return scene.AddInstance(geometry, glm::mat4(1.0f), material);
    }

    // Axis-aligned quad on the y = `y` plane, normal -Y (a ceiling / downward
    // emitter).
    inline u32 AddCeilingQuad(ReferenceScene& scene, f32 y, f32 minX, f32 maxX, f32 minZ, f32 maxZ, u32 material)
    {
        const u32 geometry = scene.AddQuadGeometry(glm::vec3(minX, y, minZ), glm::vec3(maxX, y, minZ),
                                                   glm::vec3(maxX, y, maxZ), glm::vec3(minX, y, maxZ));
        return scene.AddInstance(geometry, glm::mat4(1.0f), material);
    }

    // Axis-aligned quad on the z = `z` plane, normal +Z.
    inline u32 AddBackWallQuad(ReferenceScene& scene, f32 z, f32 minX, f32 maxX, f32 minY, f32 maxY, u32 material)
    {
        const u32 geometry = scene.AddQuadGeometry(glm::vec3(minX, minY, z), glm::vec3(maxX, minY, z),
                                                   glm::vec3(maxX, maxY, z), glm::vec3(minX, maxY, z));
        return scene.AddInstance(geometry, glm::mat4(1.0f), material);
    }

    // Axis-aligned quad on the x = `x` plane, normal +X (a LEFT wall seen from
    // inside the room).
    inline u32 AddLeftWallQuad(ReferenceScene& scene, f32 x, f32 minY, f32 maxY, f32 minZ, f32 maxZ, u32 material)
    {
        const u32 geometry = scene.AddQuadGeometry(glm::vec3(x, minY, minZ), glm::vec3(x, maxY, minZ),
                                                   glm::vec3(x, maxY, maxZ), glm::vec3(x, minY, maxZ));
        return scene.AddInstance(geometry, glm::mat4(1.0f), material);
    }

    // Axis-aligned quad on the x = `x` plane, normal -X (a RIGHT wall).
    inline u32 AddRightWallQuad(ReferenceScene& scene, f32 x, f32 minY, f32 maxY, f32 minZ, f32 maxZ, u32 material)
    {
        const u32 geometry = scene.AddQuadGeometry(glm::vec3(x, minY, minZ), glm::vec3(x, minY, maxZ),
                                                   glm::vec3(x, maxY, maxZ), glm::vec3(x, maxY, minZ));
        return scene.AddInstance(geometry, glm::mat4(1.0f), material);
    }

    // Closed axis-aligned box with OUTWARD normals, assembled from the six
    // quad helpers so the winding is the already-verified one.
    inline void AddBox(ReferenceScene& scene, const glm::vec3& boxMin, const glm::vec3& boxMax, u32 material)
    {
        AddFloorQuad(scene, boxMax.y, boxMin.x, boxMax.x, boxMin.z, boxMax.z, material);     // +Y
        AddCeilingQuad(scene, boxMin.y, boxMin.x, boxMax.x, boxMin.z, boxMax.z, material);   // -Y
        AddLeftWallQuad(scene, boxMax.x, boxMin.y, boxMax.y, boxMin.z, boxMax.z, material);  // +X
        AddRightWallQuad(scene, boxMin.x, boxMin.y, boxMax.y, boxMin.z, boxMax.z, material); // -X
        AddBackWallQuad(scene, boxMax.z, boxMin.x, boxMax.x, boxMin.y, boxMax.y, material);  // +Z
        // -Z: the back-wall winding reversed.
        const u32 backGeometry = scene.AddQuadGeometry(glm::vec3(boxMin.x, boxMin.y, boxMin.z),
                                                       glm::vec3(boxMin.x, boxMax.y, boxMin.z),
                                                       glm::vec3(boxMax.x, boxMax.y, boxMin.z),
                                                       glm::vec3(boxMax.x, boxMin.y, boxMin.z));
        scene.AddInstance(backGeometry, glm::mat4(1.0f), material);
    }

    // -------------------------------------------------------------------------
    // The white-furnace anchor: a single infinite-ish plane under a uniform
    // environment of radiance 1, viewed along its own normal.
    //
    // Deliberately a single CONVEX surface: a ray leaving the plane can never
    // hit it again, so the converged radiance is exactly the single-scatter
    // directional albedo
    //     integral of f(l, v) * (n . l) dw
    // — precisely the quantity PbrFurnaceProbe.glsl estimates on the GPU with
    // N = V = +Z. That is what makes the two directly comparable rather than
    // merely "both about 1".
    // -------------------------------------------------------------------------
    struct FurnacePlaneScene
    {
        ReferenceScene Scene;
        u32 MaterialIndex = 0;

        // Trace straight down at the plane from above: N = V exactly.
        [[nodiscard]] Ray ViewRay() const
        {
            return Ray(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
        }
    };

    inline FurnacePlaneScene MakeFurnacePlaneScene(f32 roughness, f32 metallic,
                                                   const glm::vec3& albedo = glm::vec3(1.0f),
                                                   f32 environmentRadiance = 1.0f)
    {
        FurnacePlaneScene fixture;

        ReferenceMaterial material;
        material.BaseColor = albedo;
        material.Metallic = metallic;
        material.Roughness = roughness;
        fixture.MaterialIndex = fixture.Scene.AddMaterial(material);

        // Large enough that a cosine-sampled bounce essentially never leaves it
        // sideways within the traced depth.
        AddFloorQuad(fixture.Scene, 0.0f, -1000.0f, 1000.0f, -1000.0f, 1000.0f, fixture.MaterialIndex);

        ReferenceEnvironment environment;
        environment.Radiance = glm::vec3(environmentRadiance);
        fixture.Scene.SetEnvironment(environment);

        fixture.Scene.Build();
        return fixture;
    }

    // -------------------------------------------------------------------------
    // Cornell box.
    //
    // Room spans [-1, 1]^3, open at +Z (where the camera sits). Left wall red,
    // right wall green, everything else white; one downward-facing emissive
    // quad under the ceiling, and a white block on the floor to cast a real
    // shadow and occlude part of the indirect bounce.
    // -------------------------------------------------------------------------
    // A texture the fixture owns TWICE: the 8-bit pixels a Texture2D is
    // uploaded from and the decoded ReferenceTexture the CPU samples, so the
    // two tracers read one image. `Srgb` says which decode the GPU format
    // applies and FromRgba8 mirrored.
    struct FixtureImage
    {
        u32 Width = 0;
        u32 Height = 0;
        bool Srgb = false;
        std::vector<u8> Rgba;
        std::shared_ptr<const ReferenceTexture> Reference;
    };

    struct CornellBoxScene
    {
        ReferenceScene Scene;
        u32 WhiteMaterial = 0;
        u32 RedMaterial = 0;
        u32 GreenMaterial = 0;
        u32 LightMaterial = 0;
        // The textured variant's extra materials (MakeTexturedCornellBoxScene);
        // unused by the plain box.
        u32 FloorMaterial = 0;
        u32 BoxMaterial = 0;
        u32 MaskMaterial = 0;
        std::vector<FixtureImage> Images;

        [[nodiscard]] const FixtureImage* FindImage(const ReferenceTexture* texture) const
        {
            for (const FixtureImage& image : Images)
            {
                if (image.Reference.get() == texture)
                    return &image;
            }
            return nullptr;
        }

        [[nodiscard]] static glm::vec3 EyePosition()
        {
            return glm::vec3(0.0f, 0.0f, 3.0f);
        }

        [[nodiscard]] static glm::mat4 ViewProjection(u32 width, u32 height)
        {
            const glm::mat4 view = glm::lookAt(EyePosition(), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 projection = glm::perspective(glm::radians(45.0f),
                                                          static_cast<f32>(width) / static_cast<f32>(height),
                                                          0.1f, 100.0f);
            return projection * view;
        }

        // Camera looking down -Z into the open face.
        [[nodiscard]] ReferenceCamera MakeCamera(u32 width, u32 height) const
        {
            return ReferenceCamera::FromViewProjection(ViewProjection(width, height), EyePosition());
        }

        // A second, RAKING view for evidence: INSIDE the room, high on the
        // right, looking down across the floor at the block.
        //
        // The head-on camera sees the block flat-on, so its side faces — where
        // colour bleeding is strongest and most legible — are edge-on or hidden.
        // From here the block's left face reads visibly RED and its right face
        // GREEN, which is the clearest possible read on whether indirect light
        // is carrying wall albedo. Head-on covers what this pose cannot: the
        // ceiling and the emitter. CLAUDE.md asks for multiple angles precisely
        // so one pose's blind spot is not mistaken for correctness.
        //
        // Inside rather than outside the open +Z face on purpose: an oblique
        // camera placed outside sees mostly past the opening, and ~18% of that
        // frame is empty black. This pose measures 0.4%.
        [[nodiscard]] static glm::vec3 RakingEyePosition()
        {
            return glm::vec3(0.75f, 0.72f, 0.88f);
        }

        [[nodiscard]] static ReferenceCamera MakeRakingCamera(u32 width, u32 height)
        {
            const glm::vec3 eye = RakingEyePosition();
            const glm::mat4 view = glm::lookAt(eye, glm::vec3(-0.35f, -0.90f, -0.45f), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 projection = glm::perspective(glm::radians(62.0f),
                                                          static_cast<f32>(width) / static_cast<f32>(height),
                                                          0.05f, 100.0f);
            return ReferenceCamera::FromViewProjection(projection * view, eye);
        }

        // Project a world point to a pixel coordinate (row 0 == top), the
        // inverse of ReferenceCamera::GenerateRay.
        //
        // Region assertions MUST be anchored this way rather than with
        // eyeballed pixel fractions. Getting it wrong is not a loud failure:
        // a "floor near the red wall" rectangle that actually lands ON the red
        // wall still passes a redness assertion — it just stops measuring
        // colour bleeding and starts measuring the wall's own albedo, and the
        // test then holds green while indirect lighting is entirely broken.
        [[nodiscard]] static glm::ivec2 ProjectToPixel(const glm::vec3& worldPosition, u32 width, u32 height)
        {
            const glm::vec4 clip = ViewProjection(width, height) * glm::vec4(worldPosition, 1.0f);
            if (!(std::abs(clip.w) > 0.0f))
                return glm::ivec2(-1);

            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const f32 u = (ndc.x * 0.5f + 0.5f) * static_cast<f32>(width);
            const f32 v = (0.5f - ndc.y * 0.5f) * static_cast<f32>(height);
            return glm::ivec2(static_cast<i32>(std::floor(u)), static_cast<i32>(std::floor(v)));
        }
    };

    // For the multi-bounce white-furnace variant of this same geometry — no
    // emitter, uniform albedo, uniform environment — use MakeCornellFurnaceScene
    // below. Passing `emissiveRadiance = 0` here does NOT produce it: it just
    // removes the only light source and renders black.
    // `model` selects the closure every material traces with. Legacy is the
    // default the existing gates and hashes were pinned on; the GPU path
    // tracer's parity test (issue #1055) asks for ClosureV2, because the GPU
    // shades every hit with the v2 closure — that is the model whose
    // Evaluate / Sample / Pdf triple has GLSL twins.
    inline CornellBoxScene MakeCornellBoxScene(f32 emissiveRadiance = 18.0f, PBRModel model = PBRModel::Legacy)
    {
        CornellBoxScene fixture;
        ReferenceScene& scene = fixture.Scene;

        ReferenceMaterial white;
        white.BaseColor = glm::vec3(0.73f);
        white.Roughness = 1.0f;
        white.Model = model;
        fixture.WhiteMaterial = scene.AddMaterial(white);

        ReferenceMaterial red = white;
        red.BaseColor = glm::vec3(0.65f, 0.05f, 0.05f);
        fixture.RedMaterial = scene.AddMaterial(red);

        ReferenceMaterial green = white;
        green.BaseColor = glm::vec3(0.12f, 0.45f, 0.15f);
        fixture.GreenMaterial = scene.AddMaterial(green);

        ReferenceMaterial light;
        light.BaseColor = glm::vec3(0.0f); // the emitter is black to reflection
        light.Roughness = 1.0f;
        light.Emissive = glm::vec3(emissiveRadiance);
        light.Model = model;
        fixture.LightMaterial = scene.AddMaterial(light);

        AddFloorQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddCeilingQuad(scene, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddBackWallQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddLeftWallQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.RedMaterial);
        AddRightWallQuad(scene, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.GreenMaterial);

        // Ceiling emitter, one-sided, facing down.
        AddCeilingQuad(scene, 0.98f, -0.3f, 0.3f, -0.3f, 0.3f, fixture.LightMaterial);

        // A block on the floor: casts a shadow and blocks part of the bounce.
        AddBox(scene, glm::vec3(-0.55f, -1.0f, -0.55f), glm::vec3(-0.05f, -0.2f, -0.05f), fixture.WhiteMaterial);

        scene.Build();
        return fixture;
    }

    // The Cornell box turned into a MULTI-BOUNCE white furnace: identical
    // geometry, every surface a perfect white diffuse reflector, no emitter,
    // and a uniform environment of radiance 1 entering through the open face.
    // With a perfectly energy-conserving BRDF every pixel would converge to
    // exactly 1.0; anything ABOVE that is energy created by the transport, and
    // is the check the analytic furnace test defines as "a hard bug".
    inline CornellBoxScene MakeCornellFurnaceScene(f32 albedo = 1.0f, f32 environmentRadiance = 1.0f)
    {
        CornellBoxScene fixture;
        ReferenceScene& scene = fixture.Scene;

        ReferenceMaterial white;
        white.BaseColor = glm::vec3(albedo);
        white.Roughness = 1.0f;
        white.Metallic = 0.0f;
        fixture.WhiteMaterial = scene.AddMaterial(white);
        fixture.RedMaterial = fixture.WhiteMaterial;
        fixture.GreenMaterial = fixture.WhiteMaterial;
        fixture.LightMaterial = fixture.WhiteMaterial;

        AddFloorQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddCeilingQuad(scene, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddBackWallQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddLeftWallQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddRightWallQuad(scene, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddBox(scene, glm::vec3(-0.55f, -1.0f, -0.55f), glm::vec3(-0.05f, -0.2f, -0.05f), fixture.WhiteMaterial);

        ReferenceEnvironment environment;
        environment.Radiance = glm::vec3(environmentRadiance);
        scene.SetEnvironment(environment);

        scene.Build();
        return fixture;
    }

    // ---- procedural images for the textured box -----------------------------
    // Small, blocky and asymmetric on purpose: a texel is several pixels wide at
    // the parity resolution, so a swapped axis, a wrong wrap or an sRGB decode
    // applied on one side only moves a region mean by far more than the budget.

    inline FixtureImage FinishImage(FixtureImage image)
    {
        image.Reference = std::make_shared<const ReferenceTexture>(
            ReferenceTexture::FromRgba8(image.Width, image.Height, std::span<const u8>(image.Rgba), image.Srgb));
        return image;
    }

    inline FixtureImage MakeCheckerImage(u32 size, u32 cell, const glm::u8vec3& a, const glm::u8vec3& b)
    {
        FixtureImage image;
        image.Width = size;
        image.Height = size;
        image.Srgb = true;
        image.Rgba.resize(static_cast<sizet>(size) * size * 4u);
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const bool odd = (((x / cell) + (y / cell)) & 1u) != 0u;
                const glm::u8vec3 c = odd ? b : a;
                u8* texel = &image.Rgba[(static_cast<sizet>(y) * size + x) * 4u];
                texel[0] = c.r;
                texel[1] = c.g;
                texel[2] = c.b;
                texel[3] = 255;
            }
        }
        return FinishImage(std::move(image));
    }

    // White rgb with two transparent horizontal slats: a masked quad cut from
    // it casts a striped shadow.
    inline FixtureImage MakeSlatMaskImage(u32 size)
    {
        FixtureImage image;
        image.Width = size;
        image.Height = size;
        image.Srgb = true;
        image.Rgba.resize(static_cast<sizet>(size) * size * 4u);
        for (u32 y = 0; y < size; ++y)
        {
            const u32 band = (y * 8u) / size; // 8 bands; bands 2 and 5 are open
            const bool open = band == 2u || band == 5u;
            for (u32 x = 0; x < size; ++x)
            {
                u8* texel = &image.Rgba[(static_cast<sizet>(y) * size + x) * 4u];
                texel[0] = 255;
                texel[1] = 255;
                texel[2] = 255;
                texel[3] = open ? 0 : 255;
            }
        }
        return FinishImage(std::move(image));
    }

    // A left-to-right ramp: an emitter whose radiance varies across its face.
    inline FixtureImage MakeRampEmissiveImage(u32 size)
    {
        FixtureImage image;
        image.Width = size;
        image.Height = size;
        image.Srgb = true;
        image.Rgba.resize(static_cast<sizet>(size) * size * 4u);
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const u8 v = static_cast<u8>(64u + (191u * x) / std::max(size - 1u, 1u));
                u8* texel = &image.Rgba[(static_cast<sizet>(y) * size + x) * 4u];
                texel[0] = v;
                texel[1] = static_cast<u8>(255u - v / 2u);
                texel[2] = v;
                texel[3] = 255;
            }
        }
        return FinishImage(std::move(image));
    }

    // glTF metallic-roughness: G = roughness in stripes, B = metallic on the
    // right half. Linear data.
    inline FixtureImage MakeMetallicRoughnessImage(u32 size)
    {
        FixtureImage image;
        image.Width = size;
        image.Height = size;
        image.Srgb = false;
        image.Rgba.resize(static_cast<sizet>(size) * size * 4u);
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                u8* texel = &image.Rgba[(static_cast<sizet>(y) * size + x) * 4u];
                texel[0] = 0;
                texel[1] = ((x / 2u) & 1u) != 0u ? 255 : 140;
                texel[2] = x >= size / 2u ? 255 : 0;
                texel[3] = 255;
            }
        }
        return FinishImage(std::move(image));
    }

    // A tangent-space normal map of a sinusoidal bump field, xy encoded as
    // (n * 0.5 + 0.5). Linear data.
    inline FixtureImage MakeBumpNormalImage(u32 size)
    {
        FixtureImage image;
        image.Width = size;
        image.Height = size;
        image.Srgb = false;
        image.Rgba.resize(static_cast<sizet>(size) * size * 4u);
        constexpr f32 kTwoPi = 6.28318530717958647692f;
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size);
                const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size);
                const f32 nx = 0.35f * std::sin(kTwoPi * u);
                const f32 ny = 0.35f * std::sin(kTwoPi * v);
                u8* texel = &image.Rgba[(static_cast<sizet>(y) * size + x) * 4u];
                texel[0] = static_cast<u8>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
                texel[1] = static_cast<u8>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
                texel[2] = 255;
                texel[3] = 255;
            }
        }
        return FinishImage(std::move(image));
    }

    // The Cornell box with every kind of map the tracers sample: a checker
    // albedo on the floor, a metallic-roughness and a normal map on the block,
    // a ramp emissive map on the light, and a slatted alpha-MASK quad hung
    // under the light. ClosureV2 throughout (the closure both tracers share).
    inline CornellBoxScene MakeTexturedCornellBoxScene(f32 emissiveRadiance = 18.0f)
    {
        CornellBoxScene fixture;
        ReferenceScene& scene = fixture.Scene;

        fixture.Images.push_back(MakeCheckerImage(16u, 4u, glm::u8vec3(200, 200, 200), glm::u8vec3(60, 60, 60)));
        fixture.Images.push_back(MakeSlatMaskImage(16u));
        fixture.Images.push_back(MakeRampEmissiveImage(8u));
        fixture.Images.push_back(MakeMetallicRoughnessImage(8u));
        fixture.Images.push_back(MakeBumpNormalImage(16u));
        const auto& checker = fixture.Images[0].Reference;
        const auto& slats = fixture.Images[1].Reference;
        const auto& ramp = fixture.Images[2].Reference;
        const auto& metallicRoughness = fixture.Images[3].Reference;
        const auto& bumps = fixture.Images[4].Reference;

        ReferenceMaterial white;
        white.BaseColor = glm::vec3(0.73f);
        white.Roughness = 1.0f;
        white.Model = PBRModel::ClosureV2;
        fixture.WhiteMaterial = scene.AddMaterial(white);

        ReferenceMaterial red = white;
        red.BaseColor = glm::vec3(0.65f, 0.05f, 0.05f);
        fixture.RedMaterial = scene.AddMaterial(red);

        ReferenceMaterial green = white;
        green.BaseColor = glm::vec3(0.12f, 0.45f, 0.15f);
        fixture.GreenMaterial = scene.AddMaterial(green);

        ReferenceMaterial floor = white;
        floor.BaseColor = glm::vec3(1.0f);
        floor.AlbedoMap = checker;
        fixture.FloorMaterial = scene.AddMaterial(floor);

        ReferenceMaterial box = white;
        box.BaseColor = glm::vec3(0.6f, 0.62f, 0.7f);
        box.Metallic = 1.0f;  // times the map's blue: metallic on the right half
        box.Roughness = 1.0f; // times the map's green: rough stripes
        box.MetallicRoughnessMap = metallicRoughness;
        box.NormalMap = bumps;
        box.NormalScale = 1.0f;
        fixture.BoxMaterial = scene.AddMaterial(box);

        ReferenceMaterial light;
        light.BaseColor = glm::vec3(0.0f);
        light.Roughness = 1.0f;
        light.Emissive = glm::vec3(emissiveRadiance);
        light.EmissiveMap = ramp;
        light.Model = PBRModel::ClosureV2;
        fixture.LightMaterial = scene.AddMaterial(light);

        ReferenceMaterial mask = white;
        mask.BaseColor = glm::vec3(0.8f);
        mask.AlbedoMap = slats;
        mask.AlphaMask = true;
        mask.AlphaCutoff = 0.5f;
        fixture.MaskMaterial = scene.AddMaterial(mask);

        AddFloorQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.FloorMaterial);
        AddCeilingQuad(scene, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddBackWallQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.WhiteMaterial);
        AddLeftWallQuad(scene, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.RedMaterial);
        AddRightWallQuad(scene, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, fixture.GreenMaterial);

        AddCeilingQuad(scene, 0.98f, -0.3f, 0.3f, -0.3f, 0.3f, fixture.LightMaterial);
        // The slatted mask, hung under the light: its shadow on the floor is the
        // alpha test, the one thing an opaque trace cannot reproduce.
        AddCeilingQuad(scene, 0.55f, -0.4f, 0.4f, -0.4f, 0.4f, fixture.MaskMaterial);

        AddBox(scene, glm::vec3(-0.55f, -1.0f, -0.55f), glm::vec3(-0.05f, -0.2f, -0.05f), fixture.BoxMaterial);

        scene.Build();
        return fixture;
    }

    // The plain box (ClosureV2) plus a small warm sphere light hanging in the
    // open right half: the emitter is visible to the camera, lights the floor
    // and the green wall with a soft shadow off the block, and shares the
    // frame with the ceiling quad, so NEE's two strategies and both emitter
    // kinds coexist in one MIS.
    inline CornellBoxScene MakeSphereLightCornellBoxScene(f32 emissiveRadiance = 6.0f)
    {
        CornellBoxScene fixture = MakeCornellBoxScene(emissiveRadiance, PBRModel::ClosureV2);
        // The scene was built by MakeCornellBoxScene; AddLight then Build again.
        ReferenceLight sphere;
        sphere.Type = ReferenceLightType::SphereArea;
        sphere.Position = glm::vec3(0.45f, 0.1f, 0.35f);
        sphere.Radius = 0.12f;
        sphere.Color = glm::vec3(1.0f, 0.75f, 0.5f);
        sphere.Intensity = 6.0f;
        sphere.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 10.0f);
        fixture.Scene.AddLight(sphere);
        fixture.Scene.Build();
        return fixture;
    }
} // namespace OloEngine::Tests::PathTracingFixtures
