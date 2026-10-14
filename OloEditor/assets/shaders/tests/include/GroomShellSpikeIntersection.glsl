#ifndef OLO_GROOM_SHELL_SPIKE_INTERSECTION
#define OLO_GROOM_SHELL_SPIKE_INTERSECTION
// Camera-relative intersection. Avoid subtracting two distance-squared terms
// when a millimetre shell is viewed hundreds of metres from the body.
bool oloGroomShellSpikeIntersect(vec3 origin, vec3 ray, float radius, bool front, out vec3 position)
{
    float a = dot(ray.xy, ray.xy);
    if (a < 1e-8) return false;
    float closestT = -dot(origin.xy, ray.xy) / a;
    vec3 closest = fma(ray, vec3(closestT), origin);
    float discriminant = radius * radius - dot(closest.xy, closest.xy);
    if (discriminant < 0.0) return false;
    float offset = (front ? -1.0 : 1.0) * sqrt(discriminant / a);
    position = closest + offset * ray;
    return true;
}
#endif
