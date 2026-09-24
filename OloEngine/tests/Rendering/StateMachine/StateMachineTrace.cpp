#include "OloEnginePCH.h"
#include "StateMachineTrace.h"

#include <charconv>
#include <sstream>

namespace OloEngine::Tests::StateMachine
{
    namespace
    {
        constexpr std::array<std::string_view, static_cast<sizet>(OpKind::Count)> kOpNames{
            "resize", "path",      "feature",          "msaa",      "upscale",         "shader-reload", "scene-reload",
            "entity-churn", "fence-drain", "frames-in-flight", "scene-swap", "history-advance", "camera-move", "pool-trim"
        };
        constexpr std::array<std::string_view, kPathCount> kPathNames{ "forward", "forward-plus", "deferred" };
        constexpr std::array<std::string_view, kUpscaleModes.size()> kUpscaleNames{ "off", "performance", "quality" };
        constexpr std::array<std::string_view, static_cast<sizet>(FeatureId::Count)> kFeatureNames{
            "bloom", "fxaa", "gtao", "gtao-denoise", "ssr", "vignette-grading"
        };

        // How often each operation is drawn. Reloads are the expensive ones,
        // so they are rare; configuration changes are what find cache bugs, so
        // they dominate. The order matches OpKind.
        constexpr std::array<u32, static_cast<sizet>(OpKind::Count)> kWeights{
            2u, // resize
            3u, // path
            5u, // feature
            2u, // msaa
            2u, // upscale
            1u, // shader-reload
            1u, // scene-reload
            2u, // entity-churn
            1u, // fence-drain
            1u, // frames-in-flight
            1u, // scene-swap
            1u, // history-advance
            2u, // camera-move
            1u, // pool-trim
        };

