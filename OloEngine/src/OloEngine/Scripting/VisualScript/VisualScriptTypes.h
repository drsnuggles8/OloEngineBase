#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <variant>

namespace OloEngine::VisualScript
{
    //==============================================================================
    // The load-bearing contract (issue #634).
    //
    // Every other part of the visual-scripting stack is a consumer of the types in
    // this header: the compiled plan is built from PinType/PinValue, the node
    // library declares all of its signatures in terms of them, the YAML serializer
    // round-trips them, the blackboard IS a PinValue table, and the Lua/C# bridges
    // marshal through them. Changing PinType breaks all of those at once — see
    // docs/adr/0014-visual-script-execution-model.md before widening it.
    //==============================================================================

    /// Graph-local, dense identifiers. Stable within one asset (persisted in YAML),
    /// NOT stable across assets — never use one as a global key.
    using NodeId = u32;
    using LinkId = u32;

    inline constexpr NodeId kInvalidNodeId = 0;
    inline constexpr LinkId kInvalidLinkId = 0;

    //-- Guarded numeric narrowing -------------------------------------------------
    // Every value crossing into a graph arrives unvalidated — a Lua script's
    // arithmetic, a YAML scalar someone hand-edited, a C# float. Narrowing a
    // floating value that does not fit the destination is UNDEFINED BEHAVIOUR, not
    // a saturating or wrapping result, so both directions get a checked helper
    // rather than a bare static_cast at each of the (many) call sites.

    /// True when `value` is finite AND within f32's range, i.e. `static_cast<f32>`
    /// is defined for it. Callers refuse rather than clamp: an infinity written
    /// into a blackboard variable poisons every node downstream of it, silently.
    [[nodiscard]] bool IsRepresentableAsFloat(f64 value) noexcept;

    /// Truncates toward zero like the cast it replaces, but yields 0 for anything
    /// non-finite or outside i64. A finite f32 reaches ~3.4e38 against i64's
    /// ~9.2e18, so "it was finite" is nowhere near enough of a guard.
    [[nodiscard]] i64 IntFromFloat(f64 value) noexcept;

    /// A pin's value domain.
    ///
    /// `Exec` is a first-class member rather than a separate flag on purpose: link
    /// validation, wire rendering and the compiler all branch on exactly one enum,
    /// so an exec pin can never be silently wired to a data pin.
    ///
    /// `Any` is an authoring-time wildcard used by nodes whose signature is decided
    /// by what is connected (Print, the variable accessors before a variable is
    /// chosen). It is never the type of a *value* — resolution happens at compile
    /// time and a still-unresolved `Any` input falls back to its literal default.
    enum class PinType : u8
    {
        Exec = 0,
        Bool,
        Int,
        Float,
        Vec2,
        Vec3,
        Vec4,
        String,
        Entity,
        Asset,
        Any,
    };

    enum class PinDirection : u8
    {
        Input = 0,
        Output,
    };

    [[nodiscard]] constexpr bool IsExecPin(PinType type) noexcept
    {
        return type == PinType::Exec;
    }

    [[nodiscard]] constexpr bool IsDataPin(PinType type) noexcept
    {
        return type != PinType::Exec;
    }

    [[nodiscard]] const char* PinTypeToString(PinType type) noexcept;
    [[nodiscard]] PinType PinTypeFromString(std::string_view name) noexcept;

    //==============================================================================
    /// A typed pin value: a literal default, a blackboard variable's contents, or a
    /// value in flight between two nodes.
    ///
    /// Entity and Asset both store a `u64` (both are UUID underneath) and are told
    /// apart by `m_Type` rather than by the variant alternative — putting two UUID
    /// alternatives in one std::variant would make every construction ambiguous.
    class PinValue
    {
      public:
        PinValue() = default;

        [[nodiscard]] PinType GetType() const noexcept
        {
            return m_Type;
        }

        //-- Factories -------------------------------------------------------------
        [[nodiscard]] static PinValue MakeExec();
        [[nodiscard]] static PinValue MakeBool(bool value);
        [[nodiscard]] static PinValue MakeInt(i64 value);
        [[nodiscard]] static PinValue MakeFloat(f32 value);
        [[nodiscard]] static PinValue MakeVec2(const glm::vec2& value);
        [[nodiscard]] static PinValue MakeVec3(const glm::vec3& value);
        [[nodiscard]] static PinValue MakeVec4(const glm::vec4& value);
        [[nodiscard]] static PinValue MakeString(std::string value);
        [[nodiscard]] static PinValue MakeEntity(UUID value);
        [[nodiscard]] static PinValue MakeAsset(UUID value);

