#ifndef OLO_TERRAIN_DISPLACED_HEIGHT_GLSL
#define OLO_TERRAIN_DISPLACED_HEIGHT_GLSL

// Beauty and every shadow technique must morph the same displaced surface.
float oloTerrainDisplacedHeight(float sampledHeight, float snowDisplacement,
                               float meshHeight, float morphFactor)
{
    return mix(sampledHeight + snowDisplacement, meshHeight, morphFactor);
}

#endif
