// WaterTessEvalStage.glsl — shared stage body for Water.glsl and Water_Depth.glsl.
// The surface-depth capture replays the SAME displacement chain as the
// color pass (barycentric interp + Gerstner/FFT displacement); any drift between the two
// puts the underwater fog's captured surface at a different height than
// the drawn one. Included by both after their own `#type`/`#version`
// lines; sibling includes resolve inside include/.

layout(triangles, equal_spacing, ccw) in;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
    // Reconstruction flavour of u_Projection (#691) — every stage's
    // declaration must match or glLinkProgram rejects the program; only the
    // fragment stage reads it. Identical to u_Projection on GL.
    mat4 u_ProjectionForReconstruction;
};

#include "InstanceBlock_Single.glsl"

layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;
    vec4 u_WaterDeepColor;
    vec4 u_VisualParams;
    vec4 u_NormalMapScroll;
    vec4 u_NormalMapSpeed;
    vec4 u_LightDirection;
    vec4 u_ScreenParams;
    vec4 u_DepthRefractionParams;
    vec4 u_RefractionColor;
    vec4 u_FoamParams;
    vec4 u_FoamParams2;
    vec4 u_SSSColor;
    vec4 u_SSRParams;
    vec4 u_TessParams;
    vec4 u_FFTParams;              // x = cascade count (0 = Gerstner), y = 1/L0, z = heightScale, w = horizontalScale
    // FFT ocean cascades (issue #969). C++ twin:
    // UBOStructures::WaterUBO::FFTCascadeParams, packed by
    // Ocean::PackCascadeShaderParams. Declared in EVERY stage of the water
    // programs, identically, for the reason the #967/#968 fields below are.
    //
    // u_FFTParams.x carries the CASCADE COUNT rather than a 0/1 flag — 1 is the
    // single-cascade fallback and 3 the band-limited preset, so every existing
    // `u_FFTParams.x > 0.5` test still means "FFT is on" and did not change.
    vec4 u_FFTCascadeParams;       // x = 1/L1 (mid tile), y = 1/L2 (fine tile), z = cos(theta_mid), w = sin(theta_mid)
    // Boat / actor wake foam field (issue #967). C++ twin:
    // UBOStructures::WaterUBO::WakeFieldParams / WakeFieldParams2. Declared in
    // EVERY stage of the water programs, identically, because GL requires a
    // uniform block shared across a program's stages to be declared the same
    // way in each — appending to only the stage that reads it is a link error,
    // not a silent mismatch. Only the fragment stage actually reads them.
    vec4 u_WakeFieldParams;        // xy = field window centre (world XZ), z = 1/fieldExtent, w = intensity (<=0 disables)
    vec4 u_WakeFieldParams2;       // x = wake fade start (m), y = wake fade end (m), z = edge-fade start, w = unused

    // Boat / actor wake SHAPE (issue #968). C++ twin:
    // UBOStructures::WaterUBO::WakeShapeParams / WakeHulls; GLSL evaluator:
    // include/WaterWakeCommon.glsl. Declared in EVERY stage of the water
    // programs, identically, for the same reason the #967 fields above are: GL
    // requires a uniform block shared across a program's stages to be declared
    // the same way in each, so appending to only the stages that read it is a
    // LINK error rather than a silent mismatch. Read by the vertex and
    // tess-eval stages, which is where the surface is displaced.
    vec4 u_WakeShapeParams;        // x = live hull count, y = height scale (<=0 disables), z = hull flatten strength, w = reserved
    // Shore wave deformation (issue #1033). C++ twin:
    // UBOStructures::WaterUBO::ShoreParams / ShoreParams2; the encoding contract
    // and every relation driven by it live in Renderer/Water/WaterShoreDepth.h,
    // the GLSL evaluator in include/WaterShoreCommon.glsl. Declared in EVERY
    // stage of the water programs, identically, for the same reason the #967 /
    // #968 fields above are: GL requires a uniform block shared across a
    // program's stages to be declared the same way in each, so appending to only
    // the stages that read it is a LINK error rather than a silent mismatch.
    // Read by the vertex and tess-eval stages (which displace the surface) and
    // by the fragment stage (which fades the breaker foam).
    //
    // xy = seabed depth-field window centre (world XZ),
    // z  = 1 / window extent in metres (the UV scale),
    // w  = enable. w <= 0 IS the disabled state; there is no separate flag, so a
    //      frame whose bake did not run cannot leave a stale field showing.
    vec4 u_ShoreParams;
    // x = breaker index (the a/h limit the surf zone breaks at; 0.39 is the
    //     physical value — WaterShoreDepth.h :: kBreakerIndex),
    // y = breaking foam gain, z/w = reserved.
    vec4 u_ShoreParams2;
    // 80 = WaterWake::kHullVec4Count (4 hulls x 20 vec4). The layout is
    // WaterWake.h's, verbatim; WATER_WAKE_* in WaterWakeCommon.glsl mirrors the
    // offsets so nothing here indexes it by a bare literal.
    vec4 u_WakeHulls[80];
};

