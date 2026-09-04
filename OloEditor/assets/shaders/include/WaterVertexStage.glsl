// WaterVertexStage.glsl — shared stage body for Water.glsl and Water_Depth.glsl.
// The surface-depth capture replays the SAME displacement chain as the
// color pass (Gerstner/FFT displacement, camera-relative origin); any drift between the two
// puts the underwater fog's captured surface at a different height than
// the drawn one. Included by both after their own `#type`/`#version`
// lines; sibling includes resolve inside include/.

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. Binding 57 is the
// engine-wide vertex-pull binding and the root struct carries this buffer's
// device address; the stream is the engine `Vertex` (32 B: vec3 position @0,
// vec3 normal @12, vec2 uv @24), so the per-vertex stride is 8 floats. This is
// the VERTEX stage only -- the tessellation stages consume this stage's
// varyings, not vertex attributes, so they need no branch (A10).
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Previous-frame VP for scene FB RT3 velocity. Wave displacement itself is
    // reprojected by re-evaluating sumGerstnerWaves() at `u_NormalMapSpeed.z`
    // (= prev animation time) in VS/TES so the motion vector captures on-
    // surface wave motion, not just camera and rigid motion.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
    // Reconstruction flavour of u_Projection (#691) — every stage's
    // declaration must match or glLinkProgram rejects the program; only the
    // fragment stage reads it. Identical to u_Projection on GL.
    mat4 u_ProjectionForReconstruction;
};

// Model UBO (binding 3) — the Single variant, like this shader's TES/FS:
// water is single-instance by design, and the varying-producing include
// would declare a v_InstanceIndex output no later stage consumes (a
// per-pipeline Vulkan validation interface warning).
#include "InstanceBlock_Single.glsl"

// Water UBO (binding 23)
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;             // x = Time, y = WaveSpeed, z = WaveAmplitude, w = WaveFrequency
    vec4 u_WaveDir0;               // xy = direction0, z = steepness0, w = wavelength0
    vec4 u_WaveDir1;               // xy = direction1, z = steepness1, w = wavelength1
    vec4 u_WaterColor;             // rgb = shallow color, a = Transparency
    vec4 u_WaterDeepColor;         // rgb = deep color,    a = Reflectivity
    vec4 u_VisualParams;           // x = FresnelPower, y = SpecularIntensity, z = NormalMapTiling, w = NoiseIntensity
    vec4 u_NormalMapScroll;        // xy = scroll0 offset, zw = scroll1 offset
    vec4 u_NormalMapSpeed;         // x = speed0, y = speed1, z = PrevTime, w = renderFromBelow
    vec4 u_LightDirection;         // xyz = directional light dir (normalized), w = unused
    vec4 u_ScreenParams;           // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DepthRefractionParams;  // x = depthSoftening, y = refrDistortion, z = refrHeightFactor, w = unused
    vec4 u_RefractionColor;        // rgb = tint color, w = unused
    vec4 u_FoamParams;             // x = heightStart, y = fadeDistance, z = tiling, w = brightness
    vec4 u_FoamParams2;            // x = angleExponent, y = shorelinePower, z = sssIntensity, w = mesh vertex spacing (#943)
    vec4 u_SSSColor;               // rgb = subsurface color, w = foamCoverage (#943)
    vec4 u_SSRParams;              // x = maxSteps (0=disabled), y = stepSize, z = maxDistance, w = thickness
    vec4 u_TessParams;             // x = tessellationFactor (0=disabled), y = minTessDist, z = maxTessDist, w = frustumCullEnable
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
    // Rain-impact ripples (issue #1034, §7.3). C++ twin:
    // UBOStructures::WaterUBO::RainRippleParams / RainRippleParams2; the
    // contract and every constant the field is built from live in
    // Renderer/Water/WaterRainRipples.h, the evaluator in
    // include/WaterRainCommon.glsl. Declared in EVERY stage of the water
    // programs, identically, for the same reason the #967 / #968 / #1033 fields
    // above are: GL requires a uniform block shared across a program's stages to
    // be declared the same way in each, so appending to only the stage that
    // reads it is a LINK error rather than a silent mismatch. Only Water.glsl's
    // fragment stage reads them — the ripples are normal-only and never displace.
    //
    // x = strength (artist gain x live precipitation intensity), y = density,
    // z = cell size (m), w = unused. x <= 0 IS the disabled state.
    vec4 u_RainRippleParams;
    // x = ripple fade start (m), y = fade end (m), z/w unused.
    vec4 u_RainRippleParams2;
    // Advected foam field (issue #1034, §2.2). C++ twin:
    // UBOStructures::WaterUBO::FoamFieldParams; the contract is
    // Renderer/Water/WaterFoam.h, the sampler include/WaterFoamCommon.glsl.
    // Declared in EVERY stage of the water programs, identically, for the same
    // reason every block above is. Only Water.glsl's fragment stage reads it.
    //
    // The .g channel of the SAME disturbance texture u_WakeFieldParams
    // describes — same window, same lattice, same edge fade
    // (u_WakeFieldParams2.z), different channel. It carries its own copy of the
    // window because the two gate independently: open-ocean whitecaps advect in
    // a scene with no boat in it, and u_WakeFieldParams is all-zero there.
    //
    // xy = window centre (world XZ), z = 1 / field extent, w = intensity.
    // w <= 0 IS the disabled state.
    vec4 u_FoamFieldParams;
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

