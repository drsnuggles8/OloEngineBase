#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class FArchive;
    class Entity;

    // How a replicated component's fields blend between two server snapshots on
    // the client. This is the *headline* mode for the whole component — composite
    // components (e.g. TransformComponent slerps its rotation sub-field while
    // lerping translation/scale) carry their own bespoke Interpolate fn, which is
    // authoritative. The enum is metadata for introspection / tests / docs.
    enum class EInterpolationPolicy : u8
    {
        Lerp,  // linear blend — positions, velocities, scales
        Slerp, // spherical blend — rotations / quaternions
        Step,  // no blend — hold the most-recent confirmed value (discrete / animation state)
    };

    // Describes one component that participates in snapshot replication *and*
    // client-side interpolation / reconciliation. Every field is type-erased
    // through a stateless function pointer (the concrete component type is baked
    // into each fn), so the wire/interpolation code never names a concrete
    // component. (Deliberately *not* named `*Component` — it's a registry record,
    // not an ECS component, and must stay out of OloHeaderTool's component scan.)
    //
    // This is the data-driven seam issue #462 hangs off: making a networked
    // component interpolate smoothly is a single Register() call, not a new code
    // path in EntitySnapshot / SnapshotInterpolator / ClientPrediction.
    struct InterpolationEntry
    {
        // Stable wire id (FNV-1a-32 of Name) written into every snapshot so the
        // reader matches components order-independently and skips unknown ids by
        // their byte length — the format tolerates registry drift and additions.
        u32 Id = 0;
        std::string Name;
        EInterpolationPolicy Policy = EInterpolationPolicy::Lerp;

        // Does the live entity currently carry this component?
        bool (*Has)(Entity&) = nullptr;
        // Serialize the live component into the (net) archive — the write path.
        void (*Capture)(FArchive&, Entity&) = nullptr;
        // Blend the entity's live component between two serialized snapshots
        // (before/after) by alpha in [0,1] and write the result back.
        void (*Interpolate)(Entity&, const std::vector<u8>& before, const std::vector<u8>& after, f32 alpha) = nullptr;
        // Snap the entity's live component to a single serialized snapshot — used
        // when only one bracket has the entity, and for full-state apply (rollback).
        void (*Snap)(Entity&, const std::vector<u8>& value) = nullptr;
        // Reconciliation smoothing: ease the entity's *current* (resimulated)
        // component back toward a pre-reconcile server snapshot by `rate` (0..1).
        // hardSnap > 0 => if the correction magnitude exceeds it, keep the
        // resimulated value (a teleport, not a smooth slide). Step components
        // leave this null (a discrete value has nothing to ease).
        void (*Smooth)(Entity&, const std::vector<u8>& preReconcile, f32 rate, f32 hardSnap) = nullptr;
    };

    // Process-wide registry of interpolatable components. Lazily initialises its
    // default set on first read so the snapshot/interpolation paths work without
    // an explicit init (matching the old hard-coded-transform behaviour). The
    // registration order is the canonical order EntitySnapshot serialises in; the
    // wire id (a name hash) makes the actual matching order-independent.
    class ComponentInterpolationRegistry
    {
      public:
        // Register the built-in interpolatable components (idempotent).
        static void RegisterDefaults();

        // Append a custom interpolatable component (Id is filled from Name).
        static void Register(InterpolationEntry entry);

        // The full ordered set (defaults are auto-registered on first access).
        [[nodiscard]] static const std::vector<InterpolationEntry>& GetEntries();

        // Look up an entry by wire id / name. Returns nullptr if not registered.
        [[nodiscard]] static const InterpolationEntry* FindById(u32 id);
        [[nodiscard]] static const InterpolationEntry* FindByName(std::string_view name);

        // Drop every entry. The next read re-registers the defaults (so a cleared
        // registry is never observed empty by the snapshot paths). For test isolation.
        static void Clear();

        // FNV-1a-32 of a component name → its stable wire id.
        [[nodiscard]] static constexpr u32 HashName(std::string_view name) noexcept
        {
            u32 hash = 2166136261u;
            for (const char c : name)
            {
                hash ^= static_cast<u8>(c);
                hash *= 16777619u;
            }
            return hash;
        }

      private:
        static void EnsureInitialized();

        static std::vector<InterpolationEntry> s_Entries;
        static bool s_Initialized;
    };
} // namespace OloEngine