#include "WaterCommon.glsl"

// Boat / actor wake shape (issue #968). The evaluator is buffer-agnostic and
// asks for its records through this hook; here they come from the WaterParams
// block above. Defining it right after the block, rather than letting each call
// site index u_WakeHulls itself, is what keeps ONE walk over the records shared
// with the parity probe (tests/ShaderUnit_WaterWake.glsl, which defines the same
// hook over an SSBO).
#include "WaterWakeCommon.glsl"
vec4 waterWakeFetch(int index)
{
    return u_WakeHulls[index];
}

// FFT ocean cascade textures (water-ocean.md §1).
#include "BindlessHeap.glsl"
// ARRAYS since issue #969: one layer per cascade band, so the three-band preset
// costs the same two engine texture slots the single-cascade field did. A scene
// that has not opted in gets a ONE-layer array holding the identical field.
#ifdef OLO_BINDLESS
#define u_FFTDisplacement OLO_HEAP_TEX_2D_ARRAY(50)  // rgb = (dx, h, dz), a = foam — TEX_WATER_FFT_DISPLACEMENT
#define u_FFTDerivatives OLO_HEAP_TEX_2D_ARRAY(51)  // rgb = normal, a = jacobian — TEX_WATER_FFT_DERIVATIVES
#else
layout(binding = 50) uniform sampler2DArray u_FFTDisplacement; // rgb = (dx, h, dz), a = foam
layout(binding = 51) uniform sampler2DArray u_FFTDerivatives;  // rgb = normal, a = jacobian
#endif

// The cascade sum, shared with the tess-eval and fragment stages and mirrored
// on the CPU by OceanFFTField::SampleCascades. The fetch hooks are what let one
// walk over the bands serve every stage's own sampler declaration.
#include "OceanCascadeCommon.glsl"
vec4 oceanCascadeFetchDisplacement(vec2 uv, int layer)
{
    return textureLod(u_FFTDisplacement, vec3(uv, float(layer)), 0.0);
}
vec4 oceanCascadeFetchDerivatives(vec2 uv, int layer)
{
    return textureLod(u_FFTDerivatives, vec3(uv, float(layer)), 0.0);
}

// Seabed depth field (issue #1033) — TEX_WATER_SHORE_DEPTH.
// r = water depth in metres, gb = d(depth)/d(worldXZ). Baked by
// WaterShoreDepthSystem from the scene's terrain; the encoding contract is
// Renderer/Water/WaterShoreDepth.h. This is the hook WaterShoreCommon.glsl
// forward-declares, so the shore math needs no sampler of its own and both
// displacing stages share one walk over it.
#ifdef OLO_BINDLESS
#define u_ShoreDepth OLO_HEAP_TEX_2D(71)
#else
layout(binding = 71) uniform sampler2D u_ShoreDepth;
#endif
vec4 waterShoreFetchDepth(vec2 uv)
{
    return textureLod(u_ShoreDepth, uv, 0.0);
}

layout(location = 0) in vec3 tc_WorldPos[];
layout(location = 1) in vec3 tc_Normal[];
layout(location = 2) in vec2 tc_TexCoord[];
layout(location = 3) in vec3 tc_PrevWorldPos[];

layout(location = 0) out vec3 v_WorldPos;
#ifdef OLO_WATER_DEPTH_ONLY
// Water_Depth's fragment stage reads ONLY v_WorldPos, and SPIR-V compilation
// dead-strips unread FS inputs — so a real output here would be a
// written-but-unconsumed interface warning per depth pipeline. Route the
// rest into plain locals the compiler removes; the displacement math above
// stays byte-identical to the color pass.
vec3 v_Normal;
vec2 v_TexCoord;
vec3 v_ViewDir;
vec3 v_Tangent;
vec3 v_Bitangent;
float v_WaveHeight;
vec3 v_PrevWorldPos;
float v_ShoreFoam;
#else
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
layout(location = 6) out float v_WaveHeight;
layout(location = 7) out vec3 v_PrevWorldPos;
// Breaking-wave foam from the shore transform (issue #1033). One float rather
// than the pair the displacing stage computes, because the fragment stage needs
// only "has this wave broken", and the two halves of that answer are combined
// where they are produced:
//
//   * the depth-limited breaker clamp — the wave could not stand up in this
//     much water, so the excess was taken off the surface. Exactly 0 outside
//     the surf zone, by construction;
//   * an actual FOLD of the horizontal displacement map (Jacobian < 0), which
//     is the same criterion the FFT ocean's foam alpha uses. Never true on a
//     sea that is not folding, so this cannot add foam to open water.
layout(location = 8) out float v_ShoreFoam;
#endif

