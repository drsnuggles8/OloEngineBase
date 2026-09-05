#include "OloEnginePCH.h"
#include "OloEngine/Scripting/VisualScript/ComponentFieldRegistry.h"

#include "OloEngine/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace OloEngine::VisualScript
{
    namespace
    {
        //======================================================================
        // The PinValue <-> C++ member bridge.
        //
        // One specialization per PinType the generator emits. Each is a pair of
        // static templates over the MEMBER type, so a single `Int` bridge serves
        // i32 / u32 / u8 / i16 / every enum, and the narrowing back into the
        // member's own width happens in exactly one place.
        //======================================================================

        /// Change detection. Mirrors the MCP registry's `FieldChanged` — memcmp on
        /// a trivially-copyable field rather than `operator==` — because the
        /// question here is "did the stored bytes move", not "are these two values
        /// numerically equal". That distinction is why this is not a float
        /// comparison in disguise and does not violate the repo's no-`==`-on-floats
        /// rule: it deliberately reports a change for two values that compare equal
        /// but are stored differently (-0.0 vs 0.0), which is the honest answer for
        /// an undo/dirty signal.
        template<typename T>
        [[nodiscard]] bool StoredValueMoved(const T& before, const T& after)
        {
            if constexpr (std::is_same_v<T, std::string>)
            {
                return before != after;
            }
            else
            {
                static_assert(std::is_trivially_copyable_v<T>,
                              "StoredValueMoved: field must be trivially copyable or std::string");
                return std::memcmp(&before, &after, sizeof(T)) != 0;
            }
        }

        /// Clamp into the entry's declared bounds. Bounds travel as `f64` because
        /// every rangeable field type — f32, i32, u32, a small int, an enum's
        /// underlying type, a float-vector component — is exactly representable in
        /// a double, so no bound is lost on the way in.
        [[nodiscard]] f64 ApplyBounds(f64 value, const FieldBounds& bounds)
        {
            if (bounds.m_Min.m_Present && value < bounds.m_Min.m_Value)
                value = bounds.m_Min.m_Value;
            if (bounds.m_Max.m_Present && value > bounds.m_Max.m_Value)
                value = bounds.m_Max.m_Value;
            return value;
        }

        /// Store `next` into `member` and report what happened. The single place a
        /// write lands, so change detection cannot diverge between bridges.
        template<typename T>
        [[nodiscard]] FieldWriteResult Store(T& member, const T& next)
        {
            if (!StoredValueMoved(member, next))
                return FieldWriteResult::Unchanged;
            member = next;
            return FieldWriteResult::Changed;
        }

        template<PinType Kind>
        struct PinBridge;

        template<>
        struct PinBridge<PinType::Bool>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeBool(static_cast<bool>(member));
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds&)
            {
                return Store<T>(member, static_cast<T>(value.AsBool()));
            }
        };

        template<>
        struct PinBridge<PinType::Int>
        {
            /// The integral type a member is ranged and cast through. For an enum
            /// that is its underlying type — clamping in the enum type itself is
            /// not meaningful, and `static_cast<Enum>` of an out-of-range integer
            /// is exactly the corruption `OLO_SERIALIZE(Reject)` exists to stop.
            //
            // Spelled through std::conditional_t of the TRAITS, not of the
            // types: `std::conditional_t<is_enum, std::underlying_type_t<T>, T>`
            // is a hard error for every non-enum T, because both branches of
            // conditional_t are instantiated eagerly and
            // `std::underlying_type_t<unsigned>` has no `type`. Deferring the
            // `::type` until after the choice is what makes one bridge serve
            // both enums and plain integers.
            template<typename T>
            using Underlying =
                typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>::type;

            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeInt(static_cast<i64>(static_cast<Underlying<T>>(member)));
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds& bounds)
            {
                using U = Underlying<T>;
                // Two clamps, both mandatory and in this order. The declared bounds
                // come first because they are the semantic range the scene loader
                // enforces; the type-width clamp comes second because a cast of an
                // out-of-range integer to a narrower type is implementation-defined
                // at best (and UB for an enum with a fixed range). Without the
                // second, a graph writing 9999 into a `u8` field would wrap to 15.
                f64 wide = ApplyBounds(static_cast<f64>(value.AsInt()), bounds);
                wide = std::clamp(wide, static_cast<f64>(std::numeric_limits<U>::lowest()),
                                  static_cast<f64>(std::numeric_limits<U>::max()));
                return Store<T>(member, static_cast<T>(static_cast<U>(wide)));
            }
        };

        template<>
        struct PinBridge<PinType::Float>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeFloat(static_cast<f32>(member));
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds& bounds)
            {
                // IsFinite() on the VALUE, not isfinite() on AsFloat(): the
                // accessor already maps a non-finite Float to 0.0f (see
                // PinValue::AsFloat), so testing its result can never fail and a
                // NaN would land as a silent, plausible-looking 0. IsFinite
                // inspects the stored value, which is the actual question.
                if (!value.IsFinite())
                    return FieldWriteResult::Rejected;
                return Store<T>(member, static_cast<T>(ApplyBounds(static_cast<f64>(value.AsFloat()), bounds)));
            }
        };

        /// Shared by Vec2/Vec3/Vec4. Bounds apply per component, matching the
        /// serializer's `OLO_SERIALIZE(Clamp)` on a `glm::vec3` (a component-wise
        /// `glm::clamp`), and the finiteness check is whole-vector: rejecting one
        /// bad component would leave a half-updated value, which is the same
        /// reasoning that keeps `Reject` off vectors in the serializer.
        template<typename V>
        [[nodiscard]] FieldWriteResult WriteVector(V& member, const PinValue& value, const V& incoming,
                                                   const FieldBounds& bounds)
        {
            // Whole-vector refusal, tested on the PinValue for the same reason
            // the scalar path gives. Whole-vector rather than per-component
            // because rejecting one component would leave a half-updated value
            // - the same reasoning that keeps OLO_SERIALIZE(Reject) off vectors.
            if (!value.IsFinite())
                return FieldWriteResult::Rejected;
            V next = incoming;
            for (glm::length_t i = 0; i < static_cast<glm::length_t>(V::length()); ++i)
                next[i] = static_cast<f32>(ApplyBounds(static_cast<f64>(next[i]), bounds));
            return Store<V>(member, next);
        }

        template<>
        struct PinBridge<PinType::Vec2>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeVec2(member);
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds& bounds)
            {
                return WriteVector<T>(member, value, value.AsVec2(), bounds);
            }
        };

        template<>
        struct PinBridge<PinType::Vec3>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeVec3(member);
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds& bounds)
            {
                return WriteVector<T>(member, value, value.AsVec3(), bounds);
            }
        };

        template<>
        struct PinBridge<PinType::Vec4>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeVec4(member);
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds& bounds)
            {
                return WriteVector<T>(member, value, value.AsVec4(), bounds);
            }
        };

        template<>
        struct PinBridge<PinType::String>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeString(member);
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds&)
            {
                return Store<T>(member, value.AsString());
            }
        };

        template<>
        struct PinBridge<PinType::Entity>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeEntity(UUID(static_cast<u64>(member)));
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds&)
            {
                return Store<T>(member, static_cast<T>(UUID(static_cast<u64>(value.AsEntity()))));
            }
        };

        template<>
        struct PinBridge<PinType::Asset>
        {
            template<typename T>
            [[nodiscard]] static PinValue Read(const T& member)
            {
                return PinValue::MakeAsset(UUID(static_cast<u64>(member)));
            }
            template<typename T>
            [[nodiscard]] static FieldWriteResult Write(T& member, const PinValue& value, const FieldBounds&)
            {
                return Store<T>(member, static_cast<T>(UUID(static_cast<u64>(value.AsAsset()))));
            }
        };

        //======================================================================
        // Entry construction.
        //
        // `AccessT` is a CAPTURELESS lambda mapping a component reference to an
        // lvalue for the member — the whole access chain, so a field inside a
        // nested record ("System.Emitter.RateOverTime") is reached the same way a
        // top-level one is. Captureless means default-constructible, which is what
        // lets every closure below stay a plain function pointer instead of a
        // std::function: ~1.5k entries, no allocation, no indirect-call table.
        //======================================================================

        template<typename C, PinType Kind, typename AccessT>
        [[nodiscard]] ComponentFieldEntry MakeFieldEntry(const char* component, const char* field, AccessT,
                                                         FieldBounds bounds = {})
        {
            using MemberT = std::remove_reference_t<decltype(AccessT{}(std::declval<C&>()))>;

            ComponentFieldEntry entry;
            entry.m_Component = component;
            entry.m_Field = field;
            entry.m_Type = Kind;
            entry.m_Bounds = bounds;
            entry.m_Has = [](Entity e)
            { return e.HasComponent<C>(); };
            entry.m_Read = [](Entity e) -> PinValue
            {
                return PinBridge<Kind>::template Read<MemberT>(AccessT{}(e.GetComponent<C>()));
            };
            entry.m_Write = [](Entity e, const PinValue& value, const FieldBounds& b) -> FieldWriteResult
            {
                return PinBridge<Kind>::template Write<MemberT>(AccessT{}(e.GetComponent<C>()), value, b);
            };
            return entry;
        }

