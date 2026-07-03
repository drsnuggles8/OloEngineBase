#pragma once

// =============================================================================
// OloEngine Reflection Macros
//
// Marker macros parsed by OloHeaderTool to generate scripting bindings
// (C++ Mono glue, C# proxy classes, Lua Sol2 bindings).
//
// Usage:
//   struct SomeComponent
//   {
//       OLO_PROPERTY()
//       float m_Speed = 1.0f;
//
//       OLO_PROPERTY(Name = "ProjectionType", Type = "int",
//           Get = "static_cast<int>(Camera.GetProjectionType())",
//           Set = "Camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>({v}))")
//       SceneCamera Camera;
//   };
//
// Supported metadata keys:
//   Name  — Override the script-facing property name (default: field name sans m_ prefix)
//   Type  — Override the exposed type (float/bool/int/vec2/vec3/vec4)
//   Get   — Custom getter expression on the component (uses $ for component ref)
//   Set   — Custom setter expression ({v} = incoming value)
//   Skip  — If "true", the next field is the anchor but no binding is generated
//           (used when OLO_PROPERTY carries only metadata for custom PROP)
// =============================================================================

// Scene-serializer field directive, parsed by OloHeaderTool's scene-serializer
// codegen (the full data-member scan, NOT the OLO_PROPERTY scan). Controls how the
// generated Scene{Serialize,Deserialize}Components.Generated.inl treats the member
// it precedes.
//
// Usage:
//   struct SomeComponent
//   {
//       glm::vec4 m_Color = { 1, 1, 1, 1 };   // authored — round-trips normally
//
//       // Runtime state — not serialized
//       OLO_SERIALIZE(Skip)
//       SomeState m_State = SomeState::Idle;   // omitted from scene YAML, kept at ctor default on load
//   };
//
// Supported metadata keys:
//   Skip  — If present (or "= true"), the field is dropped from generated scene
//           serialize/deserialize AND does not mark the component non-trivial. This
//           lets an otherwise all-trivial component with one runtime-only field be
//           fully generated instead of hand-written + kComponentsCustomSerialize.
//   Clamp — Marks the field range-validated on deserialize. Requires at least one
//           of Min / Max (below); the deserialize block ranges the read value into
//           [Min, Max] (both given), or applies a one-sided std::max/std::min (only
//           one given). Mirrors the SanitizeFloat/std::clamp idiom every hand-written
//           clamp-only component already used, so the generated block reproduces the
//           same result for valid data. Only scalar Float/Int/UInt/SmallInt/
//           SmallUInt/Enum fields support Clamp today (Vec*/vector-of-T clamping is a
//           follow-up) — requesting it on any other type marks the whole component
//           non-trivial (fail-safe) rather than silently dropping the annotation.
//           Note this is CLAMP-to-range, not REJECT-out-of-range: a component whose
//           hand-written deserialize instead *rejects* an out-of-bounds value (keeps
//           the constructor default rather than clamping to the bound) is a different
//           semantic and must stay hand-written.
//   Min   — Clamp lower bound, e.g. Min = 0.0f. Emitted as a static_cast to the
//           field's own type, so an int literal is fine for a float field.
//   Max   — Clamp upper bound, e.g. Max = 100.0f.
//
// Usage:
//   struct SomeComponent
//   {
//       OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
//       f32 m_Density = 0.5f;   // deserialize: m_Density = std::clamp(v, 0.0f, 100.0f)
//   };
//
// May co-exist with OLO_PROPERTY on the same field (e.g. a runtime field exposed to
// scripts but not serialized); order between the two markers does not matter.

// These macros expand to nothing — they are markers for OloHeaderTool.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define OLO_PROPERTY(...)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define OLO_SERIALIZE(...)
