# The directional shadow belongs to one light (#1533)

Read before touching a lit shader's light loop (`PBR_MultiLight*.glsl`,
`DeferredLightingShared.glsl`, `Foliage_*.glsl`, `Terrain_*.glsl`, `DDGI_Relight.glsl`,
`GroomStrand.glsl`), the directional-light block in `Scene.cpp`'s light gather, or a second
directional light that looks wrong.

## The rules

1. **Gate every directional shadow lookup on the light's INDEX as well as its type:
   `lightType == DIRECTIONAL_LIGHT && i == 0 && u_DirectionalShadowEnabled != 0`.** The CSM
   cascades and VSM's clip map cover one directional light, UBO index 0; directional lights are
   packed first. `u_DirectionalShadowEnabled` means "light 0 casts", not "this light casts". A
   second directional light has no map, so it is unshadowed. It never borrows light 0's map.

2. **Light 0 is the cascades' owner: the brightest directional light that CASTS, packed first by
   `Scene.cpp`.** Never "whichever light the view yields first". EnTT yields the newest entity
   first, so that rule handed the cascades to a non-casting fill light created after the sun. The
   sun then cast nothing, anywhere. With no casting light, the view order stands.

## What it cost

Both halves broke the same scene, a sun plus a rim light. With the sun created first, the rim became
light 0, nothing cast at all, and the dog showcase stood on the lawn with no shadow under it. Had the
sun been light 0, every lit shader would have multiplied the rim light by the sun's map anyway. That
darkens the rim exactly where the sun is blocked, which is exactly where a rim light is meant to
read. The missing rim halo was first blamed on the fur shading.
`DeferredLightingShared.glsl` even said in a comment that only the first light gets the cascades,
two lines above the check that ignored it. Its contact shadows tested `i == 0`; its CSM did not.

## The evidence

`SecondSunShadowVisualEvidenceTest` creates a casting sun, then a second, non-casting sun (the
failing creation order). It renders both on Forward, Forward+ and Deferred. First it asserts that
the sun casts at all (rule 2). Then it measures the second sun's added light inside the sun's shadow
against its added light on lit ground (rule 1). With the rule 1 bug, the gain inside is near zero.
Fixed, it is at least as large as outside, because tone mapping compresses the brighter lit ground
more.

A second light that should cast needs its own map. The ray-traced tier already hands any
directional light a mask channel.