// The spelling the generated .inl uses. `Access` is a member-ACCESS CHAIN, not a
// single name — `Translation`, or `HeightShaping.IslandFalloff` — pasted into the
// accessor lambda. `Kind` is a bare PinType enumerator name, so a wrong one from
// the generator is a compile error here rather than a wrong pin at runtime.
#define OLO_VSF_FIELD(Comp, Key, Access, Kind)                                                 \
    ::OloEngine::VisualScript::MakeFieldEntry<Comp, ::OloEngine::VisualScript::PinType::Kind>( \
        #Comp, Key, [](Comp& c) -> auto& { return c.Access; })

#define OLO_VSF_FIELD_RANGE(Comp, Key, Access, Kind, Min, Max)                                 \
    ::OloEngine::VisualScript::MakeFieldEntry<Comp, ::OloEngine::VisualScript::PinType::Kind>( \
        #Comp, Key, [](Comp& c) -> auto& { return c.Access; },                                 \
        ::OloEngine::VisualScript::FieldBounds{ Min, Max })

#define OLO_VSF_BOUND(expr)               \
    ::OloEngine::VisualScript::FieldBound \
    {                                     \
        true, static_cast<f64>(expr)      \
    }
#define OLO_VSF_NO_BOUND \
    ::OloEngine::VisualScript::FieldBound {}

#include "OloEngine/Scripting/VisualScript/Generated/ComponentFieldRegistry.Generated.inl"

#undef OLO_VSF_FIELD
#undef OLO_VSF_FIELD_RANGE
#undef OLO_VSF_BOUND
#undef OLO_VSF_NO_BOUND

        struct Registry
        {
            std::vector<ComponentFieldEntry> m_Entries;
            /// "Component.Field" -> index. Built over the sorted vector, so the
            /// span All() hands out stays the presentation order while lookup is
            /// still O(1).
            std::unordered_map<std::string, sizet> m_ByKey;
            std::vector<std::string> m_ComponentNames;
        };

        const Registry& GetRegistry()
        {
            static const Registry s_Registry = []
            {
                Registry registry;
                BuildRegistryChunks(registry.m_Entries);

                std::ranges::sort(registry.m_Entries,
                                  [](const ComponentFieldEntry& a, const ComponentFieldEntry& b)
                                  {
                                      if (a.m_Component != b.m_Component)
                                          return a.m_Component < b.m_Component;
                                      return a.m_Field < b.m_Field;
                                  });

                registry.m_ByKey.reserve(registry.m_Entries.size());
                for (sizet i = 0; i < registry.m_Entries.size(); ++i)
                {
                    const ComponentFieldEntry& entry = registry.m_Entries[i];
                    registry.m_ByKey.emplace(entry.m_Component + "." + entry.m_Field, i);
                    if (registry.m_ComponentNames.empty() || registry.m_ComponentNames.back() != entry.m_Component)
                        registry.m_ComponentNames.push_back(entry.m_Component);
                }
                return registry;
            }();
            return s_Registry;
        }

        /// Resolve the two accepted component spellings to the struct name, or an
        /// empty string. See ComponentFieldRegistry::Find for why both are taken.
        std::string ResolveComponentName(const Registry& registry, std::string_view component)
        {
            std::string name(component);
            if (std::ranges::binary_search(registry.m_ComponentNames, name))
                return name;
            name += "Component";
            if (std::ranges::binary_search(registry.m_ComponentNames, name))
                return name;
            return {};
        }
    } // namespace

    std::span<const ComponentFieldEntry> ComponentFieldRegistry::All()
    {
        return GetRegistry().m_Entries;
    }

    const ComponentFieldEntry* ComponentFieldRegistry::Find(std::string_view component, std::string_view field)
    {
        const Registry& registry = GetRegistry();
        const std::string resolved = ResolveComponentName(registry, component);
        if (resolved.empty())
            return nullptr;

        const auto it = registry.m_ByKey.find(resolved + "." + std::string(field));
        return it == registry.m_ByKey.end() ? nullptr : &registry.m_Entries[it->second];
    }

    const std::vector<std::string>& ComponentFieldRegistry::ComponentNames()
    {
        return GetRegistry().m_ComponentNames;
    }

    std::vector<const ComponentFieldEntry*> ComponentFieldRegistry::FieldsOf(std::string_view component)
    {
        const Registry& registry = GetRegistry();
        const std::string resolved = ResolveComponentName(registry, component);
        if (resolved.empty())
            return {};

        std::vector<const ComponentFieldEntry*> result;
        for (const ComponentFieldEntry& entry : registry.m_Entries)
        {
            if (entry.m_Component == resolved)
                result.push_back(&entry);
        }
        return result;
    }

} // namespace OloEngine::VisualScript
