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
// The shared camera block (include/CameraCommon.glsl), identical in every
// stage of every program that includes this — GL links a program only if
// its stages agree on the block — and carrying the forward screen-space AO
// lane (issue #1452).
#include "CameraCommon.glsl"

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
    // Projected grid (issue #1035, water-ocean.md §4.1). C++ twin:
    // UBOStructures::WaterUBO::ProjectedGridParams / ProjectedGridParams2; the
    // contract and the census that chose this scheme are
    // Renderer/Water/WaterSurfaceLod.h, the evaluator is
    // waterProjectGridVertex() in include/WaterVertexStage.glsl. Declared in
    // EVERY stage of the water programs, identically, for the same reason every
    // block above is: GL requires a uniform block shared across a program's
    // stages to be declared the same way in each, so appending to only the
    // stages that read it is a LINK error rather than a silent mismatch. Read
    // by the vertex stage (which places the grid) and the tess-control stage
    // (whose subdivision rule changes with it).
    //
    // x = enable; x <= 0 IS the disabled state, so a build with no projected
    //     water pays one compare per vertex,
    // y = the NDC x minimum of the rectangle the grid is laid out over,
    // z = its NEAR y edge — near by geometry, not by sign: the bottom of the
    //     screen is y = -1 on GL and +1 under the Vulkan seam's row flip. The
    //     rectangle stops short of the sky and extends PAST the near edge by
    //     however far a crest can move a vertex there,
    // w = band-limit spacing per metre of ray distance: one grid step of view
    //     angle, so a vertex t metres out is sampled ~w*t metres apart. The
    //     rim radius a missed ray is pushed to is derived in-shader from the
    //     half-extents below and u_Model, not uploaded.
    vec4 u_ProjectedGridParams;
    // xy = the surface's LOCAL half-extents. The clamp into this rect is what
    //      keeps a finite water tile finite: a screen-space grid has no idea
    //      where the water ends.
    // z = the NDC x maximum, w = the FAR y edge (partner of .z above).
    vec4 u_ProjectedGridParams2;
};

// No wave, FFT, shore or wake includes and no samplers: this stage places the
// resting surface and the tess-eval stage displaces it (issue #1470, see the
// end of main()). Water.glsl's and Water_Depth.glsl's TES carry all of that.

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
// Previous-frame world position (wave + model reprojection) for RT3 velocity.
layout(location = 7) out vec3 v_PrevWorldPos;
// The band-limit spacing this vertex is sampled at (issue #1035). The world
// grid's spacing is one number for the whole surface (u_FoamParams2.w); a
// projected grid's varies by three orders of magnitude across one frame, so it
// is derived per vertex here and carried to the tess-eval stage, which
// interpolates it. PER VERTEX is the load-bearing part: deriving it per PATCH
// in the tess-eval stage hands a shared vertex two different octave weights
// from its two patches, and the surface tears along every patch edge.
layout(location = 9) out float v_ProjSpacing;

