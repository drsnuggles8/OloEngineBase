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

#include <cmath>

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
    struct CornellBoxScene
    {
        ReferenceScene Scene;
        u32 WhiteMaterial = 0;
        u32 RedMaterial = 0;
        u32 GreenMaterial = 0;
        u32 LightMaterial = 0;

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
    inline CornellBoxScene MakeCornellBoxScene(f32 emissiveRadiance = 18.0f)
    {
        CornellBoxScene fixture;
        ReferenceScene& scene = fixture.Scene;

        ReferenceMaterial white;
        white.BaseColor = glm::vec3(0.73f);
        white.Roughness = 1.0f;
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
} // namespace OloEngine::Tests::PathTracingFixtures
