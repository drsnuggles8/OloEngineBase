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
//           same result for valid data. Scalar Float/Int/UInt/SmallInt/SmallUInt/Enum
//           fields and glm::vec3 (clamped per-component) support Clamp today
//           (Vec2/Vec4/vector-of-T clamping remain a follow-up) — requesting it on
//           any other type marks the whole component non-trivial (fail-safe) rather
//           than silently dropping the annotation.
//           Note this is CLAMP-to-range, not REJECT-out-of-range — for that, use
//           Reject below.
//  Reject — The sibling of Clamp: same Min/Max bounds, opposite failure mode. An
//           out-of-range (or, for a float, non-finite) value leaves the field at its
//           CONSTRUCTOR DEFAULT instead of saturating at the nearest bound.
//           Reach for this whenever saturating would turn a corrupt value into a
//           different VALID one. The motivating case is an enum: Clamp(0, 2) maps a
//           corrupt `7` to enumerator 2, silently selecting a real-but-wrong mode,
//           while every other load path for such a field maps anything unrecognised
//           back to enumerator 0. Rule of thumb — Clamp for a continuous quantity
//           where "as close as we can get" is meaningful (a density, a radius),
//           Reject for a discriminated one where it is not (an enum, a mode index).
//           Scalar Float/Int/UInt/SmallInt/SmallUInt/Enum fields only: glm::vec3 is
//           deliberately unsupported, since rejecting one bad component would leave
//           a half-updated vector. Mutually exclusive with Clamp; requesting both,
//           or requesting Reject on an unsupported type, marks the whole component
//           non-trivial (fail-safe) rather than silently picking one.
//           Applies on BOTH generated read paths (YAML and the binary scene
//           writer). At the MCP boundary the value is CLAMPED to the same bounds
//           instead — MCP can only range a write, not refuse it, and a bounded
//           write beats an unvalidated one.
//   Min   — Clamp/Reject lower bound, e.g. Min = 0.0f. Emitted as a static_cast to
//           the field's own type, so an int literal is fine for a float field. For a
//           glm::vec3 field, broadcast to all three components via glm::vec3(Min).
//   Max   — Clamp/Reject upper bound, e.g. Max = 100.0f.
//
// Usage:
//   struct SomeComponent
//   {
//       OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
//       f32 m_Density = 0.5f;   // deserialize: m_Density = std::clamp(v, 0.0f, 100.0f)
//       OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
//       glm::vec3 m_Extents{ 0.5f, 0.5f, 0.5f };
//       // deserialize: m_Extents = glm::clamp(v, glm::vec3(0.0f), glm::vec3(1000.0f))
//       OLO_SERIALIZE(Reject, Min = 0, Max = 2)
//       SomeMode m_Mode = SomeMode::First;
//       // deserialize: if (const int v = ...; v >= 0 && v <= 2) m_Mode = SomeMode(v);
//       // i.e. a corrupt 7 stays SomeMode::First rather than becoming mode 2.
//   };
//
// May co-exist with OLO_PROPERTY on the same field (e.g. a runtime field exposed to
// scripts but not serialized); order between the two markers does not matter.

// These macros expand to nothing — they are markers for OloHeaderTool.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define OLO_PROPERTY(...)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define OLO_SERIALIZE(...)
