#ifndef OLO_GROOM_SOFT_INTERSECTION_GLSL
#define OLO_GROOM_SOFT_INTERSECTION_GLSL

float oloGroomIntersectionCoverage(float separation, float distance, float curveParameter, float protectedRoot)
{
    if (distance <= 0.0)
        return 1.0;
    float root = clamp(protectedRoot, 0.0, 1.0);
    if (root >= 1.0 || (root > 0.0 && curveParameter <= root))
        return 1.0;
    float transition = min(root, 1.0 - root);
    float t = transition > 0.0 ? clamp((curveParameter - root) / transition, 0.0, 1.0)
                               : (curveParameter < root ? 0.0 : 1.0);
    float weight = t * t * (3.0 - 2.0 * t);
    return mix(1.0, clamp(separation / distance, 0.0, 1.0), weight);
}

// The reconstruction projection retains GL's [-1,1] Z on BOTH backends.
// Device depth has already been remapped to [0,1] by either rasterizer.
// Solving clip.z/clip.w also handles orthographic cameras.
float oloGroomViewDistance(float depth, mat4 projection)
{
    float z = depth * 2.0 - 1.0;
    return -(projection[3][2] - z * projection[3][3]) /
             (z * projection[2][3] - projection[2][2]);
}

#endif