        /// The zero value for `type` (`Any`/`Exec` produce a default-constructed,
        /// type-carrying value). Used for unconnected pins and fresh variables.
        [[nodiscard]] static PinValue DefaultFor(PinType type);

        //-- Accessors -------------------------------------------------------------
        // Each coerces where the conversion is lossless-or-documented (see
        // ConvertTo) and otherwise returns the zero value, so a node body never has
        // to guard. Coercion is deliberate: a graph author wiring an Int into a
        // Float pin is doing the obvious thing, and refusing it at runtime would be
        // an invisible no-op instead of an authoring-time error.
        [[nodiscard]] bool AsBool() const noexcept;
        [[nodiscard]] i64 AsInt() const noexcept;
        [[nodiscard]] f32 AsFloat() const noexcept;
        [[nodiscard]] glm::vec2 AsVec2() const noexcept;
        [[nodiscard]] glm::vec3 AsVec3() const noexcept;
        [[nodiscard]] glm::vec4 AsVec4() const noexcept;
        [[nodiscard]] std::string AsString() const;
        [[nodiscard]] UUID AsEntity() const noexcept;
        [[nodiscard]] UUID AsAsset() const noexcept;

        /// Reinterpret this value as `type`. Numeric types inter-convert; anything
        /// can be read as String (its display form); String parses back into the
        /// numeric/vector types. An impossible conversion yields DefaultFor(type).
        [[nodiscard]] PinValue ConvertTo(PinType type) const;

        /// Round-trippable text form used by the YAML serializer and by Print.
        /// Floats are emitted with enough digits to survive save → load → save.
        [[nodiscard]] std::string ToStorageString() const;
        [[nodiscard]] static PinValue FromStorageString(PinType type, std::string_view text);

        /// True when every float component is finite. Every value read from YAML,
        /// a save-game or a script goes through this before entering a graph — a
        /// NaN in a blackboard variable otherwise poisons every downstream node
        /// silently, and NaN comparisons make Branch take neither obvious side.
        [[nodiscard]] bool IsFinite() const noexcept;

        /// Replace non-finite float components with 0. Returns true when something
        /// was actually clamped, so callers can log once.
        bool SanitizeNonFinite();

        [[nodiscard]] bool operator==(const PinValue& other) const;

      private:
        using Storage = std::variant<std::monostate, bool, i64, f32, glm::vec2, glm::vec3, glm::vec4, std::string, u64>;

        PinType m_Type = PinType::Exec;
        Storage m_Storage{};
    };

    //==============================================================================
    /// One pin on a node type. The registry owns the canonical list; the compiler
    /// resolves link endpoints (serialized by NAME) against it, so inserting a pin
    /// into the middle of a node's signature does not silently rewire old graphs.
    struct PinDescriptor
    {
        std::string m_Name;
        PinType m_Type = PinType::Exec;
        PinDirection m_Direction = PinDirection::Input;
        PinValue m_DefaultValue{};

        PinDescriptor() = default;
        PinDescriptor(std::string name, PinType type, PinDirection direction)
            : m_Name(std::move(name)), m_Type(type), m_Direction(direction), m_DefaultValue(PinValue::DefaultFor(type))
        {
        }
        PinDescriptor(std::string name, PinType type, PinDirection direction, PinValue defaultValue)
            : m_Name(std::move(name)), m_Type(type), m_Direction(direction), m_DefaultValue(std::move(defaultValue))
        {
        }
    };

    //==============================================================================
    /// Whether a link between two pins is legal, and (for data links) whether it
    /// needs a coercion. Shared by the compiler and — once it exists — the editor
    /// canvas's link-drag feedback, so the two can never disagree.
    enum class LinkCompatibility : u8
    {
        Incompatible = 0,
        Exact,
        Coerced,
    };

    [[nodiscard]] LinkCompatibility CheckLinkCompatibility(PinType source, PinType target) noexcept;

} // namespace OloEngine::VisualScript
