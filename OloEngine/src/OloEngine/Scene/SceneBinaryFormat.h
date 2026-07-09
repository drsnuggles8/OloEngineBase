#pragma once

#include "OloEngine/Core/Base.h"

#include <type_traits>

namespace OloEngine
{
    // ============================================================================
    // .scenebin — Binary Scene Sidecar Cache — Version 1  (issue #525)
    //
    // A derived, disposable cache written next to a scene's `.olo`/`.yaml` source
    // (`<scene>.scenebin`). YAML stays the source of truth; the sidecar exists
    // only to skip yaml-cpp's per-entity node traversal on subsequent loads, the
    // dominant cost for entity-heavy scenes (100k transform-only entities: ~43 s
    // just to tokenize + ~55 s to node-walk/convert, vs ~1 s from binary — see
    // #525 / #584).
    //
    // HYBRID coverage. Every component loads correctly through the sidecar; the
    // format is per-entity, and each entity is stored in one of two kinds:
    //   * kBinary — the entity's every component is in the binary-covered set
    //     (IDComponent + TagComponent + TransformComponent + the OloHeaderTool
    //     auto-generated trivial component set). Stored as typed binary: fast.
    //   * kYaml   — the entity carries at least one component outside that set
    //     (a mesh, sprite, camera, script, particle system, …). Stored as the
    //     exact text SceneSerializer::SerializeEntity emits, and loaded back
    //     through the UNCHANGED SceneSerializer::DeserializeEntity path. This is
    //     correct for ANY component with zero reimplementation / drift risk; it
    //     just doesn't get the binary speedup.
    // A scene of thousands of simple entities (the #525 case) is all-kBinary and
    // gets the full speedup; a mixed scene gets a speedup proportional to its
    // simple-entity fraction; nothing is ever dropped or falls back wholesale.
    //
    // Layout (all multi-byte values little-endian, like MeshBinaryFormat.h):
    //   [FileHeader]
    //   string SceneName
    //   string SettingsYAML              — the scene doc minus its Entities
    //                                       sequence, re-emitted (tiny; reused so
    //                                       scene-level settings never drift).
    //   [EntityCount × EntityRecord]:
    //     u8     Kind (kBinary / kYaml)
    //     u64    UUID
    //     string Tag
    //     if kBinary: repeated { u32 ComponentId; <typed component payload> }
    //                 terminated by a u32 0 sentinel. ComponentId is a stable
    //                 FNV-1a-32 of the component type name (compiler-independent,
    //                 unlike entt's type_hash — so sidecars are portable).
    //     if kYaml:   string EntityYAML   (SerializeEntity output for this entity)
    //
    // Invalidation (see SceneSerializer::TryLoadBinarySidecar): the fast path runs
    // only when Magic matches, Version is in [MinSupportedVersion, CurrentVersion],
    // the recorded SceneSchemaVersion equals this build's
    // SceneSerializer::CurrentVersion (a YAML-schema bump re-derives the cache
    // rather than migrating binary bytes), and the recorded source size + timestamp
    // match the current `.olo`. Any mismatch, short read, non-finite float, or
    // unknown ComponentId abandons the fast path (destroying any entities created
    // so far) and falls back to YAML, which rewrites a fresh sidecar.
    // ============================================================================

    namespace OSceneFormat
    {
        constexpr u32 MagicNumber = 0x424E4353; // "SCNB" in little-endian
        constexpr u32 CurrentVersion = 1;
        constexpr u32 MinSupportedVersion = 1;

        // Per-entity storage kind (the u8 that prefixes each EntityRecord).
        enum EntityKind : u8
        {
            kBinary = 0, // typed binary component blocks
            kYaml = 1,   // SerializeEntity text (complex/uncovered components)
        };

        // ── Safety caps for deserialized counts/lengths (defence against a
        //    corrupt or truncated sidecar) ──
        constexpr u32 MaxEntityCount = 50'000'000;
        constexpr u32 MaxTagLength = 65'536; // entity tag / scene name

        // Fixed-size file header. Body (name, settings blob, entity records)
        // follows immediately after.
        struct FileHeader
        {
            u32 Magic = MagicNumber;
            u32 Version = CurrentVersion;
            u32 SceneSchemaVersion = 0; // SceneSerializer::CurrentVersion at write time
            u32 Flags = 0;              // reserved (e.g. future zlib-compressed body)
            u64 SourceFileSize = 0;     // source `.olo` size in bytes, for staleness
            u64 SourceTimestamp = 0;    // source `.olo` last-write-time count, for staleness
            u32 EntityCount = 0;
            u32 Reserved = 0; // padding / future use — keeps the header 8-byte aligned
        };
    } // namespace OSceneFormat

    // ── Compile-time ABI guard for the wire-format header ──
    // Any padding or field-order change breaks binary compatibility with
    // sidecars already on disk; bump OSceneFormat::CurrentVersion if you must.
    static_assert(std::is_trivially_copyable_v<OSceneFormat::FileHeader>);
    static_assert(std::is_standard_layout_v<OSceneFormat::FileHeader>);
    static_assert(sizeof(OSceneFormat::FileHeader) == 40);

} // namespace OloEngine
