// WaterTessControlStage.glsl — shared stage body for Water.glsl and Water_Depth.glsl.
// The surface-depth capture replays the SAME displacement chain as the
// color pass (adaptive tess levels + displaced frustum cull); any drift between the two
// puts the underwater fog's captured surface at a different height than
// the drawn one. Included by both after their own `#type`/`#version`
// lines; sibling includes resolve inside include/.

layout(vertices = 3) out;

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
    vec4 u_FFTParams;
    // Boat / actor wake foam field (issue #967). C++ twin:
    // UBOStructures::WaterUBO::WakeFieldParams / WakeFieldParams2. Declared in
    // EVERY stage of the water programs, identically, because GL requires a
    // uniform block shared across a program's stages to be declared the same
    // way in each — appending to only the stage that reads it is a link error,
    // not a silent mismatch. Only the fragment stage actually reads them.
    vec4 u_WakeFieldParams;        // xy = field window centre (world XZ), z = 1/fieldExtent, w = intensity (<=0 disables)
    vec4 u_WakeFieldParams2;       // x = wake fade start (m), y = wake fade end (m), z = edge-fade start, w = unused
};

layout(location = 0) in vec3 v_WorldPos[];
layout(location = 1) in vec3 v_Normal[];
layout(location = 2) in vec2 v_TexCoord[];
layout(location = 3) in vec3 v_ViewDir[];
layout(location = 4) in vec3 v_Tangent[];
layout(location = 5) in vec3 v_Bitangent[];
#ifndef OLO_VULKAN
layout(location = 6) in float v_WaveHeight[]; // non-tess fallback interface only
#endif
layout(location = 7) in vec3 v_PrevWorldPos[];

layout(location = 0) out vec3 tc_WorldPos[];
layout(location = 1) out vec3 tc_Normal[];
layout(location = 2) out vec2 tc_TexCoord[];
layout(location = 3) out vec3 tc_PrevWorldPos[];

float calcTessLevel(vec3 p0, vec3 p1)
{
    // u_TessParams.x is the near-camera subdivision factor. When tessellation is
    // disabled the C++ side passes 0 here — but a DRAWN patch needs a tess level
    // of at least 1; a level below 1 (and especially 0) discards the patch
    // entirely (GL 4.6 §11.2.2). The old mix(maxFactor, 1.0, t) therefore handed
    // near patches (t≈0) a level of ~0 whenever tessellation was off, silently
    // culling all the water close to the camera and leaving a see-through hole to
    // the seafloor. Clamp the factor (and the result) to >= 1 so the base grid is
    // always rasterised; "disabled" just means no extra subdivision.
    float maxFactor = max(u_TessParams.x, 1.0);
    float minDist = u_TessParams.y;
    float maxDist = u_TessParams.z;

    vec3 mid = (p0 + p1) * 0.5;
    float dist = distance(u_CameraPosition, mid);

    // Linear falloff: close = maxFactor, far = 1.0
    float t = clamp((dist - minDist) / max(maxDist - minDist, 0.001), 0.0, 1.0);
    return max(mix(maxFactor, 1.0, t), 1.0);
}

