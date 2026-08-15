#include "OloEnginePCH.h"
#include "NodeBuilders.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // Division by (near) zero yields 0 rather than an infinity. An inf that
        // reaches a transform write is a whole-frame NaN cascade, and every
        // downstream node would then read garbage with nothing in the log.
        constexpr f32 kDivideEpsilon = 1e-9f;

        f32 SafeDivide(f32 a, f32 b)
        {
            return std::fabs(b) < kDivideEpsilon ? 0.0f : a / b;
        }
    } // namespace

    void RegisterMathNodes(NodeRegistry& registry)
    {
        //-- Scalar arithmetic ----------------------------------------------------
        RegisterFloatBinary(registry, "Math.Add", "Add", "A + B", [](f32 a, f32 b)
                            { return a + b; });
        RegisterFloatBinary(registry, "Math.Subtract", "Subtract", "A - B", [](f32 a, f32 b)
                            { return a - b; });
        RegisterFloatBinary(registry, "Math.Multiply", "Multiply", "A * B", [](f32 a, f32 b)
                            { return a * b; });
        RegisterFloatBinary(registry, "Math.Divide", "Divide", "A / B (0 when B is ~0)", SafeDivide);
        RegisterFloatBinary(registry, "Math.Min", "Min", "The smaller of A and B", [](f32 a, f32 b)
                            { return std::min(a, b); });
        RegisterFloatBinary(registry, "Math.Max", "Max", "The larger of A and B", [](f32 a, f32 b)
                            { return std::max(a, b); });
        RegisterFloatBinary(registry, "Math.Pow", "Power", "A raised to B", [](f32 a, f32 b)
                            {
                                const f32 result = std::pow(a, b);
                                return std::isfinite(result) ? result : 0.0f; });

        RegisterPureNode(registry, "Math.Abs", "Absolute", "Math", "The magnitude of A",
                         { In("A", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeFloat(std::fabs(ctx.GetInputFloat(0)))); });

        RegisterPureNode(registry, "Math.Negate", "Negate", "Math", "-A",
                         { In("A", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeFloat(-ctx.GetInputFloat(0))); });

        RegisterPureNode(registry, "Math.Sqrt", "Square Root", "Math", "The square root of A (0 for negatives)",
                         { In("A", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         {
                             const f32 a = ctx.GetInputFloat(0);
                             ctx.SetOutput(1, PinValue::MakeFloat(a <= 0.0f ? 0.0f : std::sqrt(a)));
                         });

        RegisterPureNode(registry, "Math.Sin", "Sine", "Math", "sin(A), A in radians",
                         { In("A", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeFloat(std::sin(ctx.GetInputFloat(0)))); });

        RegisterPureNode(registry, "Math.Cos", "Cosine", "Math", "cos(A), A in radians",
                         { In("A", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeFloat(std::cos(ctx.GetInputFloat(0)))); });

        RegisterPureNode(registry, "Math.Clamp", "Clamp", "Math", "A limited to [Min, Max]",
                         { In("A", PinValue::MakeFloat(0.0f)), In("Min", PinValue::MakeFloat(0.0f)),
                           In("Max", PinValue::MakeFloat(1.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         {
                             const f32 low = ctx.GetInputFloat(1);
                             const f32 high = ctx.GetInputFloat(2);
                             // An inverted range is an authoring mistake, not a
                             // reason to hit std::clamp's precondition (UB).
                             ctx.SetOutput(3, PinValue::MakeFloat(std::clamp(ctx.GetInputFloat(0), std::min(low, high), std::max(low, high))));
                         });

        RegisterPureNode(registry, "Math.Lerp", "Lerp", "Math", "Linear blend from A to B by Alpha",
                         { In("A", PinValue::MakeFloat(0.0f)), In("B", PinValue::MakeFloat(1.0f)),
                           In("Alpha", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         {
                             const f32 a = ctx.GetInputFloat(0);
                             const f32 b = ctx.GetInputFloat(1);
                             const f32 t = ctx.GetInputFloat(2);
                             ctx.SetOutput(3, PinValue::MakeFloat(a + (b - a) * t));
                         });

        //-- Integer arithmetic ---------------------------------------------------
        RegisterPureNode(registry, "Math.IntAdd", "Add (Integer)", "Math", "A + B on integers",
                         { In("A", PinValue::MakeInt(0)), In("B", PinValue::MakeInt(0)), Out("Result", PinType::Int) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeInt(ctx.GetInputInt(0) + ctx.GetInputInt(1))); });

        RegisterPureNode(registry, "Math.IntSubtract", "Subtract (Integer)", "Math", "A - B on integers",
                         { In("A", PinValue::MakeInt(0)), In("B", PinValue::MakeInt(0)), Out("Result", PinType::Int) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeInt(ctx.GetInputInt(0) - ctx.GetInputInt(1))); });

        RegisterPureNode(registry, "Math.Modulo", "Modulo", "Math", "A modulo B (0 when B is 0)",
                         { In("A", PinValue::MakeInt(0)), In("B", PinValue::MakeInt(1)), Out("Result", PinType::Int) },
                         [](NodeContext& ctx)
                         {
                             const i64 b = ctx.GetInputInt(1);
                             // b == 0 is integer-division UB; b == -1 with
                             // INT64_MIN overflows. Both are one guard.
                             ctx.SetOutput(2, PinValue::MakeInt((b == 0 || b == -1) ? 0 : ctx.GetInputInt(0) % b));
                         });

        //-- Random (deterministic, seeded per scene) -----------------------------
        RegisterPureNode(registry, "Math.RandomFloat", "Random Float", "Math", "A uniform value in [Min, Max)",
                         { In("Min", PinValue::MakeFloat(0.0f)), In("Max", PinValue::MakeFloat(1.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         {
                             const f32 low = ctx.GetInputFloat(0);
                             const f32 high = ctx.GetInputFloat(1);
                             ctx.SetOutput(2, PinValue::MakeFloat(low + (high - low) * ctx.NextRandom()));
                         });

        RegisterPureNode(registry, "Math.RandomInt", "Random Integer", "Math", "A uniform integer in [Min, Max]",
                         { In("Min", PinValue::MakeInt(0)), In("Max", PinValue::MakeInt(1)), Out("Result", PinType::Int) },
                         [](NodeContext& ctx)
                         {
                             const i64 low = ctx.GetInputInt(0);
                             const i64 high = ctx.GetInputInt(1);
                             if (high <= low)
                             {
                                 ctx.SetOutput(2, PinValue::MakeInt(low));
                                 return;
                             }
                             const i64 span = high - low + 1;
                             const i64 offset = static_cast<i64>(ctx.NextRandom() * static_cast<f32>(span));
                             ctx.SetOutput(2, PinValue::MakeInt(low + std::min(offset, span - 1)));
                         });

        //-- Logic ----------------------------------------------------------------
        RegisterPureNode(registry, "Logic.And", "And", "Logic", "A AND B",
                         { In("A", PinValue::MakeBool(false)), In("B", PinValue::MakeBool(false)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeBool(ctx.GetInputBool(0) && ctx.GetInputBool(1))); });

        RegisterPureNode(registry, "Logic.Or", "Or", "Logic", "A OR B",
                         { In("A", PinValue::MakeBool(false)), In("B", PinValue::MakeBool(false)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeBool(ctx.GetInputBool(0) || ctx.GetInputBool(1))); });

        RegisterPureNode(registry, "Logic.Not", "Not", "Logic", "NOT A",
                         { In("A", PinValue::MakeBool(false)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeBool(!ctx.GetInputBool(0))); });

        RegisterPureNode(registry, "Logic.Greater", "Greater Than", "Logic", "A > B",
                         { In("A", PinValue::MakeFloat(0.0f)), In("B", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeBool(ctx.GetInputFloat(0) > ctx.GetInputFloat(1))); });

        RegisterPureNode(registry, "Logic.Less", "Less Than", "Logic", "A < B",
                         { In("A", PinValue::MakeFloat(0.0f)), In("B", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeBool(ctx.GetInputFloat(0) < ctx.GetInputFloat(1))); });

        // NOT an == node: exact float equality is a bug generator, and the repo's
        // coding rules forbid it outright. The author must pick a tolerance.
        RegisterPureNode(registry, "Logic.NearlyEqual", "Nearly Equal", "Logic", "|A - B| <= Tolerance",
                         { In("A", PinValue::MakeFloat(0.0f)), In("B", PinValue::MakeFloat(0.0f)),
                           In("Tolerance", PinValue::MakeFloat(1e-4f)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         {
                             const f32 tolerance = std::fabs(ctx.GetInputFloat(2));
                             ctx.SetOutput(3, PinValue::MakeBool(std::fabs(ctx.GetInputFloat(0) - ctx.GetInputFloat(1)) <= tolerance));
                         });

        RegisterPureNode(registry, "Logic.IntEqual", "Equal (Integer)", "Logic", "A == B on integers",
                         { In("A", PinValue::MakeInt(0)), In("B", PinValue::MakeInt(0)), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeBool(ctx.GetInputInt(0) == ctx.GetInputInt(1))); });

        RegisterPureNode(registry, "Logic.StringEqual", "Equal (String)", "Logic", "A == B on strings",
                         { In("A", PinValue::MakeString({})), In("B", PinValue::MakeString({})), Out("Result", PinType::Bool) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeBool(ctx.GetInputString(0) == ctx.GetInputString(1))); });

        RegisterPureNode(registry, "Logic.Select", "Select", "Logic", "A when Condition, otherwise B",
                         { In("Condition", PinValue::MakeBool(false)), In("A", PinValue::MakeFloat(0.0f)),
                           In("B", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(3, PinValue::MakeFloat(ctx.GetInputBool(0) ? ctx.GetInputFloat(1) : ctx.GetInputFloat(2))); });

        //-- Vector ---------------------------------------------------------------
        RegisterPureNode(registry, "Vector.Make", "Make Vector", "Vector", "Builds a vector from X, Y and Z",
                         { In("X", PinValue::MakeFloat(0.0f)), In("Y", PinValue::MakeFloat(0.0f)),
                           In("Z", PinValue::MakeFloat(0.0f)), Out("Vector", PinType::Vec3) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(3, PinValue::MakeVec3({ ctx.GetInputFloat(0), ctx.GetInputFloat(1), ctx.GetInputFloat(2) })); });

        RegisterPureNode(registry, "Vector.Break", "Break Vector", "Vector", "Splits a vector into X, Y and Z",
                         { In("Vector", PinType::Vec3), Out("X", PinType::Float), Out("Y", PinType::Float), Out("Z", PinType::Float) },
                         [](NodeContext& ctx)
                         {
                             const glm::vec3 v = ctx.GetInputVec3(0);
                             ctx.SetOutput(1, PinValue::MakeFloat(v.x));
                             ctx.SetOutput(2, PinValue::MakeFloat(v.y));
                             ctx.SetOutput(3, PinValue::MakeFloat(v.z));
                         });

        RegisterPureNode(registry, "Vector.Add", "Add (Vector)", "Vector", "A + B",
                         { In("A", PinType::Vec3), In("B", PinType::Vec3), Out("Result", PinType::Vec3) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeVec3(ctx.GetInputVec3(0) + ctx.GetInputVec3(1))); });

        RegisterPureNode(registry, "Vector.Subtract", "Subtract (Vector)", "Vector", "A - B",
                         { In("A", PinType::Vec3), In("B", PinType::Vec3), Out("Result", PinType::Vec3) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeVec3(ctx.GetInputVec3(0) - ctx.GetInputVec3(1))); });

        RegisterPureNode(registry, "Vector.Scale", "Scale (Vector)", "Vector", "A * Scalar",
                         { In("A", PinType::Vec3), In("Scalar", PinValue::MakeFloat(1.0f)), Out("Result", PinType::Vec3) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeVec3(ctx.GetInputVec3(0) * ctx.GetInputFloat(1))); });

        RegisterPureNode(registry, "Vector.Dot", "Dot Product", "Vector", "A dot B",
                         { In("A", PinType::Vec3), In("B", PinType::Vec3), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeFloat(glm::dot(ctx.GetInputVec3(0), ctx.GetInputVec3(1)))); });

        RegisterPureNode(registry, "Vector.Cross", "Cross Product", "Vector", "A cross B",
                         { In("A", PinType::Vec3), In("B", PinType::Vec3), Out("Result", PinType::Vec3) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeVec3(glm::cross(ctx.GetInputVec3(0), ctx.GetInputVec3(1)))); });

        RegisterPureNode(registry, "Vector.Length", "Vector Length", "Vector", "The magnitude of A",
                         { In("A", PinType::Vec3), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(1, PinValue::MakeFloat(glm::length(ctx.GetInputVec3(0)))); });

        RegisterPureNode(registry, "Vector.Distance", "Vector Distance", "Vector", "The distance between A and B",
                         { In("A", PinType::Vec3), In("B", PinType::Vec3), Out("Result", PinType::Float) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeFloat(glm::distance(ctx.GetInputVec3(0), ctx.GetInputVec3(1)))); });

        RegisterPureNode(registry, "Vector.Normalize", "Normalize", "Vector", "A scaled to unit length (zero stays zero)",
                         { In("A", PinType::Vec3), Out("Result", PinType::Vec3) },
                         [](NodeContext& ctx)
                         {
                             const glm::vec3 v = ctx.GetInputVec3(0);
                             // glm::normalize on a zero vector produces NaNs.
                             // dot(v,v) rather than glm::length2: the latter lives in
                             // gtx/norm.hpp, which needs GLM_ENABLE_EXPERIMENTAL.
                             const f32 lengthSquared = glm::dot(v, v);
                             ctx.SetOutput(1, PinValue::MakeVec3(lengthSquared < kDivideEpsilon ? glm::vec3(0.0f) : v / std::sqrt(lengthSquared)));
                         });

        RegisterPureNode(registry, "Vector.Lerp", "Lerp (Vector)", "Vector", "Linear blend from A to B by Alpha",
                         { In("A", PinType::Vec3), In("B", PinType::Vec3), In("Alpha", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Vec3) },
                         [](NodeContext& ctx)
                         {
                             const glm::vec3 a = ctx.GetInputVec3(0);
                             const glm::vec3 b = ctx.GetInputVec3(1);
                             ctx.SetOutput(3, PinValue::MakeVec3(a + (b - a) * ctx.GetInputFloat(2)));
                         });
    }

} // namespace OloEngine::VisualScript