// =============================================================================
// Projected grid (issue #1035, water-ocean.md §4.1; Johanson 2004)
// =============================================================================
//
// The surface mesh's (u, v) is read as a SCREEN coordinate instead of a world
// one: cast this pixel's ray from the eye, intersect it with the water plane,
// and the vertex lands wherever this pixel column meets the water. Vertex
// density is then a property of the viewport, not of
// m_WorldSizeX/Z — which is the point, because the census in
// WaterGeometryLodProfileTest measured 80-94% of a world-space grid's triangles
// landing sub-pixel at a grazing angle while every base patch still paid its
// vertex invocation.
//
// The ray is built HERE from u_Projection and u_View rather than uploaded as
// an inverse matrix. That is deliberate: these are the render-RELATIVE,
// backend-ADJUSTED matrices this stage feeds gl_Position, so a ray derived from
// them cannot be in the wrong space or the wrong handedness. Uploading a
// precomputed inverse means picking which flavour to invert on the CPU and
// being silently wrong on one backend, or at 40 km from the origin, if the
// choice drifts. The CPU side (WaterSurfaceLod::ComputeNdcBounds) works in
// absolute space through an inverse view-projection — NDC is identical in
// both, which is what lets the two agree on the rectangle.
//
// The CPU mirror is WaterSurfaceLod::ProjectGridVertex; the two are pinned
// against each other by WaterGeometryLodProfileTest.
vec3 waterProjectGridVertex(vec2 ndc, vec3 planePoint, vec3 planeNormal, float rimRadius,
                            float spacingPerMetre, out float outSpacing)
{
    // The view ray for this NDC, built from the camera BASIS rather than by
    // inverting the view-projection.
    //
    // In view space a pixel's ray is (x / P00, y / P11, -1): the projection's
    // two focal terms undo the perspective scale and the camera looks down its
    // own -z. Taking that into the world is the INVERSE of the view's 3x3 —
    // not its transpose. The editor camera is rigid and the two agree, but a
    // runtime camera is an entity transform, scale included, and then the
    // view's 3x3 is S^-1 R^T whose inverse R S is not its transpose R S^-1.
    // A 3x3 inverse is ~30 multiplies per vertex; an `inverse(u_ViewProjection)`
    // route was written first, gives the same ray, and costs a mat4 inverse.
    //
    // Convention-safe by construction: on Vulkan the projection seam negates
    // P11 (RHI/RHIProjectionSeam.h) and the NDC y handed in is flipped the same
    // way, so y / P11 is the same view-space direction on both backends. The
    // seam's z remap touches P22/P32 only, which this never reads.
    const float EPS = 1e-6;
    vec3 dirView = vec3(ndc.x / u_Projection[0][0], ndc.y / u_Projection[1][1], -1.0);
    vec3 rayDir = normalize(inverse(mat3(u_View)) * dirView);
    vec3 rayOrigin = u_CameraPosition;

    float denom = dot(rayDir, planeNormal);
    float t = (abs(denom) >= EPS) ? (dot(planePoint - rayOrigin, planeNormal) / denom) : -1.0;
    // t is metres along a unit ray that already points away from the eye, so
    // "in front" is simply t > 0. No upper bound: a hit near the horizon is
    // legitimately far away and the caller's rect clamp is what bounds it.
    if (t > 0.0) // t is -1 whenever denom was too small, so this is the whole test
    {
        // World metres between this vertex and its grid neighbour: one grid
        // step of view angle (spacingPerMetre, from the CPU), scaled by the
        // distance, stretched by the incidence angle along the ray's slope.
        // Continuous in t, which is what a shared vertex needs.
        outSpacing = spacingPerMetre * t / max(abs(denom), 0.05);
        return rayOrigin + rayDir * t;
    }
    // A missed row lands on the rim, which is as far as the surface goes.
    outSpacing = spacingPerMetre * rimRadius;

    // Missed: above the horizon, or parallel to the surface. Slide out along
    // the ray's horizontal component to the rim. The ray already points forward
    // so this lands in front of the camera, and the caller's rect clamp then
    // puts it on the surface edge as a zero-area row.
    vec3 camOnPlane = rayOrigin - planeNormal * dot(rayOrigin - planePoint, planeNormal);
    vec3 horizontal = rayDir - planeNormal * dot(rayDir, planeNormal);
    float lenSq = dot(horizontal, horizontal);
    if (lenSq < EPS)
        return camOnPlane;
    return camOnPlane + horizontal * inversesqrt(lenSq) * rimRadius;
}

// ---- the layout rectangle's (u, v) -> NDC map (issue #1217) -----------------
//
// The rectangle reaches PAST the screen so a crest can lift near water into
// frame, and at a low eye that skirt is most of it: mapped uniformly, 61% of
// WaterShowcase's rows landed below the screen at a 3 m grazing pose and only
// 25% of its vertices ever reached the raster. The skirt rows only have to
// EXIST — what the viewer sees of them is the displaced surface, not the
// resting lattice — so each END of each axis is capped at
// WATER_PROJGRID_SKIRT_SHARE of that axis' rows and the rest go to the part of
// the rectangle that is on screen, where the map stays exactly uniform.
//
// Mirror of WaterSurfaceLod::MapProjectedGridUV; the contract is in
// Renderer/Water/WaterSurfaceLod.h and the CPU side is pinned by
// WaterGeometryLodProfileTest.
#define WATER_PROJGRID_SKIRT_SHARE 0.1 // == WaterSurfaceLod::kSkirtParamShare

// A skirt advances as `a*w + (1-a)*w^2` in its own parameter (0 at the screen
// edge, 1 at the outer edge). Monotone for any a in [0, 1], and a is picked so
// the slope MATCHES the on-screen one at w = 0: the mesh band-limit reads this
// step, and a step that jumps between two adjacent rows changes the octave
// ladder across one edge of the mesh.
float waterProjGridSkirtCurve(float a, float w)
{
    return a * w + (1.0 - a) * w * w;
}

// ...and the same curve's slope at w, over its slope at the join — i.e. this
// row's NDC step over the on-screen one, which is what the band-limit spacing
// has to be widened by out here.
float waterProjGridSkirtStepRatio(float a, float w)
{
    float sa = max(a, 1e-4);
    return (sa + 2.0 * (1.0 - sa) * w) / sa;
}