// Conservative upper bound on per-axis Gerstner displacement for the current
// wave parameters. Used as a culling margin: a patch's corners are inflated by
// this distance before the frustum test so waves at the edges of off-screen
// patches don't pop into view. Derived from the wave bookkeeping in
// `sumGerstnerWaves` (WaterCommon.glsl): per wave the displacement components
// are bounded by `steepness * wavelength / 2π`, summed with the per-octave
// amplitude weights (0.55, 0.55, 0.5, 0.4, 0.3, 0.22, 0.15, 0.1 = 2.77).
// Detail octaves use the avg-wavelength × avg-steepness fall-back; bounding
// each by the largest octave's product over-estimates, which is the safe
// direction for a culling margin. A 1.5x safety factor absorbs phase / domain-
// warp interactions that the per-axis split-then-sum derivation ignores.
float computeMaxWaveDisplacement()
{
    const float TWO_PI = 6.28318530;
    float freq = max(u_WaveParams.w, 0.01);
    float amp = u_WaveParams.z;

    float wl0 = max(u_WaveDir0.w, 0.1) / freq;
    float wl1 = max(u_WaveDir1.w, 0.1) / freq;
    float a0 = u_WaveDir0.z * wl0 / TWO_PI;
    float a1 = u_WaveDir1.z * wl1 / TWO_PI;

    float avgWL = (wl0 + wl1) * 0.5;
    float avgSt = (u_WaveDir0.z + u_WaveDir1.z) * 0.5;
    float maxOctaveA = avgSt * avgWL / TWO_PI;
    // Detail-octave amp-weight sum = 0.5+0.4+0.3+0.22+0.15+0.1
    float octaveSum = maxOctaveA * 1.67;

    return amp * (a0 * 0.55 + a1 * 0.55 + octaveSum) * 1.5;
}

// True when the patch (corners p0/p1/p2) is entirely outside the view frustum
// once each corner is grown by `margin` along every axis. Uses the standard
// Gribb-Hartmann plane extraction from the view-projection matrix; planes are
// kept un-normalised and the sphere-vs-plane threshold is scaled by
// `length(plane.xyz)` so the test stays correct regardless of perspective
// asymmetry.
bool isPatchOutsideFrustum(vec3 p0, vec3 p1, vec3 p2, float margin)
{
    mat4 vp = u_ViewProjection;
    // GLSL is column-major: vp[col][row]. Row i = vec4(vp[0][i] .. vp[3][i]).
    vec4 row0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 row1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 row2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 row3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

    vec4 planes[6];
    planes[0] = row3 + row0; // left
    planes[1] = row3 - row0; // right
    planes[2] = row3 + row1; // bottom
    planes[3] = row3 - row1; // top
    planes[4] = row3 + row2; // near (GL [-1,1] clip-space depth)
    planes[5] = row3 - row2; // far

    for (int i = 0; i < 6; ++i)
    {
        vec4 plane = planes[i];
        float normFactor = length(plane.xyz);
        // A patch is fully outside this plane when every inflated corner's
        // signed distance is more negative than `margin * normFactor`.
        float d0 = dot(plane.xyz, p0) + plane.w;
        float d1 = dot(plane.xyz, p1) + plane.w;
        float d2 = dot(plane.xyz, p2) + plane.w;
        float threshold = -margin * normFactor;
        if (d0 < threshold && d1 < threshold && d2 < threshold)
            return true;
    }
    return false;
}

void main()
{
    tc_WorldPos[gl_InvocationID] = v_WorldPos[gl_InvocationID];
    tc_Normal[gl_InvocationID] = v_Normal[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];
    tc_PrevWorldPos[gl_InvocationID] = v_PrevWorldPos[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        // Skip patches whose Gerstner-displaced bound lies entirely outside
        // the view frustum. u_TessParams.w == 0 keeps the legacy "always
        // tessellate" behaviour so the cull can be disabled from the C++
        // side without touching the shader.
        bool culled = false;
        if (u_TessParams.w > 0.5)
        {
            float margin = computeMaxWaveDisplacement();
            culled = isPatchOutsideFrustum(v_WorldPos[0], v_WorldPos[1], v_WorldPos[2], margin);
        }

        if (culled)
        {
            // Setting any TessLevelOuter to 0 discards the patch (GL 4.6 §11.2.2),
            // saving the TES invocations and rasterizer setup for off-screen patches.
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
        }
        else
        {
            float e0 = calcTessLevel(v_WorldPos[1], v_WorldPos[2]);
            float e1 = calcTessLevel(v_WorldPos[2], v_WorldPos[0]);
            float e2 = calcTessLevel(v_WorldPos[0], v_WorldPos[1]);

            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = (e0 + e1 + e2) / 3.0;
        }
    }
}