// FFT ocean cascade textures (water-ocean.md §1). Sampled when
// u_FFTParams.x > 0.5 instead of summing Gerstner waves analytically.
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

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
#ifndef OLO_VULKAN
// Non-tess fallback only (VS feeds FS directly). Under Vulkan water always
// tessellates and the TES computes its own v_WaveHeight — declaring it here
// would be a written-but-unconsumed VS->TCS interface warning per pipeline.
layout(location = 6) out float v_WaveHeight;
#endif
// Previous-frame world position (wave + model reprojection) for RT3 velocity.
layout(location = 7) out vec3 v_PrevWorldPos;
#ifndef OLO_VULKAN
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
// Non-tess fallback only, exactly like v_WaveHeight above: under Vulkan water
// always tessellates and the TES emits its own.
layout(location = 8) out float v_ShoreFoam;
#endif

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    vec4 worldPosPrev = u_PrevModel * vec4(a_Position, 1.0);
    // Camera-relative (issue #429): u_Model is render-relative, so add the render
    // origin back for the world-anchored wave phase / FFT field sampling. The
    // displaced position stays relative (Gerstner sums are shifted back by the
    // origin) so gl_Position uses the relative view-projection.
    vec3 worldPosAbs = worldPos.xyz + u_RenderOrigin;
    vec3 worldPosPrevAbs = worldPosPrev.xyz + u_RenderOrigin;

    float time = u_WaveParams.x * u_WaveParams.y;                   // Time * WaveSpeed
    float prevTime = u_NormalMapSpeed.z * u_WaveParams.y;            // PrevTime * WaveSpeed
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    // When tessellation is active, vertex shader passes through
    // (displacement happens in TES instead)
    if (u_TessParams.x > 0.0)
    {
        v_WorldPos = worldPos.xyz;
        v_PrevWorldPos = worldPosPrev.xyz; // TES will re-displace
        v_Normal = vec3(0.0, 1.0, 0.0);
        v_TexCoord = a_TexCoord;
        v_ViewDir = normalize(u_CameraPosition - worldPos.xyz);
        v_Tangent = vec3(1.0, 0.0, 0.0);
        v_Bitangent = vec3(0.0, 0.0, 1.0);
#ifndef OLO_VULKAN
        v_WaveHeight = 0.0;
#endif
        gl_Position = vec4(worldPos.xyz, 1.0); // TES will transform
        return;
    }

    // Seabed under this vertex (issue #1033). Sampled ONCE and reused for the
    // previous-frame evaluation below: the sea floor does not move between
    // frames, and sampling it twice would only cost a second fetch.
    WaterShoreSample shore = waterShoreSample(worldPosAbs.xz, u_ShoreParams);
    float shoreBreak = 0.0;
    float shoreJacobian = 1.0;

    vec3 displacedNormal;
    vec3 displacedPos;
    if (u_FFTParams.x > 0.5)
    {
        // FFT ocean: sum the band-limited cascades (issue #969). One cascade is
        // the pre-#969 field, sampled at the same UV through the same maths.
        // Evaluated at the ABSOLUTE world position (issue #429), which is also
        // the space OceanFFTField::SampleCascades answers in.
        OceanCascadeSample fft = sampleOceanCascades(worldPosAbs.xz, u_FFTParams, u_FFTCascadeParams);
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
        displacedPos = worldPos.xyz + fftDisp;
        // Built from the SUMMED slope, not from an averaged normal — see
        // OceanCascadeCommon.glsl. Zero/NaN-safe: a zero or non-finite texel
        // would otherwise make normalize() emit NaN into the whole triangle's
        // interpolants.
        vec3 fftNormal = oceanCascadeNormal(fft);
        displacedNormal = (dot(fftNormal, fftNormal) > 1e-12) ? normalize(fftNormal) : vec3(0.0, 1.0, 0.0);
        v_PrevWorldPos = displacedPos; // FFT field has no prev-frame copy → no wave reprojection
    }
    else
    {
        // World-anchored phase (issue #429): evaluate at the absolute world
        // position, then shift the returned displaced position back to relative
        // space (sumGerstnerWaves returns position + displacement).
        displacedPos = sumGerstnerWavesShore(
            worldPosAbs, time,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            u_FoamParams2.w, // mesh vertex spacing — band-limits the octave ladder (#943)
            shore, u_ShoreParams2.x, // seabed depth + the breaker index (#1033)
            displacedNormal, shoreJacobian, shoreBreak
        ) - u_RenderOrigin;

        // Prev-frame displaced position — same Gerstner sum evaluated at prev time
        // through the prev model transform so the motion vector captures wave sway.
        vec3 _prevNormalUnused;
        float _prevJacobianUnused;
        float _prevBreakUnused;
        vec3 displacedPosPrev = sumGerstnerWavesShore(
            worldPosPrevAbs, prevTime,
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
        float wakeSpacing = u_FoamParams2.w;
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
            displacedPos = worldPos.xyz + (displacedPos - worldPos.xyz) * (1.0 - wake.y);
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

    // Normalized wave height for foam/SSS. Gerstner divides by amplitude to get a
    // ~[-1,1] range; FFT height is already in metres (its own amplitude), so pass
    // it through raw — foamHeightStart then reads as a metre threshold.
    float maxAmplitude = max(amplitude, 0.001);
#ifndef OLO_VULKAN
    v_WaveHeight = (u_FFTParams.x > 0.5) ? (displacedPos.y - worldPos.y)
                                         : (displacedPos.y - worldPos.y) / maxAmplitude;
    // Issue #1033 — see the varying's declaration for what the two terms are.
    v_ShoreFoam = max(shoreBreak, clamp(-shoreJacobian * 2.0, 0.0, 1.0));
#endif

    v_WorldPos = displacedPos;
    v_Normal = displacedNormal;
    v_TexCoord = a_TexCoord;
    v_ViewDir = normalize(u_CameraPosition - displacedPos);

    // Compute tangent frame from displaced normal for normal mapping
    vec3 N = normalize(displacedNormal);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    v_Tangent = normalize(cross(up, N));
    v_Bitangent = cross(N, v_Tangent);

    gl_Position = u_ViewProjection * vec4(displacedPos, 1.0);
}
