#include "OloEnginePCH.h"
#include "NodeBuilders.h"

#include <charconv>
#include <string>
#include <system_error>

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // Substitutes {0}/{1}/{2} in `format`. Deliberately not fmt::format: the
        // format string is authored data, and fmt throws (or, worse, is a
        // compile-time-checked API misused at runtime) on a malformed one. An
        // unmatched brace here is simply copied through.
        std::string Substitute(const std::string& format, const std::vector<std::string>& args)
        {
            std::string result;
            result.reserve(format.size() + 16);
            for (sizet i = 0; i < format.size(); ++i)
            {
                if (format[i] != '{')
                {
                    result += format[i];
                    continue;
                }
                const sizet close = format.find('}', i);
                if (close == std::string::npos)
                {
                    result += format[i];
                    continue;
                }
                const std::string token = format.substr(i + 1, close - i - 1);
                bool consumed = false;
                // from_chars, not stoul: the format string is authored data and
                // stoul THROWS on a token too large to fit, which would take out
                // the whole tick from inside a node body.
                u32 index = 0;
                if (std::from_chars(token.data(), token.data() + token.size(), index).ec == std::errc{} && index < args.size())
                {
                    result += args[index];
                    consumed = true;
                }
                if (!consumed)
                {
                    result += format.substr(i, close - i + 1);
                }
                i = close;
            }
            return result;
        }
    } // namespace

    void RegisterUtilityNodes(NodeRegistry& registry)
    {
        RegisterExecNode(registry, "Utility.Print", "Print String", "Utility",
                         "Writes Message to the engine log and to the graph log the debugger reads.",
                         { ExecIn(), In("Message", PinValue::MakeString({})), ExecOut() },
                         [](NodeContext& ctx)
                         {
                             ctx.Log(ctx.GetInputString(1));
                             ctx.Trigger(2);
                         });

        RegisterPureNode(registry, "Utility.ToString", "To String", "Utility", "Renders any value as text.",
                         { In("Value", PinType::Any), Out("Result", PinType::String) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeString(ctx.GetInput(0).AsString())); });

        RegisterPureNode(registry, "Utility.Format", "Format Text", "Utility",
                         "Substitutes {0}, {1} and {2} in Format with A, B and C.",
                         { In("Format", PinValue::MakeString({})), In("A", PinType::Any), In("B", PinType::Any),
                           In("C", PinType::Any), Out("Result", PinType::String) },
                         [](NodeContext& ctx)
                         {
                             const std::vector<std::string> args{ ctx.GetInput(1).AsString(), ctx.GetInput(2).AsString(), ctx.GetInput(3).AsString() };
                             ctx.SetOutput(4, PinValue::MakeString(Substitute(ctx.GetInputString(0), args)));
                         });

        RegisterExecNode(registry, "Utility.PublishEvent", "Publish Event", "Utility",
                         "Queues a named event. Delivered after the visual-script system finishes its pass, so it "
                         "never fires inside an entity iteration.",
                         { ExecIn(), In("Event Name", PinValue::MakeString({})), In("Payload", PinType::Any),
                           In("Target", PinType::Entity), In("To Gameplay Bus", PinValue::MakeBool(false)), ExecOut() },
                         [](NodeContext& ctx)
                         {
                             const std::string name = ctx.GetInputString(1);
                             if (name.empty())
                             {
                                 ctx.Error("Publish Event with an empty name would reach nothing");
                                 ctx.Trigger(5);
                                 return;
                             }
                             EmittedEvent event;
                             event.m_Name = name;
                             event.m_Payload = ctx.GetInput(2);
                             // An unwired Target broadcasts. GetInputEntity would
                             // substitute "me", which would make the common
                             // "tell everyone" case silently self-addressed.
                             event.m_Target = ctx.IsInputConnected(3) ? ctx.GetInput(3).AsEntity() : UUID(0);
                             event.m_PublishToBus = ctx.GetInputBool(4);
                             ctx.Emit(std::move(event));
                             ctx.Trigger(5);
                         });

        RegisterExecNode(registry, "Utility.Branch On String", "Branch On String", "Utility",
                         "Takes Match when Value equals Compare, otherwise No Match.",
                         { ExecIn(), In("Value", PinValue::MakeString({})), In("Compare", PinValue::MakeString({})),
                           ExecOut("Match"), ExecOut("No Match") },
                         [](NodeContext& ctx)
                         { ctx.Trigger(ctx.GetInputString(1) == ctx.GetInputString(2) ? 3 : 4); });
    }

} // namespace OloEngine::VisualScript
