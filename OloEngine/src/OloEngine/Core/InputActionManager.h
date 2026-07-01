#pragma once

#include "OloEngine/Core/IInputProvider.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/TransparentStringHash.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // StringHash / StringEqual (transparent heterogeneous lookup) now live in
    // Core/TransparentStringHash.h so other subsystems can reuse them.

    // Named input contexts. Exactly one is active at a time — the top of the
    // context stack. Each context owns its own InputActionMap, so switching
    // contexts changes which actions drive IsActionPressed / GetActionAxisValue
    // without manual unbinding. Use SetInputContext for a hard switch, or
    // PushContext / PopContext to nest (e.g. push Menu over Gameplay, pop to
    // restore). The first Update() after the active context changes suppresses
    // just-pressed/just-released, so a key still held from the previous context
    // (e.g. the Escape that opened a menu) doesn't immediately fire a same-key
    // action in the new context.
    enum class InputContextType : u8
    {
        Gameplay,
        Menu,
        Vehicle,
        Custom
    };

    // The full set of input contexts, in enum order. Used for deterministic
    // iteration when serializing every context's map and for the editor selector.
    inline constexpr std::array<InputContextType, 4> AllInputContextTypes{ InputContextType::Gameplay, InputContextType::Menu, InputContextType::Vehicle,
                                                                           InputContextType::Custom };

    // Stable string names for the contexts — used as YAML keys when persisting
    // per-context action maps. Round-trips via StringToInputContextType.
    [[nodiscard]] const char* InputContextTypeToString(InputContextType ctx);
    [[nodiscard]] std::optional<InputContextType> StringToInputContextType(std::string_view str);

    class InputActionManager
    {
      public:
        static void Init();
        static void Shutdown();

        // Call once per frame before layer updates to refresh action states.
        static void Update();

        // Query methods — operate on the active context's map. Return false for unknown action names.
        [[nodiscard]] static bool IsActionPressed(std::string_view actionName);
        [[nodiscard]] static bool IsActionJustPressed(std::string_view actionName);
        [[nodiscard]] static bool IsActionJustReleased(std::string_view actionName);

        // Returns the analog value for the first GamepadAxis binding in the action.
        // Keyboard/mouse bindings return 0/1; gamepad axis bindings return the raw axis value.
        [[nodiscard]] static f32 GetActionAxisValue(std::string_view actionName);

        // --- Action-map contexts (gameplay / menu / vehicle / custom) ---

        // Hard switch: collapse the context stack to a single entry and activate ctx.
        // A no-op (preserving cached state) when ctx is already the sole active context.
        static void SetInputContext(InputContextType ctx);

        // Push ctx onto the context stack, making it active. The context beneath
        // stays in place and its map is restored on the matching PopContext.
        static void PushContext(InputContextType ctx);

        // Pop the active context, restoring the one beneath. Never pops the base
        // context (returns false and leaves the stack untouched when at depth 1).
        static bool PopContext();

        [[nodiscard]] static InputContextType GetInputContext()
        {
            return s_ContextStack.back();
        }

        // Number of contexts currently on the stack (always >= 1 after Init).
        [[nodiscard]] static sizet GetContextDepth()
        {
            return s_ContextStack.size();
        }

        // Replace the active context's action map and reset all cached state.
        static void SetActionMap(const InputActionMap& map);

        // Replace a specific context's action map. If ctx is the active context this
        // also resets cached state; otherwise the map is stored until ctx is activated.
        static void SetActionMap(InputContextType ctx, const InputActionMap& map);

        // Inject a custom input provider (for testing). Passing nullptr restores the default.
        static void SetInputProvider(IInputProvider* provider);

        // Mutable access to the active context's map for editor panel mutation.
        // Stale state is pruned in Update().
        [[nodiscard]] static InputActionMap& GetActionMap()
        {
            return ActiveMap();
        }
        [[nodiscard]] static const InputActionMap& GetActionMapConst()
        {
            return ActiveMap();
        }

        // Read-only access to any context's map. A context that was never set reads
        // back as a shared empty map WITHOUT being inserted, so a read never
        // fabricates a context (use GetActionMapMutable to author one).
        [[nodiscard]] static const InputActionMap& GetActionMap(InputContextType ctx)
        {
            static const InputActionMap s_Empty;
            auto it = s_ContextMaps.find(ctx);
            return it != s_ContextMaps.end() ? it->second : s_Empty;
        }

        // Mutable access to a specific context's map for editor authoring. Unlike the
        // read-only overload this DOES create the context if absent (editing it implies
        // authoring it). Does not change the active context.
        [[nodiscard]] static InputActionMap& GetActionMapMutable(InputContextType ctx)
        {
            return s_ContextMaps[ctx];
        }

        // All context maps that currently exist, keyed by context. Only contexts that
        // have been set or authored are present. Used to persist every context at once.
        [[nodiscard]] static const std::unordered_map<InputContextType, InputActionMap>& GetAllContextMaps()
        {
            return s_ContextMaps;
        }

        // Replace ALL context maps wholesale (used when loading a project / reverting to
        // disk). Clears any contexts not in `maps` so state can't leak across projects,
        // and resets cached press/axis state since the active map may have changed.
        static void ReplaceAllContextMaps(const std::unordered_map<InputContextType, InputActionMap>& maps);

      private:
        // The active context is always the top of the stack; its map lives in s_ContextMaps.
        [[nodiscard]] static InputActionMap& ActiveMap()
        {
            return s_ContextMaps[s_ContextStack.back()];
        }

        // Clear cached press/axis state and suppress just-pressed/just-released on the
        // next Update() — used whenever the active context (and thus its map) changes.
        static void ResetStateForContextChange();

        // Stack of active contexts; back() is active. Seeded with Gameplay so queries
        // are valid even before Init(). Never emptied (PopContext keeps the base).
        inline static std::vector<InputContextType> s_ContextStack{ InputContextType::Gameplay };
        // One action map per context, keyed by the enum — stack duplicates share one map.
        inline static std::unordered_map<InputContextType, InputActionMap> s_ContextMaps;
        // Set on a context change; the next Update() forces previous == current so no
        // action fires just-pressed/just-released on the first frame of the new context.
        inline static bool s_SuppressTransientOnNextUpdate = false;
        inline static std::unordered_map<std::string, bool, StringHash, StringEqual> s_CurrentState;
        inline static std::unordered_map<std::string, bool, StringHash, StringEqual> s_PreviousState;
        inline static std::unordered_map<std::string, f32, StringHash, StringEqual> s_AxisValues;
        static IInputProvider* s_InputProvider;
    };

} // namespace OloEngine