void main()
{
    // Barycentric interpolation
    vec3 pos = gl_TessCoord.x * tc_WorldPos[0]
             + gl_TessCoord.y * tc_WorldPos[1]
             + gl_TessCoord.z * tc_WorldPos[2];
    vec3 posPrev = gl_TessCoord.x * tc_PrevWorldPos[0]
                 + gl_TessCoord.y * tc_PrevWorldPos[1]
                 + gl_TessCoord.z * tc_PrevWorldPos[2];
    vec2 uv = gl_TessCoord.x * tc_TexCoord[0]
            + gl_TessCoord.y * tc_TexCoord[1]
            + gl_TessCoord.z * tc_TexCoord[2];

    // Camera-relative (issue #429): pos is render-relative; add the origin back
    // for the world-anchored wave phase / FFT sampling. Displaced position stays
    // relative (Gerstner sums shifted back by origin).
    vec3 posAbs = pos + u_RenderOrigin;
    vec3 posPrevAbs = posPrev + u_RenderOrigin;

    // Apply wave displacement (FFT ocean or analytic Gerstner)
    float time = u_WaveParams.x * u_WaveParams.y;
    float prevTime = u_NormalMapSpeed.z * u_WaveParams.y;
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    // Seabed under this tessellated vertex (issue #1033). Same one-sample rule
    // as the vertex stage, and the same absolute-world-XZ addressing — the two
    // stages displace the same surface and must not disagree about where it is.
    WaterShoreSample shore = waterShoreSample(posAbs.xz, u_ShoreParams);
    float shoreBreak = 0.0;
    float shoreJacobian = 1.0;

    vec3 displacedNormal;
    vec3 displacedPos;
    if (u_FFTParams.x > 0.5)
    {
        // Band-limited cascade sum (issue #969), through the SAME shared
        // function the vertex stage uses — the two stages displace the same
        // surface and must not each own a copy of how.
        OceanCascadeSample fft = sampleOceanCascades(posAbs.xz, u_FFTParams, u_FFTCascadeParams);
        vec3 fftDisp = vec3(fft.Displacement.x * u_FFTParams.w,
                            fft.Displacement.y * u_FFTParams.z,
                            fft.Displacement.z * u_FFTParams.w);

        // Shore depth limit for the FFT path (issue #1033). A tiled spectrum's
        // individual wave trains cannot be refracted the way the analytic
        // octaves are — every train shares one field, so there is no per-train
        // heading to turn. What the FFT path CAN honour is the depth limit, and
        // it is the half that shows: a crest standing a metre tall in 20 cm of
        // water is the artefact that reads as "a shader" at a beach. Scale the
        // whole displacement vector (not just its height) so the choppy
        // horizontal shift is suppressed with it, and report the removed excess
        // as breaking, exactly as the Gerstner path does.
        if (shore.Enabled > 0.0)
        {
            float aLimit = max(u_ShoreParams2.x, 1e-4) * shore.Depth;
            float crest = abs(fftDisp.y);
            if (crest > aLimit)
            {
                float k = aLimit / max(crest, 1e-6);
                fftDisp *= k;
                shoreBreak = clamp(1.0 - k, 0.0, 1.0);
            }
        }
        displacedPos = pos + fftDisp;
        // Zero/NaN-safe — see the vertex-stage FFT branch.
        vec3 fftNormal = oceanCascadeNormal(fft);
        displacedNormal = (dot(fftNormal, fftNormal) > 1e-12) ? normalize(fftNormal) : vec3(0.0, 1.0, 0.0);
        v_PrevWorldPos = displacedPos; // FFT field has no prev-frame copy
    }
    else
    {
        displacedPos = sumGerstnerWavesShore(
            posAbs, time,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            u_FoamParams2.w, // mesh vertex spacing — band-limits the octave ladder (#943)
            shore, u_ShoreParams2.x, // seabed depth + the breaker index (#1033)
            displacedNormal, shoreJacobian, shoreBreak
        ) - u_RenderOrigin; // world-anchored phase, relative result (issue #429)

        // Prev-frame displacement for velocity reprojection
        vec3 _prevNormalUnused;
        float _prevJacobianUnused;
        float _prevBreakUnused;
        vec3 displacedPosPrev = sumGerstnerWavesShore(
            posPrevAbs, prevTime,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            u_FoamParams2.w, // mesh vertex spacing — band-limits the octave ladder (#943)
            shore, u_ShoreParams2.x, // the SAME seabed sample — it does not move (#1033)
            _prevNormalUnused, _prevJacobianUnused, _prevBreakUnused
        ) - u_RenderOrigin;
        v_PrevWorldPos = displacedPosPrev;
    }


    // --- Boat / actor wake shape (issue #968) --------------------------------
    // Applied AFTER the ocean displacement (FFT or Gerstner) and to BOTH, since
    // it is a property of the boat rather than of the wave model. Three steps,
    // in this order:
    //
    //   1. suppress the ocean displacement inside the oriented hull footprint,
    //      which is what stops a crest rising through the deck. Scaling the
    //      whole displacement VECTOR rather than just its y also stops the
    //      choppy horizontal shift dragging the surface sideways under the
    //      hull, which reads as the boat sliding on the water;
    //   2. flatten the ocean normal to match, or the water shades as though the
    //      crest that was just removed is still there;
    //   3. add the wake height, and perturb the normal by ITS gradient.
    //
    // Step 3's normal is not optional: a displacement whose normal does not
    // carry the same factor is docs/agent-rules/water-shading-nyquist.md's
    // first rule, and it renders as flat water with an invisible bulge in it.
    //
    // Guarded on the height scale so a scene with the feature off pays one
    // compare rather than five bounding-circle rejections per vertex.
    if (u_WakeShapeParams.y > 0.0)
    {
        // u_FoamParams2.w is the BASE grid's vertex spacing (#943). This stage
        // runs on a surface the tessellator has already subdivided, so the
        // spacing that actually limits what the mesh can carry is that divided
        // by this patch's tessellation level.
        //
        // Using the base spacing here is not conservative, it is wrong in a way
        // that deletes the feature: Drift's ocean is 1600 m over a 640 grid, so
        // 2.5 m base spacing — against which a 1 m arm ridge band-limits to
        // exactly zero, at every distance, however finely the patch under the
        // boat is actually tessellated. The wake would simply not have arms,
        // with no error and nothing failing.
        float wakeTessLevel = max(gl_TessLevelInner[0], 1.0);
        float wakeSpacing = u_FoamParams2.w / wakeTessLevel;
        // Evaluated at the DISPLACED vertex's absolute world XZ, not the
        // undisplaced one. Gerstner (and the FFT's choppiness) shift a vertex
        // horizontally by up to the wave amplitude, and the CPU side reads the
        // column ABOVE a world XZ — WaterSurface::SampleHeight inverts that
        // shift specifically so it can. Evaluating here at the base position
        // would put the rendered ridge up to a metre from where physics thinks
        // it is, on a choppy sea only, with the parity test still green because
        // that test feeds both evaluators the same point. This is the one place
        // the two paths could agree on the FUNCTION and disagree about WHERE.
        vec2 wakeXZ = displacedPos.xz + u_RenderOrigin.xz;
        vec2 wake = waterWakeEvaluate(u_WakeShapeParams.x, u_WakeShapeParams.y, u_WakeShapeParams.z,
                                      wakeXZ, wakeSpacing);
        if (wake.x != 0.0 || wake.y > 0.0)
        {
            displacedPos = pos + (displacedPos - pos) * (1.0 - wake.y);
            displacedNormal = normalize(mix(displacedNormal, vec3(0.0, 1.0, 0.0), wake.y));

            displacedPos.y += wake.x;
            // The finite-difference step: a quarter metre, or half the mesh
            // spacing when that is coarser. Not smaller — at the absolute world
            // coordinates this is evaluated at, a step near f32's ulp out there
            // differences to zero and the wake shades flat.
            float wakeEps = max(0.25, wakeSpacing * 0.5);
            displacedNormal = waterWakePerturbNormal(displacedNormal, u_WakeShapeParams.x,
                                                     u_WakeShapeParams.y, wakeXZ,
                                                     wakeSpacing, wakeEps);
        }
        // v_PrevWorldPos is deliberately left un-waked: the records carry no
        // previous-frame copy, exactly like the FFT displacement field above,
        // so the motion vector misses the wake's own motion rather than
        // reporting a wrong one.
    }

    float maxAmplitude = max(amplitude, 0.001);
    v_WaveHeight = (u_FFTParams.x > 0.5) ? (displacedPos.y - pos.y)
                                         : (displacedPos.y - pos.y) / maxAmplitude;
    // Issue #1033 — see the varying's declaration for what the two terms are.
    v_ShoreFoam = max(shoreBreak, clamp(-shoreJacobian * 2.0, 0.0, 1.0));

    v_WorldPos = displacedPos;
    v_Normal = displacedNormal;
    v_TexCoord = uv;
    v_ViewDir = normalize(u_CameraPosition - displacedPos);

    // Compute tangent frame
    vec3 N = normalize(displacedNormal);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    v_Tangent = normalize(cross(up, N));
    v_Bitangent = cross(N, v_Tangent);

    gl_Position = u_ViewProjection * vec4(displacedPos, 1.0);
}