float waterProjGridAxis(float s, float lo, float hi, out float stepRatio)
{
    stepRatio = 1.0;
    s = clamp(s, 0.0, 1.0);
    float total = hi - lo;
    float screenLo = max(lo, -1.0);
    float screenHi = min(hi, 1.0);
    float window = screenHi - screenLo;
    // Nothing of this axis is on screen (or the rectangle collapsed): there is
    // no window to move the rows into, so the uniform map stands.
    if (!(total > 1e-6) || !(window > 1e-4))
        return mix(lo, hi, s);

    float low = screenLo - lo;
    float high = hi - screenHi;
    float paramLow = (low > 0.0) ? min(WATER_PROJGRID_SKIRT_SHARE, low / total) : 0.0;
    float paramHigh = (high > 0.0) ? min(WATER_PROJGRID_SKIRT_SHARE, high / total) : 0.0;
    float paramWindow = 1.0 - paramLow - paramHigh;
    if (!(paramWindow > 1e-4))
        return mix(lo, hi, s);

    float screenSlope = window / paramWindow;
    if (s < paramLow)
    {
        // Outward from the screen edge: w = 0 at the join, 1 at `lo`.
        float w = 1.0 - s / paramLow;
        // Exactly 1 when the skirt already took no more than its share, which
        // is what leaves an overhead pose bit-for-bit on the uniform map.
        float a = clamp(screenSlope * paramLow / low, 0.0, 1.0);
        stepRatio = waterProjGridSkirtStepRatio(a, w);
        return screenLo - low * waterProjGridSkirtCurve(a, w);
    }
    float paramWindowEnd = paramLow + paramWindow;
    if (s > paramWindowEnd)
    {
        float w = (s - paramWindowEnd) / max(1.0 - paramWindowEnd, 1e-6);
        float a = clamp(screenSlope * paramHigh / high, 0.0, 1.0);
        stepRatio = waterProjGridSkirtStepRatio(a, w);
        return screenHi + high * waterProjGridSkirtCurve(a, w);
    }
    return screenLo + (s - paramLow) * screenSlope;
}

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    // Projected grid (issue #1035): the SAME lattice, placed by screen
    // coordinate instead of by world coordinate. a_Position is ignored and
    // a_TexCoord — which CreateWaterGrid already writes as (fx, fz) over the
    // whole surface — becomes the screen parameter.
    vec3 gridLocalPos = a_Position;
    vec2 gridUV = a_TexCoord;
    v_ProjSpacing = u_FoamParams2.w; // the world grid's one-number spacing
    if (u_ProjectedGridParams.x > 0.5)
    {
        // The plane this surface's transform describes, in the same
        // render-relative space u_ViewProjection works in. Column 1 is the
        // transformed up axis and column 3 the surface centre, which is how the
        // planar-reflection plane is derived on the C++ side too.
        vec3 planeNormal = u_Model[1].xyz;
        float normalLenSq = dot(planeNormal, planeNormal);
        planeNormal = (normalLenSq > 1e-12) ? (planeNormal * inversesqrt(normalLenSq)) : vec3(0.0, 1.0, 0.0);
        vec3 planePoint = u_Model[3].xyz;

        // The grid spans the rectangle WaterSurfaceLod::ComputeNdcBounds
        // measured, NOT the screen. It is SMALLER than the screen at the
        // horizon end — rows up there reach no plane and would collapse onto
        // the horizon line — and LARGER at the near edge, because this stage
        // places the grid on the RESTING plane and the tess-eval stage then
        // displaces it. A vertex placed exactly at the bottom of the frame and
        // then lifted by a crest leaves the frame, taking a band of the nearest
        // water with it: from a 3 m eye a 1.5 m crest moves it up by a quarter
        // of the vertical field of view.
        //
        // v = 1 lands on the NEAR edge and v = 0 on the far one, and that
        // orientation is load-bearing. CreateWaterGrid winds its triangles
        // counter-clockwise from above for a (u -> +x, v -> +z) frame, and +z
        // is TOWARD a camera looking down -z. Screen-up is AWAY from the
        // camera, so mapping v straight onto NDC y hands the same index order a
        // frame of the opposite handedness: every triangle comes out
        // back-facing from above, and the fragment stage's waterline rule
        // (keep the face the camera is on) then discards the entire surface.
        // That is not a subtle artefact — it is "the water is not there", with
        // only a few folded rows at the rim surviving, and every intersection
        // formulation produces it identically.
        //
        // WHICH NDC y is near is not assumed here. It is y = -1 on GL and +1
        // under the Vulkan seam's row flip, and a flip hard-coded for one of
        // them reproduces the same missing surface on the other — which is
        // exactly how this was first written. The CPU decides by geometry and
        // uploads near in .z and far in .w.
        // Issue #1217: the map is uniform over the part of the rectangle that
        // is ON SCREEN — unchanged, and the property the whole design rests on
        // — and compressed over each end that is not. The warp is a statement
        // about the RECTANGLE, not about the parameter, so the y axis is always
        // walked min -> max and v is flipped instead when the near edge is the
        // minimum.
        float nearEdgeY = u_ProjectedGridParams.z;
        float farEdgeY = u_ProjectedGridParams2.w;
        float sy = (nearEdgeY > farEdgeY) ? gridUV.y : (1.0 - gridUV.y);
        float stepRatioX = 1.0;
        float stepRatioY = 1.0;
        vec2 ndc;
        ndc.x = waterProjGridAxis(gridUV.x, u_ProjectedGridParams.y, u_ProjectedGridParams2.z,
                                  stepRatioX);
        ndc.y = waterProjGridAxis(sy, min(nearEdgeY, farEdgeY), max(nearEdgeY, farEdgeY),
                                  stepRatioY);
        // u_ProjectedGridParams.w describes the ON-SCREEN step; a compressed
        // skirt row's neighbours are further apart than that, and sampling it
        // as if they were not is what would alias.
        float localSpacingPerMetre = u_ProjectedGridParams.w * max(stepRatioX, stepRatioY);

        // How far a missed ray is pushed before the rect clamp catches it: past
        // the surface's world-space half-diagonal, doubled. Derived here rather
        // than uploaded so the .w slot can carry the spacing step instead.
        float rimRadius = 2.0 * length(vec2(u_ProjectedGridParams2.x * length(u_Model[0].xyz),
                                            u_ProjectedGridParams2.y * length(u_Model[2].xyz)));
        float projSpacing = 0.0;
        vec3 hit = waterProjectGridVertex(ndc, planePoint, planeNormal, rimRadius,
                                          localSpacingPerMetre, projSpacing);
        v_ProjSpacing = projSpacing;

        // Back into surface-local space and clamp into the authored rect. This
        // is what keeps a finite tile finite; rows that would land past the rect
        // pile onto its edge as zero-area triangles.
        // Back into surface-local space WITHOUT a per-vertex mat4 inverse:
        // u_NormalMatrix is transpose(inverse(u_Model)), already uploaded per
        // draw, so its 3x3 transpose is the inverse of the model's linear part
        // and the translation is undone by subtracting column 3 first.
        vec3 local = transpose(mat3(u_NormalMatrix)) * (hit - u_Model[3].xyz);
        local.xz = clamp(local.xz, -u_ProjectedGridParams2.xy, u_ProjectedGridParams2.xy);
        local.y = 0.0;
        gridLocalPos = local;
        // Keep the UV a surface-local parameterisation rather than the screen
        // one, so anything downstream that reads it still means "where on the
        // water", the same as the world-space grid.
        gridUV = local.xz / max(u_ProjectedGridParams2.xy, vec2(1e-3)) * 0.5 + 0.5;
    }

    vec4 worldPos = u_Model * vec4(gridLocalPos, 1.0);
    vec4 worldPosPrev = u_PrevModel * vec4(gridLocalPos, 1.0);
    // The vertex stage NEVER displaces (issue #1470). Every program that
    // includes this stage — Water.glsl and Water_Depth.glsl — also has a
    // tessellation-evaluation stage, and every water draw is a patch list
    // (CommandDispatch::DrawWater), so the TES always runs and always displaces.
    // "Tessellation off" only means a tess level of 1, not "no TES". This stage
    // used to displace too whenever the tess factor was 0 — the WaterComponent
    // DEFAULT — and the TES then displaced the displaced point a second time,
    // sampling the wave field at the already-shifted XZ: double the height,
    // double the choppy drift, a surface physics (WaterSurface::SampleHeight*,
    // one displacement) does not float anything on. On a 10 m tile under a
    // 180 m FFT swell that drift is the whole tile sliding metres sideways
    // between two wave phases, which is what issue #1470 was first reported as.
    //
    // The projected grid (issue #1035) needs this for its own reason too: its
    // band-limit spacing is per vertex (v_ProjSpacing above) and is applied in
    // the TES, interpolated across the patch.
    v_WorldPos = worldPos.xyz;
    v_PrevWorldPos = worldPosPrev.xyz; // the TES displaces the previous-frame position too
    v_Normal = vec3(0.0, 1.0, 0.0);
    v_TexCoord = gridUV;
    v_ViewDir = normalize(u_CameraPosition - worldPos.xyz);
    v_Tangent = vec3(1.0, 0.0, 0.0);
    v_Bitangent = vec3(0.0, 0.0, 1.0);
    gl_Position = vec4(worldPos.xyz, 1.0); // the TES transforms
}