        template<sizet N>
        [[nodiscard]] std::optional<u32> IndexOf(const std::array<std::string_view, N>& names, std::string_view name)
        {
            for (sizet i = 0; i < N; ++i)
            {
                if (names[i] == name)
                    return static_cast<u32>(i);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::vector<std::string_view> SplitWords(std::string_view text)
        {
            std::vector<std::string_view> words;
            sizet i = 0;
            while (i < text.size())
            {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r'))
                    ++i;
                const sizet start = i;
                while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r')
                    ++i;
                if (i > start)
                    words.push_back(text.substr(start, i - start));
            }
            return words;
        }

        [[nodiscard]] std::optional<u64> ParseU64(std::string_view text)
        {
            u64 value = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        [[nodiscard]] std::optional<u32> ParseSizeIndex(std::string_view text)
        {
            const sizet x = text.find('x');
            if (x == std::string_view::npos)
                return std::nullopt;
            const auto width = ParseU64(text.substr(0, x));
            const auto height = ParseU64(text.substr(x + 1));
            if (!width || !height)
                return std::nullopt;
            for (sizet i = 0; i < kSizes.size(); ++i)
            {
                if (kSizes[i].Width == *width && kSizes[i].Height == *height)
                    return static_cast<u32>(i);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string SizeName(u32 index)
        {
            const Size& size = kSizes[index % kSizes.size()];
            return std::to_string(size.Width) + "x" + std::to_string(size.Height);
        }

        [[nodiscard]] std::optional<u32> ParseMsaaIndex(std::string_view text)
        {
            const auto samples = ParseU64(text);
            if (!samples)
                return std::nullopt;
            for (sizet i = 0; i < kMsaaSamples.size(); ++i)
            {
                if (kMsaaSamples[i] == *samples)
                    return static_cast<u32>(i);
            }
            return std::nullopt;
        }

        // Feature ops pack the feature and the on/off bit: Arg = feature*2 + on.
        [[nodiscard]] u32 FeatureArg(FeatureId feature, bool on)
        {
            return (static_cast<u32>(feature) * 2u) + (on ? 1u : 0u);
        }
    } // namespace

    ModelConfig Apply(ModelConfig config, const Op& op)
    {
        switch (op.Kind)
        {
            case OpKind::Resize:
                config.SizeIndex = op.Arg % static_cast<u32>(kSizes.size());
                break;
            case OpKind::Path:
                config.Path = op.Arg % kPathCount;
                break;
            case OpKind::Feature:
            {
                const u32 feature = (op.Arg / 2u) % static_cast<u32>(FeatureId::Count);
                const u32 bit = 1u << feature;
                config.Features = (op.Arg % 2u) != 0u ? (config.Features | bit) : (config.Features & ~bit);
                break;
            }
            case OpKind::Msaa:
                config.MsaaIndex = op.Arg % static_cast<u32>(kMsaaSamples.size());
                break;
            case OpKind::Upscale:
                config.UpscaleIndex = op.Arg % static_cast<u32>(kUpscaleModes.size());
                break;
            case OpKind::EntityChurn:
                config.BlendedMesh = op.Arg != 0u;
                break;
            case OpKind::FramesInFlight:
                config.DoubleBuffering = op.Arg != 0u;
                break;
            case OpKind::CameraMove:
                config.Pose = op.Arg % kPoseCount;
                break;
            case OpKind::ShaderReload:
            case OpKind::SceneReload:
            case OpKind::FenceDrain:
            case OpKind::SceneSwap:
            case OpKind::HistoryAdvance:
            case OpKind::PoolTrim:
            case OpKind::Count:
                break;
        }
        return config;
    }

    ModelConfig Trace::Final() const
    {
        ModelConfig config = Initial;
        for (const Op& op : Ops)
            config = Apply(config, op);
        return config;
    }

    Trace GenerateTrace(u64 seed, u32 length)
    {
        u32 totalWeight = 0;
        for (const u32 weight : kWeights)
            totalWeight += weight;

        SplitMix64 rng(seed);
        Trace trace;
        trace.Seed = seed;
        trace.Ops.reserve(length);
        for (u32 i = 0; i < length; ++i)
        {
            u32 pick = rng.Below(totalWeight);
            sizet kind = 0;
            while (pick >= kWeights[kind])
            {
                pick -= kWeights[kind];
                ++kind;
            }

            Op op;
            op.Kind = static_cast<OpKind>(kind);
            switch (op.Kind)
            {
                case OpKind::Resize:
                    op.Arg = rng.Below(static_cast<u32>(kSizes.size()));
                    break;
                case OpKind::Path:
                    op.Arg = rng.Below(kPathCount);
                    break;
                case OpKind::Feature:
                    op.Arg = rng.Below(static_cast<u32>(FeatureId::Count) * 2u);
                    break;
                case OpKind::Msaa:
                    op.Arg = rng.Below(static_cast<u32>(kMsaaSamples.size()));
                    break;
                case OpKind::Upscale:
                    op.Arg = rng.Below(static_cast<u32>(kUpscaleModes.size()));
                    break;
                case OpKind::EntityChurn:
                case OpKind::FramesInFlight:
                    op.Arg = rng.Below(2u);
                    break;
                case OpKind::CameraMove:
                    op.Arg = rng.Below(kPoseCount);
                    break;
                default:
                    op.Arg = 0u;
                    break;
            }
            trace.Ops.push_back(op);
        }
        return trace;
    }

    std::string ToString(const Op& op)
    {
        const auto kindIndex = static_cast<sizet>(op.Kind);
        if (kindIndex >= kOpNames.size())
            return "invalid";
        std::string out(kOpNames[kindIndex]);
        switch (op.Kind)
        {
            case OpKind::Resize:
                out += " " + SizeName(op.Arg);
                break;
            case OpKind::Path:
                out += " " + std::string(kPathNames[op.Arg % kPathCount]);
                break;
            case OpKind::Feature:
                out += " " + std::string(kFeatureNames[(op.Arg / 2u) % kFeatureNames.size()]) +
                       ((op.Arg % 2u) != 0u ? " on" : " off");
                break;
            case OpKind::Msaa:
                out += " " + std::to_string(kMsaaSamples[op.Arg % kMsaaSamples.size()]);
                break;
            case OpKind::Upscale:
                out += " " + std::string(kUpscaleNames[op.Arg % kUpscaleNames.size()]);
                break;
            case OpKind::EntityChurn:
                out += op.Arg != 0u ? " add" : " remove";
                break;
            case OpKind::FramesInFlight:
                out += op.Arg != 0u ? " 2" : " 1";
                break;
            case OpKind::CameraMove:
                out += " " + std::to_string(op.Arg % kPoseCount);
                break;
            default:
                break;
        }
        return out;
    }

    std::optional<Op> ParseOp(std::string_view text)
    {
        const std::vector<std::string_view> words = SplitWords(text);
        if (words.empty())
            return std::nullopt;
        const std::optional<u32> kind = IndexOf(kOpNames, words[0]);
        if (!kind)
            return std::nullopt;

        Op op;
        op.Kind = static_cast<OpKind>(*kind);
        const auto expectArgs = [&words](sizet count)
        { return words.size() == count + 1u; };

        switch (op.Kind)
        {
            case OpKind::Resize:
            {
                if (!expectArgs(1))
                    return std::nullopt;
                const auto index = ParseSizeIndex(words[1]);
                if (!index)
                    return std::nullopt;
                op.Arg = *index;
                break;
            }
            case OpKind::Path:
            {
                if (!expectArgs(1))
                    return std::nullopt;
                const auto index = IndexOf(kPathNames, words[1]);
                if (!index)
                    return std::nullopt;
                op.Arg = *index;
                break;
            }
            case OpKind::Feature:
            {
                if (!expectArgs(2) || (words[2] != "on" && words[2] != "off"))
                    return std::nullopt;
                const auto feature = IndexOf(kFeatureNames, words[1]);
                if (!feature)
                    return std::nullopt;
                op.Arg = FeatureArg(static_cast<FeatureId>(*feature), words[2] == "on");
                break;
            }
            case OpKind::Msaa:
            {
                if (!expectArgs(1))
                    return std::nullopt;
                const auto index = ParseMsaaIndex(words[1]);
                if (!index)
                    return std::nullopt;
                op.Arg = *index;
                break;
            }
            case OpKind::Upscale:
            {
                if (!expectArgs(1))
                    return std::nullopt;
                const auto index = IndexOf(kUpscaleNames, words[1]);
                if (!index)
                    return std::nullopt;
                op.Arg = *index;
                break;
            }
            case OpKind::EntityChurn:
                if (!expectArgs(1) || (words[1] != "add" && words[1] != "remove"))
                    return std::nullopt;
                op.Arg = words[1] == "add" ? 1u : 0u;
                break;
            case OpKind::FramesInFlight:
                if (!expectArgs(1) || (words[1] != "1" && words[1] != "2"))
                    return std::nullopt;
                op.Arg = words[1] == "2" ? 1u : 0u;
                break;
            case OpKind::CameraMove:
            {
                if (!expectArgs(1))
                    return std::nullopt;
                const auto pose = ParseU64(words[1]);
                if (!pose || *pose >= kPoseCount)
                    return std::nullopt;
                op.Arg = static_cast<u32>(*pose);
                break;
            }
            default:
                if (!expectArgs(0))
                    return std::nullopt;
                break;
        }
        return op;
    }

    std::string DescribeConfig(const ModelConfig& config)
    {
        std::string features;
        for (sizet i = 0; i < kFeatureNames.size(); ++i)
        {
            if (config.HasFeature(static_cast<FeatureId>(i)))
            {
                if (!features.empty())
                    features += ',';
                features += kFeatureNames[i];
            }
        }
        return "path=" + std::string(kPathNames[config.Path % kPathCount]) + " size=" + SizeName(config.SizeIndex) +
               " msaa=" + std::to_string(kMsaaSamples[config.MsaaIndex % kMsaaSamples.size()]) +
               " upscale=" + std::string(kUpscaleNames[config.UpscaleIndex % kUpscaleNames.size()]) +
               " features=" + features + " blended=" + (config.BlendedMesh ? "1" : "0") +
               " buffering=" + (config.DoubleBuffering ? "2" : "1") + " pose=" + std::to_string(config.Pose % kPoseCount);
    }

    std::string Serialize(const Trace& trace)
    {
        std::string out = "olo-state-machine-trace 1\n";
        out += "seed " + std::to_string(trace.Seed) + "\n";
        out += "initial " + DescribeConfig(trace.Initial) + "\n";
        for (const Op& op : trace.Ops)
            out += "op " + ToString(op) + "\n";
        return out;
    }

    namespace
    {
        [[nodiscard]] bool ParseConfig(std::string_view text, ModelConfig& out, std::string& error)
        {
            ModelConfig config;
            for (const std::string_view word : SplitWords(text))
            {
                const sizet eq = word.find('=');
                if (eq == std::string_view::npos)
                {
                    error = "initial: expected key=value, got '" + std::string(word) + "'";
                    return false;
                }
                const std::string_view key = word.substr(0, eq);
                const std::string_view value = word.substr(eq + 1);
                bool ok = true;
                if (key == "path")
                {
                    const auto index = IndexOf(kPathNames, value);
                    ok = index.has_value();
                    config.Path = index.value_or(0u);
                }
                else if (key == "size")
                {
                    const auto index = ParseSizeIndex(value);
                    ok = index.has_value();
                    config.SizeIndex = index.value_or(0u);
                }
                else if (key == "msaa")
                {
                    const auto index = ParseMsaaIndex(value);
                    ok = index.has_value();
                    config.MsaaIndex = index.value_or(0u);
                }
                else if (key == "upscale")
                {
                    const auto index = IndexOf(kUpscaleNames, value);
                    ok = index.has_value();
                    config.UpscaleIndex = index.value_or(0u);
                }
                else if (key == "features")
                {
                    config.Features = 0u;
                    std::string_view rest = value;
                    while (!rest.empty())
                    {
                        const sizet comma = rest.find(',');
                        const std::string_view name = rest.substr(0, comma);
                        const auto feature = IndexOf(kFeatureNames, name);
                        if (!feature)
                        {
                            ok = false;
                            break;
                        }
                        config.Features |= 1u << *feature;
                        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                    }
                }
                else if (key == "blended")
                {
                    ok = value == "0" || value == "1";
                    config.BlendedMesh = value == "1";
                }
                else if (key == "buffering")
                {
                    ok = value == "1" || value == "2";
                    config.DoubleBuffering = value == "2";
                }
                else if (key == "pose")
                {
                    const auto pose = ParseU64(value);
                    ok = pose.has_value() && *pose < kPoseCount;
                    config.Pose = static_cast<u32>(pose.value_or(0u));
                }
                else
                {
                    ok = false;
                }
                if (!ok)
                {
                    error = "initial: bad '" + std::string(word) + "'";
                    return false;
                }
            }
            out = config;
            return true;
        }
    } // namespace

    std::optional<Trace> ParseTrace(std::string_view text, std::string* error)
    {
        std::string localError;
        std::string& err = error != nullptr ? *error : localError;
        Trace trace;
        bool sawHeader = false;
        u32 lineNumber = 0;
        while (!text.empty())
        {
            const sizet newline = text.find('\n');
            std::string_view line = text.substr(0, newline);
            text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
            ++lineNumber;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.remove_suffix(1);
            if (line.empty() || line.front() == '#')
                continue;

            const sizet space = line.find(' ');
            const std::string_view keyword = line.substr(0, space);
            const std::string_view rest = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
            const std::string where = "line " + std::to_string(lineNumber) + ": ";
            if (!sawHeader)
            {
                if (keyword != "olo-state-machine-trace" || rest != "1")
                {
                    err = where + "expected 'olo-state-machine-trace 1'";
                    return std::nullopt;
                }
                sawHeader = true;
                continue;
            }
            if (keyword == "seed")
            {
                const auto seed = ParseU64(rest);
                if (!seed)
                {
                    err = where + "bad seed";
                    return std::nullopt;
                }
                trace.Seed = *seed;
            }
            else if (keyword == "initial")
            {
                std::string configError;
                if (!ParseConfig(rest, trace.Initial, configError))
                {
                    err = where + configError;
                    return std::nullopt;
                }
            }
            else if (keyword == "op")
            {
                const auto op = ParseOp(rest);
                if (!op)
                {
                    err = where + "bad operation '" + std::string(rest) + "'";
                    return std::nullopt;
                }
                trace.Ops.push_back(*op);
            }
            else
            {
                err = where + "unknown keyword '" + std::string(keyword) + "'";
                return std::nullopt;
            }
        }
        if (!sawHeader)
        {
            err = "empty trace";
            return std::nullopt;
        }
        return trace;
    }
} // namespace OloEngine::Tests::StateMachine
