#include "OloEnginePCH.h"
#include "OloEngine/Cinematic/CinematicSequenceSerializer.h"

#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        // v2 (this build) adds per-key InTangent / OutTangent for the
        // CinematicInterp::Bezier ease. Reading is forward/backward tolerant:
        // every new field has a 0 default, so a v1 file (no tangents, no Bezier
        // keys) round-trips identically and a v2 file loads in an older build as
        // plain Linear/EaseInOut keys. The version is bumped for provenance and to
        // warn loudly if a *newer* (unknown) format is ever opened here.
        constexpr i32 kCinematicSequenceVersion = 2;

        [[nodiscard]] bool IsFinite(f32 v) noexcept
        {
            return std::isfinite(v);
        }
        [[nodiscard]] bool IsFinite(const glm::vec3& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        void EmitVec3(YAML::Emitter& out, const glm::vec3& v)
        {
            out << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
        }
        void EmitQuat(YAML::Emitter& out, const glm::quat& q)
        {
            // Stored x, y, z, w.
            out << YAML::Flow << YAML::BeginSeq << q.x << q.y << q.z << q.w << YAML::EndSeq;
        }

        [[nodiscard]] glm::vec3 ReadVec3(const YAML::Node& node, const glm::vec3& fallback)
        {
            if (!node || !node.IsSequence() || node.size() < 3)
            {
                return fallback;
            }
            const glm::vec3 v{ node[0].as<f32>(fallback.x), node[1].as<f32>(fallback.y), node[2].as<f32>(fallback.z) };
            return IsFinite(v) ? v : fallback;
        }
        [[nodiscard]] glm::quat ReadQuat(const YAML::Node& node)
        {
            constexpr glm::quat identity{ 1.0f, 0.0f, 0.0f, 0.0f };
            if (!node || !node.IsSequence() || node.size() < 4)
            {
                return identity;
            }
            const glm::quat q{ /*w*/ node[3].as<f32>(1.0f), /*x*/ node[0].as<f32>(0.0f),
                               /*y*/ node[1].as<f32>(0.0f), /*z*/ node[2].as<f32>(0.0f) };
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
            {
                return identity;
            }
            const f32 len2 = glm::dot(q, q);
            return (len2 > 1e-12f) ? glm::normalize(q) : identity;
        }

        // Interp + tangents are common to every keyed (curve) channel. Tangents
        // are emitted unconditionally (two floats per key) so that *either*
        // endpoint of a Bezier segment round-trips its handle, regardless of its
        // own interp mode — a Bezier segment reads the left key's OutTangent and
        // the right key's InTangent, so gating emission on the key's own mode
        // would silently drop a non-Bezier right key's InTangent.
        template<typename KeyT>
        void EmitInterpAndTangents(YAML::Emitter& out, const KeyT& k)
        {
            out << YAML::Key << "Interp" << YAML::Value << static_cast<i32>(k.Interp);
            out << YAML::Key << "InTangent" << YAML::Value << k.InTangent;
            out << YAML::Key << "OutTangent" << YAML::Value << k.OutTangent;
        }

        // ----- channel emit -----
        void EmitFloatChannel(YAML::Emitter& out, std::string_view key, const CinematicFloatChannel& channel)
        {
            out << YAML::Key << std::string(key) << YAML::Value << YAML::BeginSeq;
            for (const auto& k : channel.Keys)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << k.Time;
                out << YAML::Key << "Value" << YAML::Value << k.Value;
                EmitInterpAndTangents(out, k);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }
        void EmitVec3Channel(YAML::Emitter& out, std::string_view key, const CinematicVec3Channel& channel)
        {
            out << YAML::Key << std::string(key) << YAML::Value << YAML::BeginSeq;
            for (const auto& k : channel.Keys)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << k.Time;
                out << YAML::Key << "Value" << YAML::Value;
                EmitVec3(out, k.Value);
                EmitInterpAndTangents(out, k);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }
        void EmitQuatChannel(YAML::Emitter& out, std::string_view key, const CinematicQuatChannel& channel)
        {
            out << YAML::Key << std::string(key) << YAML::Value << YAML::BeginSeq;
            for (const auto& k : channel.Keys)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << k.Time;
                out << YAML::Key << "Value" << YAML::Value;
                EmitQuat(out, k.Value);
                EmitInterpAndTangents(out, k);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        // ----- channel read -----
        [[nodiscard]] CinematicInterp ReadInterp(const YAML::Node& node)
        {
            const i32 raw = node ? node.as<i32>(static_cast<i32>(CinematicInterp::Linear)) : static_cast<i32>(CinematicInterp::Linear);
            if (raw < 0 || raw > static_cast<i32>(CinematicInterp::Bezier))
            {
                return CinematicInterp::Linear;
            }
            return static_cast<CinematicInterp>(raw);
        }
        // Read a single tangent slope, defaulting to 0 (flat) when absent (a v1
        // file) and rejecting non-finite values per the "validate every float read
        // from YAML" rule. A zero tangent makes a Bezier segment a smoothstep.
        [[nodiscard]] f32 ReadTangent(const YAML::Node& node)
        {
            if (!node)
            {
                return 0.0f;
            }
            const f32 v = node.as<f32>(0.0f);
            return std::isfinite(v) ? v : 0.0f;
        }
        // CinematicCurve evaluation assumes keys are sorted by Time ascending.
        // YAML preserves authoring order, which a hand-edited file may not honour,
        // so enforce the invariant after reading. stable_sort keeps the relative
        // order of equal-time keys (matters for degenerate same-time segments).
        template<typename KeyVec>
        void SortKeysByTime(KeyVec& keys)
        {
            std::stable_sort(keys.begin(), keys.end(),
                             [](const auto& a, const auto& b)
                             { return a.Time < b.Time; });
        }
        void ReadFloatChannel(const YAML::Node& node, CinematicFloatChannel& channel)
        {
            if (!node || !node.IsSequence())
            {
                return;
            }
            for (const auto& keyNode : node)
            {
                // NaN default so a missing/unparseable Time or Value is caught by
                // the finite check below and skipped, rather than materialising a
                // synthetic t == 0 / value == 0 key.
                const f32 time = keyNode["Time"].as<f32>(std::numeric_limits<f32>::quiet_NaN());
                const f32 value = keyNode["Value"].as<f32>(std::numeric_limits<f32>::quiet_NaN());
                if (!IsFinite(time) || !IsFinite(value))
                {
                    continue;
                }
                channel.Keys.push_back({ time, value, ReadInterp(keyNode["Interp"]),
                                         ReadTangent(keyNode["InTangent"]), ReadTangent(keyNode["OutTangent"]) });
            }
            SortKeysByTime(channel.Keys);
        }
        void ReadVec3Channel(const YAML::Node& node, CinematicVec3Channel& channel)
        {
            if (!node || !node.IsSequence())
            {
                return;
            }
            for (const auto& keyNode : node)
            {
                const f32 time = keyNode["Time"].as<f32>(std::numeric_limits<f32>::quiet_NaN());
                if (!IsFinite(time) || !keyNode["Value"])
                {
                    continue; // skip keys with a missing/unparseable Time or no Value
                }
                channel.Keys.push_back({ time, ReadVec3(keyNode["Value"], glm::vec3(0.0f)), ReadInterp(keyNode["Interp"]),
                                         ReadTangent(keyNode["InTangent"]), ReadTangent(keyNode["OutTangent"]) });
            }
            SortKeysByTime(channel.Keys);
        }
        void ReadQuatChannel(const YAML::Node& node, CinematicQuatChannel& channel)
        {
            if (!node || !node.IsSequence())
            {
                return;
            }
            for (const auto& keyNode : node)
            {
                const f32 time = keyNode["Time"].as<f32>(std::numeric_limits<f32>::quiet_NaN());
                if (!IsFinite(time) || !keyNode["Value"])
                {
                    continue; // skip keys with a missing/unparseable Time or no Value
                }
                channel.Keys.push_back({ time, ReadQuat(keyNode["Value"]), ReadInterp(keyNode["Interp"]),
                                         ReadTangent(keyNode["InTangent"]), ReadTangent(keyNode["OutTangent"]) });
            }
            SortKeysByTime(channel.Keys);
        }
    } // namespace

    std::string CinematicSequenceSerializer::SerializeToString(const Ref<CinematicSequence>& sequence)
    {
        OLO_PROFILE_FUNCTION();

        if (!sequence)
        {
            return {};
        }

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "CinematicSequence" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Version" << YAML::Value << kCinematicSequenceVersion;
        out << YAML::Key << "Name" << YAML::Value << sequence->Name;
        out << YAML::Key << "Duration" << YAML::Value << sequence->Duration;

        out << YAML::Key << "TransformTracks" << YAML::Value << YAML::BeginSeq;
        for (const auto& track : sequence->TransformTracks)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Target" << YAML::Value << static_cast<u64>(track.Target);
            out << YAML::Key << "Name" << YAML::Value << track.Name;
            EmitVec3Channel(out, "Translation", track.Translation);
            EmitQuatChannel(out, "Rotation", track.Rotation);
            EmitVec3Channel(out, "Scale", track.Scale);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "CameraTracks" << YAML::Value << YAML::BeginSeq;
        for (const auto& track : sequence->CameraTracks)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Target" << YAML::Value << static_cast<u64>(track.Target);
            out << YAML::Key << "Name" << YAML::Value << track.Name;
            EmitVec3Channel(out, "Position", track.Position);
            EmitQuatChannel(out, "Rotation", track.Rotation);
            EmitFloatChannel(out, "VerticalFovRadians", track.VerticalFovRadians);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "VisibilityTracks" << YAML::Value << YAML::BeginSeq;
        for (const auto& track : sequence->VisibilityTracks)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Target" << YAML::Value << static_cast<u64>(track.Target);
            out << YAML::Key << "Name" << YAML::Value << track.Name;
            out << YAML::Key << "Keys" << YAML::Value << YAML::BeginSeq;
            for (const auto& k : track.Keys)
            {
                out << YAML::Flow << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << k.Time;
                out << YAML::Key << "Visible" << YAML::Value << k.Visible;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "EventTracks" << YAML::Value << YAML::BeginSeq;
        for (const auto& track : sequence->EventTracks)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << track.Name;
            out << YAML::Key << "Keys" << YAML::Value << YAML::BeginSeq;
            for (const auto& k : track.Keys)
            {
                out << YAML::Flow << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << k.Time;
                out << YAML::Key << "Name" << YAML::Value << k.Name;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap; // CinematicSequence
        out << YAML::EndMap; // root

        return std::string(out.c_str());
    }

    Ref<CinematicSequence> CinematicSequenceSerializer::DeserializeFromString(const std::string& yamlString)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node root;
        try
        {
            root = YAML::Load(yamlString);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("CinematicSequenceSerializer: failed to parse YAML: {}", e.what());
            return nullptr;
        }

        const YAML::Node seqNode = root["CinematicSequence"];
        if (!seqNode)
        {
            OLO_CORE_ERROR("CinematicSequenceSerializer: missing 'CinematicSequence' root node");
            return nullptr;
        }

        // Tolerant version handling: every field added since v1 has a safe
        // default, so an unknown future version still loads (just without whatever
        // it added) — warn so the lossy read isn't silent. Missing Version => v1.
        if (const i32 version = seqNode["Version"].as<i32>(1); version > kCinematicSequenceVersion)
        {
            OLO_CORE_WARN("CinematicSequenceSerializer: file version {} is newer than supported version {}; "
                          "unknown fields will be ignored",
                          version, kCinematicSequenceVersion);
        }

        auto sequence = Ref<CinematicSequence>::Create();
        sequence->Name = seqNode["Name"].as<std::string>(std::string{});
        if (const f32 duration = seqNode["Duration"].as<f32>(0.0f); std::isfinite(duration) && duration >= 0.0f)
        {
            sequence->Duration = duration;
        }

        if (const YAML::Node tracks = seqNode["TransformTracks"]; tracks && tracks.IsSequence())
        {
            for (const auto& trackNode : tracks)
            {
                CinematicTransformTrack track;
                track.Target = UUID(trackNode["Target"].as<u64>(0));
                track.Name = trackNode["Name"].as<std::string>(std::string{});
                ReadVec3Channel(trackNode["Translation"], track.Translation);
                ReadQuatChannel(trackNode["Rotation"], track.Rotation);
                ReadVec3Channel(trackNode["Scale"], track.Scale);
                sequence->TransformTracks.push_back(std::move(track));
            }
        }

        if (const YAML::Node tracks = seqNode["CameraTracks"]; tracks && tracks.IsSequence())
        {
            for (const auto& trackNode : tracks)
            {
                CinematicCameraTrack track;
                track.Target = UUID(trackNode["Target"].as<u64>(0));
                track.Name = trackNode["Name"].as<std::string>(std::string{});
                ReadVec3Channel(trackNode["Position"], track.Position);
                ReadQuatChannel(trackNode["Rotation"], track.Rotation);
                ReadFloatChannel(trackNode["VerticalFovRadians"], track.VerticalFovRadians);
                sequence->CameraTracks.push_back(std::move(track));
            }
        }

        if (const YAML::Node tracks = seqNode["VisibilityTracks"]; tracks && tracks.IsSequence())
        {
            for (const auto& trackNode : tracks)
            {
                CinematicVisibilityTrack track;
                track.Target = UUID(trackNode["Target"].as<u64>(0));
                track.Name = trackNode["Name"].as<std::string>(std::string{});
                if (const YAML::Node keys = trackNode["Keys"]; keys && keys.IsSequence())
                {
                    for (const auto& keyNode : keys)
                    {
                        const f32 time = keyNode["Time"].as<f32>(std::numeric_limits<f32>::quiet_NaN());
                        if (!std::isfinite(time))
                        {
                            continue; // skip keys with a missing/unparseable Time
                        }
                        track.Keys.push_back({ time, keyNode["Visible"].as<bool>(true) });
                    }
                }
                SortKeysByTime(track.Keys); // visibility step-eval assumes ascending time
                sequence->VisibilityTracks.push_back(std::move(track));
            }
        }

        if (const YAML::Node tracks = seqNode["EventTracks"]; tracks && tracks.IsSequence())
        {
            for (const auto& trackNode : tracks)
            {
                CinematicEventTrack track;
                track.Name = trackNode["Name"].as<std::string>(std::string{});
                if (const YAML::Node keys = trackNode["Keys"]; keys && keys.IsSequence())
                {
                    for (const auto& keyNode : keys)
                    {
                        const f32 time = keyNode["Time"].as<f32>(std::numeric_limits<f32>::quiet_NaN());
                        if (!std::isfinite(time))
                        {
                            continue; // skip keys with a missing/unparseable Time
                        }
                        track.Keys.push_back({ time, keyNode["Name"].as<std::string>(std::string{}) });
                    }
                }
                SortKeysByTime(track.Keys); // event edge-firing assumes ascending time
                sequence->EventTracks.push_back(std::move(track));
            }
        }

        return sequence;
    }

    bool CinematicSequenceSerializer::Serialize(const Ref<CinematicSequence>& sequence, const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        const std::string yaml = SerializeToString(sequence);
        if (yaml.empty())
        {
            return false;
        }

        std::error_code ec;
        if (const std::filesystem::path parent = std::filesystem::path(filepath).parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream fout(filepath, std::ios::binary | std::ios::trunc);
        if (!fout)
        {
            OLO_CORE_ERROR("CinematicSequenceSerializer: failed to open '{}' for writing", filepath);
            return false;
        }
        fout << yaml;
        return static_cast<bool>(fout);
    }

    Ref<CinematicSequence> CinematicSequenceSerializer::DeserializeAsset(const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        std::ifstream fin(filepath, std::ios::binary);
        if (!fin)
        {
            OLO_CORE_ERROR("CinematicSequenceSerializer: failed to open '{}' for reading", filepath);
            return nullptr;
        }
        std::stringstream buffer;
        buffer << fin.rdbuf();
        return DeserializeFromString(buffer.str());
    }
} // namespace OloEngine
