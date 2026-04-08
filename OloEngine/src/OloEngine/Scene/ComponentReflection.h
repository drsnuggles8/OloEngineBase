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

// These macros expand to nothing — they are markers for OloHeaderTool.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define OLO_PROPERTY(...)
