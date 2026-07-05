#include "OloEnginePCH.h"
#include "ComponentInterpolationRegistry.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    namespace
    {
        // Deserialize a component from a single snapshot byte-blob via the
        // ComponentReplicator wire format. The replicator already sanitizes every
        // untrusted wire float on load (NaN/inf → safe fallback), so the returned
        // value is always finite. See ComponentReplicator.cpp.
        template<typename T>
        [[nodiscard]] T ParseComponent(const std::vector<u8>& bytes)
        {
            T comp{};
            FMemoryReader reader(bytes);
            reader.ArIsNetArchive = true;
            ComponentReplicator::Serialize(reader, comp);
            return comp;
        }

        // ── TransformComponent: translation/scale lerp, rotation slerp ───────
        // Composite policy: the visually-dominant translation is a lerp (hence the
        // headline policy), but rotation must slerp through the shortest arc or a
        // spinning entity pops at each snapshot boundary.

        bool TransformHas(Entity& e)
        {
            return e.HasComponent<TransformComponent>();
        }

        void TransformCapture(FArchive& ar, Entity& e)
        {
            ComponentReplicator::Serialize(ar, e.GetComponent<TransformComponent>());
        }

        void TransformInterpolate(Entity& e, const std::vector<u8>& before, const std::vector<u8>& after, f32 alpha)
        {
            const TransformComponent tb = ParseComponent<TransformComponent>(before);
            const TransformComponent ta = ParseComponent<TransformComponent>(after);
            auto& t = e.GetComponent<TransformComponent>();
            t.Translation = glm::mix(tb.Translation, ta.Translation, alpha);
            t.SetRotation(glm::slerp(tb.GetRotation(), ta.GetRotation(), alpha));
            t.Scale = glm::mix(tb.Scale, ta.Scale, alpha);
        }

        void TransformSnap(Entity& e, const std::vector<u8>& value)
        {
            const TransformComponent tv = ParseComponent<TransformComponent>(value);
            auto& t = e.GetComponent<TransformComponent>();
            t.Translation = tv.Translation;
            t.SetRotation(tv.GetRotation());
            t.Scale = tv.Scale;
        }

        void TransformSmooth(Entity& e, const std::vector<u8>& preReconcile, f32 rate, f32 hardSnap)
        {
            const TransformComponent pre = ParseComponent<TransformComponent>(preReconcile);
            auto& t = e.GetComponent<TransformComponent>();
            // Correction beyond the hard-snap distance is a teleport — keep the
            // resimulated value rather than sliding the camera across the map.
            if (const f32 error = glm::length(t.Translation - pre.Translation); hardSnap > 0.0f && error >= hardSnap)
            {
                return;
            }
            t.Translation = glm::mix(t.Translation, pre.Translation, rate);
            t.SetRotation(glm::slerp(t.GetRotation(), pre.GetRotation(), rate));
            t.Scale = glm::mix(t.Scale, pre.Scale, rate);
        }

        // ── Rigidbody3DComponent: velocity lerp, body-type/mass step ─────────

        bool Rigidbody3DHas(Entity& e)
        {
            return e.HasComponent<Rigidbody3DComponent>();
        }

        void Rigidbody3DCapture(FArchive& ar, Entity& e)
        {
            ComponentReplicator::Serialize(ar, e.GetComponent<Rigidbody3DComponent>());
        }

        void Rigidbody3DInterpolate(Entity& e, const std::vector<u8>& before, const std::vector<u8>& after, f32 alpha)
        {
            const Rigidbody3DComponent rb = ParseComponent<Rigidbody3DComponent>(before);
            const Rigidbody3DComponent ra = ParseComponent<Rigidbody3DComponent>(after);
            auto& rc = e.GetComponent<Rigidbody3DComponent>();
            rc.m_InitialLinearVelocity = glm::mix(rb.m_InitialLinearVelocity, ra.m_InitialLinearVelocity, alpha);
            rc.m_InitialAngularVelocity = glm::mix(rb.m_InitialAngularVelocity, ra.m_InitialAngularVelocity, alpha);
            // Discrete fields hold the most-recent confirmed (before) value.
            rc.m_Type = rb.m_Type;
            rc.m_Mass = rb.m_Mass;
        }

        void Rigidbody3DSnap(Entity& e, const std::vector<u8>& value)
        {
            const Rigidbody3DComponent rv = ParseComponent<Rigidbody3DComponent>(value);
            auto& rc = e.GetComponent<Rigidbody3DComponent>();
            rc.m_Type = rv.m_Type;
            rc.m_Mass = rv.m_Mass;
            rc.m_InitialLinearVelocity = rv.m_InitialLinearVelocity;
            rc.m_InitialAngularVelocity = rv.m_InitialAngularVelocity;
        }

        void Rigidbody3DSmooth(Entity& e, const std::vector<u8>& preReconcile, f32 rate, f32 /*hardSnap*/)
        {
            const Rigidbody3DComponent pre = ParseComponent<Rigidbody3DComponent>(preReconcile);
            auto& rc = e.GetComponent<Rigidbody3DComponent>();
            rc.m_InitialLinearVelocity = glm::mix(rc.m_InitialLinearVelocity, pre.m_InitialLinearVelocity, rate);
            rc.m_InitialAngularVelocity = glm::mix(rc.m_InitialAngularVelocity, pre.m_InitialAngularVelocity, rate);
        }

        // ── AnimationStateComponent: step (discrete state) ───────────────────
        // The networked subset is the discrete clip selection + playback cursor;
        // it is never blended — a half-way-between-two-clips state is meaningless.
        // The client snaps to the most-recent confirmed value and its local
        // animation system advances the cursor between snapshots.

        bool AnimationHas(Entity& e)
        {
            return e.HasComponent<AnimationStateComponent>();
        }

        void AnimationCapture(FArchive& ar, Entity& e)
        {
            ComponentReplicator::Serialize(ar, e.GetComponent<AnimationStateComponent>());
        }

        void AnimationSnap(Entity& e, const std::vector<u8>& value)
        {
            const AnimationStateComponent av = ParseComponent<AnimationStateComponent>(value);
            auto& ac = e.GetComponent<AnimationStateComponent>();
            ac.m_State = av.m_State;
            ac.m_CurrentClipIndex = av.m_CurrentClipIndex;
            ac.m_CurrentTime = av.m_CurrentTime;
            ac.m_IsPlaying = av.m_IsPlaying;
        }

        void AnimationInterpolate(Entity& e, const std::vector<u8>& before, const std::vector<u8>& /*after*/, f32 /*alpha*/)
        {
            // Step: hold the most-recent confirmed value across the whole interval.
            AnimationSnap(e, before);
        }
    } // namespace

    std::vector<InterpolationEntry> ComponentInterpolationRegistry::s_Entries;
    bool ComponentInterpolationRegistry::s_Initialized = false;

    void ComponentInterpolationRegistry::EnsureInitialized()
    {
        if (s_Initialized)
        {
            return;
        }
        // Set first: the default appends below must not recurse back through the
        // public read API (which would re-enter EnsureInitialized).
        s_Initialized = true;

        s_Entries.push_back({
            .Id = HashName("TransformComponent"),
            .Name = "TransformComponent",
            .Policy = EInterpolationPolicy::Lerp,
            .Has = &TransformHas,
            .Capture = &TransformCapture,
            .Interpolate = &TransformInterpolate,
            .Snap = &TransformSnap,
            .Smooth = &TransformSmooth,
        });

        s_Entries.push_back({
            .Id = HashName("Rigidbody3DComponent"),
            .Name = "Rigidbody3DComponent",
            .Policy = EInterpolationPolicy::Lerp,
            .Has = &Rigidbody3DHas,
            .Capture = &Rigidbody3DCapture,
            .Interpolate = &Rigidbody3DInterpolate,
            .Snap = &Rigidbody3DSnap,
            .Smooth = &Rigidbody3DSmooth,
        });

        s_Entries.push_back({
            .Id = HashName("AnimationStateComponent"),
            .Name = "AnimationStateComponent",
            .Policy = EInterpolationPolicy::Step,
            .Has = &AnimationHas,
            .Capture = &AnimationCapture,
            .Interpolate = &AnimationInterpolate,
            .Snap = &AnimationSnap,
            .Smooth = nullptr, // discrete state — nothing to ease during reconciliation
        });

        OLO_CORE_TRACE("[ComponentInterpolationRegistry] Registered {} default interpolatable components", s_Entries.size());
    }

    void ComponentInterpolationRegistry::RegisterDefaults()
    {
        EnsureInitialized();
    }

    void ComponentInterpolationRegistry::Register(InterpolationEntry entry)
    {
        EnsureInitialized();
        entry.Id = HashName(entry.Name);

        // Reject duplicates: a second entry sharing a wire id (or name) would make
        // EntitySnapshot emit both copies while FindById/FindByName only ever resolve
        // the first — keep exactly one visible registration per component. (Matching
        // ids on different names is a hash collision, which would also corrupt the
        // wire format, so reject that too.)
        for (const auto& existing : s_Entries)
        {
            if (existing.Id == entry.Id || existing.Name == entry.Name)
            {
                OLO_CORE_WARN("[ComponentInterpolationRegistry] Ignoring duplicate registration for '{}' (id {} collides with already-registered '{}')", entry.Name, entry.Id, existing.Name);
                return;
            }
        }

        s_Entries.push_back(std::move(entry));
    }

    const std::vector<InterpolationEntry>& ComponentInterpolationRegistry::GetEntries()
    {
        EnsureInitialized();
        return s_Entries;
    }

    const InterpolationEntry* ComponentInterpolationRegistry::FindById(u32 id)
    {
        EnsureInitialized();
        for (const auto& entry : s_Entries)
        {
            if (entry.Id == id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    const InterpolationEntry* ComponentInterpolationRegistry::FindByName(std::string_view name)
    {
        EnsureInitialized();
        for (const auto& entry : s_Entries)
        {
            if (entry.Name == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    void ComponentInterpolationRegistry::Clear()
    {
        s_Entries.clear();
        s_Initialized = false;
    }
} // namespace OloEngine
