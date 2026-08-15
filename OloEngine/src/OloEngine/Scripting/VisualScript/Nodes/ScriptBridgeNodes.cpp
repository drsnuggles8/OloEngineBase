#include "OloEnginePCH.h"
#include "NodeBuilders.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"

#include <sol/sol.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace OloEngine
{
    namespace Scripting
    {
        // Defined in LuaScriptEngine.cpp; null until LuaScriptEngine::Init runs.
        extern sol::state* GetState();
    } // namespace Scripting
} // namespace OloEngine

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // Resolve "a.b.c" against the Lua globals table without creating the
        // intermediate tables sol2's operator[] chain would.
        sol::protected_function ResolveLuaFunction(sol::state& lua, const std::string& path)
        {
            sol::object current = lua.globals();
            sizet start = 0;
            while (start <= path.size())
            {
                const sizet dot = path.find('.', start);
                const std::string segment = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                if (segment.empty() || current.get_type() != sol::type::table)
                {
                    return {};
                }
                current = current.as<sol::table>().raw_get<sol::object>(segment);
                if (dot == std::string::npos)
                {
                    break;
                }
                start = dot + 1;
            }
            if (!current.valid() || current.get_type() != sol::type::function)
            {
                return {};
            }
            return current.as<sol::protected_function>();
        }

        // PinValue -> Lua. Entity/Asset ids are full-range u64 and Lua's integer
        // is SIGNED 64-bit, so they go across as the same bit pattern reinterpreted
        // — the SOL_ALL_INTEGER_VALUES_FIT contract documented in
        // docs/agent-rules/script-structural-command-safe-point.md.
        sol::object ToLua(sol::state& lua, const PinValue& value)
        {
            switch (value.GetType())
            {
                case PinType::Bool:
                    return sol::make_object(lua, value.AsBool());
                case PinType::Int:
                    return sol::make_object(lua, value.AsInt());
                case PinType::Float:
                    return sol::make_object(lua, static_cast<f64>(value.AsFloat()));
                case PinType::Entity:
                case PinType::Asset:
                    return sol::make_object(lua, static_cast<i64>(static_cast<u64>(value.AsEntity())));
                case PinType::Vec2:
                case PinType::Vec3:
                case PinType::Vec4:
                case PinType::String:
                    return sol::make_object(lua, value.AsString());
                case PinType::Exec:
                case PinType::Any:
                    break;
            }
            return sol::lua_nil;
        }

        PinValue FromLua(const sol::object& object)
        {
            if (!object.valid())
            {
                return {};
            }
            switch (object.get_type())
            {
                case sol::type::boolean:
                    return PinValue::MakeBool(object.as<bool>());
                case sol::type::number:
                {
                    const f64 raw = object.as<f64>();
                    // Every cast below is guarded first: converting a floating value
                    // that does not fit the destination is undefined behaviour, not
                    // a saturating or wrapping result, and Lua reaches this with
                    // whatever a script computed — inf, NaN and 1e300 included.
                    if (!std::isfinite(raw))
                    {
                        return {};
                    }

                    // Lua has one number type; keep integral results integral so a
                    // count wired into an Int pin does not round-trip through f32.
                    // fpclassify rather than `fraction == 0.0`: comparing floats
                    // with == is banned repo-wide (CLAUDE.md -> Conventions).
                    f64 integral = 0.0;
                    const f64 fraction = std::modf(raw, &integral);
                    // Bounds are the exact powers of two either side of i64, so both
                    // comparisons are exact in f64. 2^63 itself is NOT representable
                    // as i64, hence the strict upper bound.
                    constexpr f64 kIntMin = -9223372036854775808.0; // -2^63
                    constexpr f64 kIntMax = 9223372036854775808.0;  //  2^63
                    if (std::fpclassify(fraction) == FP_ZERO && integral >= kIntMin && integral < kIntMax)
                    {
                        return PinValue::MakeInt(static_cast<i64>(integral));
                    }

                    if (std::abs(raw) > static_cast<f64>(std::numeric_limits<f32>::max()))
                    {
                        return {};
                    }
                    return PinValue::MakeFloat(static_cast<f32>(raw));
                }
                case sol::type::string:
                    return PinValue::MakeString(object.as<std::string>());
                default:
                    return {};
            }
        }
    } // namespace

    void RegisterScriptBridgeNodes(NodeRegistry& registry)
    {
        RegisterExecNode(registry, "Script.CallLuaFunction", "Call Lua Function", "Scripting",
                         "Calls a global Lua function by dotted path, passing one argument and reading one result.",
                         { ExecIn(), In("Function", PinValue::MakeString({})), In("Argument", PinType::Any),
                           ExecOut(), Out("Result", PinType::Any), Out("Succeeded", PinType::Bool) },
                         [](NodeContext& ctx)
                         {
                             ctx.SetOutput(5, PinValue::MakeBool(false));

                             sol::state* lua = Scripting::GetState();
                             const std::string path = ctx.GetInputString(1);
                             if (lua == nullptr || path.empty())
                             {
                                 ctx.Error("Call Lua Function needs an initialised Lua engine and a function name");
                                 ctx.Trigger(3);
                                 return;
                             }

                             sol::protected_function function = ResolveLuaFunction(*lua, path);
                             if (!function.valid())
                             {
                                 ctx.Error("Lua function '" + path + "' does not exist");
                                 ctx.Trigger(3);
                                 return;
                             }

                             // protected_function, never plain function: a Lua
                             // error inside an authored graph must not longjmp
                             // through the VM's C++ frames.
                             sol::protected_function_result result = function(ToLua(*lua, ctx.GetInput(2)));
                             if (!result.valid())
                             {
                                 const sol::error error = result;
                                 ctx.Error(std::string("Lua error in '") + path + "': " + error.what());
                                 ctx.Trigger(3);
                                 return;
                             }

                             ctx.SetOutput(4, result.return_count() > 0 ? FromLua(result.get<sol::object>(0)) : PinValue{});
                             ctx.SetOutput(5, PinValue::MakeBool(true));
                             ctx.Trigger(3);
                         });

        RegisterExecNode(registry, "Script.CallCSharpMethod", "Call C# Method", "Scripting",
                         "Invokes a static method on a managed class, passing one float argument.",
                         { ExecIn(), In("Class", PinValue::MakeString({})), In("Method", PinValue::MakeString({})),
                           In("Argument", PinValue::MakeFloat(0.0f)), ExecOut(), Out("Succeeded", PinType::Bool) },
                         [](NodeContext& ctx)
                         {
                             ctx.SetOutput(5, PinValue::MakeBool(false));

                             const std::string className = ctx.GetInputString(1);
                             const std::string methodName = ctx.GetInputString(2);
                             if (className.empty() || methodName.empty())
                             {
                                 ctx.Error("Call C# Method needs both a class and a method name");
                                 ctx.Trigger(4);
                                 return;
                             }

                             // GetEntityClass returns null when the C# engine is
                             // not initialised at all (headless, or a build with
                             // scripting disabled) — the same graceful degradation
                             // ScriptEngine::Init already logs.
                             Ref<ScriptClass> scriptClass = ScriptEngine::GetEntityClass(className);
                             if (!scriptClass)
                             {
                                 ctx.Error("C# class '" + className + "' is not loaded");
                                 ctx.Trigger(4);
                                 return;
                             }
                             MonoMethod* method = scriptClass->GetMethod(methodName, 1);
                             if (method == nullptr)
                             {
                                 ctx.Error("C# method '" + className + "." + methodName + "(float)' does not exist");
                                 ctx.Trigger(4);
                                 return;
                             }

                             // Instance is nullptr below, so the method MUST be static —
                             // mono_runtime_invoke with a null `this` on an instance
                             // method is undefined behaviour, not a catchable error.
                             if (!ScriptClass::IsMethodStatic(method))
                             {
                                 ctx.Error("C# method '" + className + "." + methodName + "' must be static");
                                 ctx.Trigger(4);
                                 return;
                             }

                             // GetMethod matched on ARITY alone, so `void F(string)`
                             // is still sitting here. Passing &argument to it makes
                             // mono reinterpret an f32* as a MonoString* — memory
                             // corruption, not a catchable managed error.
                             if (!ScriptClass::IsMethodSingleFloatParameter(method))
                             {
                                 ctx.Error("C# method '" + className + "." + methodName + "' must take exactly one float parameter");
                                 ctx.Trigger(4);
                                 return;
                             }

                             f32 argument = ctx.GetInputFloat(3);
                             void* params[] = { &argument };
                             bool threw = false;
                             (void)scriptClass->InvokeMethod(nullptr, method, params, &threw);
                             // Succeeded reflects whether the managed call actually
                             // completed. The returned MonoObject* cannot: a void
                             // method returns nullptr whether or not it threw.
                             ctx.SetOutput(5, PinValue::MakeBool(!threw));
                             ctx.Trigger(4);
                         });
    }

} // namespace OloEngine::VisualScript
