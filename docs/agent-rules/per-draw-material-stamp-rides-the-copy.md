# A per-draw material stamp only survives if `Material`'s copy carries it

**Rule.** A value stamped onto a per-draw copy of a `Material` must be copied by `Material`'s
hand-written copy constructor **and** `operator=` in `Material.cpp`, and pinned in
`MaterialCopyTest`'s fill/check helpers. A test for a stamped value must drive the engine path that
makes the copy. Setting the field directly on the component's material skips that copy.

## What happened (#1243, found by #1395)

#1243 added `Material::m_SkinExpressionDetail`, a runtime-only value stamped per draw from the
entity's applied morph weights. `Scene.cpp::StampSkinExpression` copies the shared material, sets
the value on the copy and returns it in a `std::optional<Material>`. Building that optional runs
`Material`'s copy constructor, and the constructor did not copy the new field. Every stamped draw
therefore reached `CreatePODMaterialDataForMaterial` with the value reset to 0. The
expression-driven pore band was inert on the skinned paths from the day it shipped.

Nothing noticed, for two reasons:

- The epic evidence test (`SkinDigitalHumanEvidenceTest`) worked around a different gap (#1395:
  the rigid path never stamped at all) by calling `SetSkinExpressionDetail` on the **component's**
  material. The rigid path passes that material to `DrawMesh` by reference with no copy, so the
  hand-stamp reached the shader and the test was green.
- `MaterialCopyTest` checks a list of fields. It cannot notice a field that was never added to
  the list.

The same hazard is written above the members in `Material.h` and above the copy constructor in
`Material.cpp`, from alpha mode (#629), transmission (#970) and material kind (#1231). This is the
fourth time it has happened.

## How it was found

The #1395 rewrite removed the hand-stamp and A/B-ed the profile's gain (2.4 against 0) at a full
expression. The frame moved only at the rims where the eyes and lips meet the cranium, and that
happened at a neutral face as well. The pore pattern itself was identical. A temporary log in the
rigid arm printed `stamped=true detail=0`: the stamp was decided, and its value was gone by the
time the draw received the material. With the copy fixed, the same A/B moves 48–55 k pixels by up
to 84–93/255.

## Checklist when adding a runtime material field

1. Add it to the copy constructor and `operator=` in `Material.cpp`.
2. Set a non-default value in `MaterialCopyTest`'s fill helper and check it in the check helper.
3. Make any test of the value go through the real submission path. A value set on the source
   material proves nothing about a path that copies it.
