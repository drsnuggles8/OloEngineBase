#ifndef OLO_WATER_TILE_FRAME_GLSL
#define OLO_WATER_TILE_FRAME_GLSL

// Local X/Z span the drawn grid even when the parent transform shears it.
// Keep the winding here: both water fragment programs select the same face
// from the side of the displaced surface the viewer occupies.
vec3 waterGridNormal(mat4 model)
{
    vec3 normal = cross(model[2].xyz, model[0].xyz);
    float magnitude = length(normal);
    return magnitude > 0.0 ? normal / magnitude : vec3(0.0, 1.0, 0.0);
}

bool waterCameraBelowFragment(vec3 restNormal, vec3 camera, vec3 surface)
{
    return dot(camera - surface, restNormal) < 0.0;
}
#endif
