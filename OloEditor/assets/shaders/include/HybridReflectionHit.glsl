#ifndef OLO_HYBRID_REFLECTION_HIT_GLSL
#define OLO_HYBRID_REFLECTION_HIT_GLSL

// Cone approximation for one mirror ray per pixel. The cone keeps the
// primary pixel's angular diameter after reflection. Project two diameter
// vectors onto the hit plane and map them through the triangle's UV Jacobian.
// Grazing incidence is bounded to an 8x stretch to contain oversmoothing
// at silhouettes. These are analytic ray footprints independent of quad
// derivatives and therefore safe after divergent traversal.
mat2 oloReflectHitFootprint(vec3 e1, vec3 e2, vec2 d1, vec2 d2, vec3 direction, float diameter)
{
    const vec3 winding = cross(e1, e2);
    const float area = length(winding);
    if (!(area > 1e-12) || isinf(area))
        return mat2(0.0);

    const vec3 planeNormal = winding / area;
    const vec3 step1 = cross(e2, planeNormal) / area;
    const vec3 step2 = cross(planeNormal, e1) / area;
    const vec3 gradientU = d1.x * step1 + d2.x * step2;
    const vec3 gradientV = d1.y * step1 + d2.y * step2;
    const vec3 side = abs(direction.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    const vec3 axisX = normalize(cross(side, direction)) * diameter;
    const vec3 axisY = cross(direction, axisX);
    const float incidence = dot(planeNormal, direction);
    const float safeIncidence = (incidence < 0.0 ? -1.0 : 1.0) * max(abs(incidence), 0.125);
    const vec3 hitX = axisX - direction * (dot(planeNormal, axisX) / safeIncidence);
    const vec3 hitY = axisY - direction * (dot(planeNormal, axisY) / safeIncidence);
    const vec2 dx = vec2(dot(gradientU, hitX), dot(gradientV, hitX));
    const vec2 dy = vec2(dot(gradientU, hitY), dot(gradientV, hitY));
    if (any(isnan(vec4(dx, dy))) || any(isinf(vec4(dx, dy))))
        return mat2(0.0);
    return mat2(dx, dy);
}

#endif
