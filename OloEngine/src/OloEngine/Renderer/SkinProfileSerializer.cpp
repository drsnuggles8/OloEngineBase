#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h" // AssetSerializer.h forward-declares Scene; pulling it in resolves Ref<Scene>::~Ref instantiation chains.
#include "OloEngine/Renderer/SkinProfile.h"

#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    namespace
    {
        // Every float that crosses this boundary goes through here. A YAML file
        // is untrusted input (CLAUDE.md → Conventions): `nan` and `.inf` are
        // both valid YAML floats, and a non-finite scatter radius reaches the
        // GPU as a NaN the whole frame divides by. Reading into a default and
        // then running SkinProfileParameters::Sanitize() over the record is the
        // second half of the same gate.
        [[nodiscard]] f32 ReadFinite(const YAML::Node& node, f32 fallback, const char* what)
        {
            if (!node)
                return fallback;
            const f32 value = node.as<f32>(fallback);
            if (!std::isfinite(value))
            {
                OLO_CORE_WARN("SkinProfileSerializer - '{}' is not finite; using {}.", what, fallback);
                return fallback;
            }
            return value;
        }

        [[nodiscard]] glm::vec3 ReadFiniteVec3(const YAML::Node& node, const glm::vec3& fallback, const char* what)
        {
            if (!node || !node.IsSequence() || node.size() != 3)
                return fallback;
            return glm::vec3(ReadFinite(node[0], fallback.x, what),
                             ReadFinite(node[1], fallback.y, what),
                             ReadFinite(node[2], fallback.z, what));
        }

        void EmitVec3(YAML::Emitter& out, const char* key, const glm::vec3& value)
        {
            out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << YAML::EndSeq;
        }
    } // namespace

    std::string SkinProfileSerializer::SerializeToYAML(const Ref<SkinProfile>& profile) const
    {
        OLO_PROFILE_FUNCTION();

        const SkinProfileParameters& p = profile->GetParameters();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "SkinProfile" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << std::string(profile->GetName());
        // The ALGORITHM version, written as its integer because that is what is
        // on disk and what append-only numbering protects. It is deliberately a
        // separate key from anything naming the material's kind: a scene records
        // "this material is skin", a profile records "authored against transport
        // version N", and the two move independently (issue #1231, ADR 0024).
        out << YAML::Key << "EvaluationModel" << YAML::Value << static_cast<i32>(p.EvaluationModel);
        // Units and colour space are written into the file, not just the header,
        // so a hand-edited .oloskin says what its numbers mean.
        out << YAML::Key << "Units" << YAML::Value << "colours: linear Rec.709 (unitless 0..1); lengths: millimetres";
        EmitVec3(out, "ScatterColor", p.ScatterColor);
        EmitVec3(out, "ScatterRadiusMM", p.ScatterRadiusMM);
        out << YAML::Key << "ThicknessScale" << YAML::Value << p.ThicknessScale;
        EmitVec3(out, "SpecularTint", p.SpecularTint);
        // The transmission lobe (issue #1242). A NESTED MAP rather than three
        // more top-level keys, so a reader can see at a glance which parameters
        // belong to the transport that only version 2 evaluates — and so adding
        // a fourth cannot collide with a diffusion key.
        out << YAML::Key << "Transmission" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Strength" << YAML::Value << p.Transmission.Strength;
        out << YAML::Key << "Anisotropy" << YAML::Value << p.Transmission.Anisotropy;
        out << YAML::Key << "Power" << YAML::Value << p.Transmission.Power;
        out << YAML::EndMap;
        // The layered surface response (issue #1243), nested for the reason
        // Transmission is: these five belong to the transport only version 3
        // evaluates, and a reader should be able to see that without consulting
        // a header.
        out << YAML::Key << "Specular" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "LobeMix" << YAML::Value << p.Specular.LobeMix;
        out << YAML::Key << "LobeRoughnessScale" << YAML::Value << p.Specular.LobeRoughnessScale;
        out << YAML::Key << "NormalVarianceStrength" << YAML::Value << p.Specular.NormalVarianceStrength;
        out << YAML::Key << "DetailStrength" << YAML::Value << p.Specular.DetailStrength;
        out << YAML::Key << "ExpressionDetailGain" << YAML::Value << p.Specular.ExpressionDetailGain;
        out << YAML::EndMap;
        // The oral surface (issue #1245), nested for the reason the two blocks
        // above are: these four belong to the transport only version 4
        // evaluates, and grouping them is what lets an author read a .oloskin
        // and see which of its numbers a version-3 head would ignore.
        out << YAML::Key << "Oral" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "CoatStrength" << YAML::Value << p.Oral.CoatStrength;
        out << YAML::Key << "CoatRoughness" << YAML::Value << p.Oral.CoatRoughness;
        out << YAML::Key << "CoatIor" << YAML::Value << p.Oral.CoatIor;
        out << YAML::Key << "CavityOcclusion" << YAML::Value << p.Oral.CavityOcclusion;
        out << YAML::EndMap;

        // The eye (issue #1244), nested for the reason the three blocks above
        // are. TWELVE fields rather than four, and they are worth the space:
        // six of them are clinical lengths and an index, which is what lets an
        // author read this block against a real eye instead of against a
        // shader. Written in the order the guide's authoring table lists them —
        // the two strengths first, because they are the two an author touches.
        out << YAML::Key << "Ocular" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "OcularStrength" << YAML::Value << p.Ocular.OcularStrength;
        out << YAML::Key << "RefractionStrength" << YAML::Value << p.Ocular.RefractionStrength;
        out << YAML::Key << "EyeRadiusMM" << YAML::Value << p.Ocular.EyeRadiusMM;
        out << YAML::Key << "CorneaRadiusMM" << YAML::Value << p.Ocular.CorneaRadiusMM;
        out << YAML::Key << "IrisRadiusMM" << YAML::Value << p.Ocular.IrisRadiusMM;
        out << YAML::Key << "PupilRadiusMM" << YAML::Value << p.Ocular.PupilRadiusMM;
        out << YAML::Key << "IrisPlaneDepthMM" << YAML::Value << p.Ocular.IrisPlaneDepthMM;
        out << YAML::Key << "CorneaIor" << YAML::Value << p.Ocular.CorneaIor;
        out << YAML::Key << "LimbalRingWidthMM" << YAML::Value << p.Ocular.LimbalRingWidthMM;
        out << YAML::Key << "LimbalRingStrength" << YAML::Value << p.Ocular.LimbalRingStrength;
        out << YAML::Key << "PupilDarkening" << YAML::Value << p.Ocular.PupilDarkening;
        out << YAML::Key << "IrisConcavity" << YAML::Value << p.Ocular.IrisConcavity;
        EmitVec3(out, "IrisColor", p.Ocular.IrisColor);
        out << YAML::EndMap;
        out << YAML::EndMap;
        out << YAML::EndMap;
        return std::string(out.c_str());
    }

    bool SkinProfileSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<SkinProfile>& profile) const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            YAML::Node root = YAML::Load(yamlString);
            auto section = root["SkinProfile"];
            if (!section)
                return false;

            const SkinProfileParameters defaults{};
            SkinProfileParameters parameters;

            if (auto n = section["Name"]; n)
                profile->SetName(n.as<std::string>(std::string("Skin")));

            if (auto n = section["EvaluationModel"]; n)
            {
                const i32 model = n.as<i32>(0);
                if (IsValidSkinEvaluationModel(model))
                {
                    parameters.EvaluationModel = static_cast<SkinEvaluationModel>(model);
                }
                else
                {
                    // Loud, not silent: a profile authored against a transport
                    // version this build does not have would otherwise shade as
                    // version 0 and look merely "a bit off".
                    OLO_CORE_ERROR("SkinProfileSerializer - evaluation model {} is not one this build knows "
                                   "(valid range 0..{}); loading as {}.",
                                   model, kSkinEvaluationModelCount - 1, ToString(defaults.EvaluationModel));
                }
            }

            parameters.ScatterColor = ReadFiniteVec3(section["ScatterColor"], defaults.ScatterColor, "ScatterColor");
            parameters.ScatterRadiusMM = ReadFiniteVec3(section["ScatterRadiusMM"], defaults.ScatterRadiusMM, "ScatterRadiusMM");
            parameters.ThicknessScale = ReadFinite(section["ThicknessScale"], defaults.ThicknessScale, "ThicknessScale");
            parameters.SpecularTint = ReadFiniteVec3(section["SpecularTint"], defaults.SpecularTint, "SpecularTint");

            // The transmission lobe (issue #1242). A file written before #1242
            // has no Transmission node at all, and that is the case this shape
            // is built for: every field keeps its DEFAULT, and the defaults are
            // the ones that make a version-2 profile transmit sensibly. Since
            // such a file is also at transport version 0 or 1, the lobe is not
            // evaluated anyway — so an old .oloskin round-trips to the same
            // pixels, which is the property the version exists to protect.
            if (auto transmission = section["Transmission"]; transmission)
            {
                parameters.Transmission.Strength =
                    ReadFinite(transmission["Strength"], defaults.Transmission.Strength, "Transmission.Strength");
                parameters.Transmission.Anisotropy =
                    ReadFinite(transmission["Anisotropy"], defaults.Transmission.Anisotropy, "Transmission.Anisotropy");
                parameters.Transmission.Power =
                    ReadFinite(transmission["Power"], defaults.Transmission.Power, "Transmission.Power");
            }

            // The layered surface response (issue #1243). Same shape and same
            // argument as the block above: a file written before #1243 has no
            // Specular node, every field keeps its default, and such a file is
            // at transport version 0, 1 or 2 — where the lobe mixture and the
            // detail normal are not evaluated at all. So a .oloskin authored
            // against any earlier version round-trips to the SAME PIXELS, which
            // is the prior-on-disk-version cell the verification grid asks for
            // and SkinProfileSerializerTest asserts directly.
            //
            // NOTE that NormalVarianceStrength's default is 0.5 and not 0, so an
            // old file loaded here does acquire a non-zero value in that field.
            // That is not a behaviour change for it: the field is only read at
            // version 3, and an old file is not at version 3. It matters only if
            // someone ALSO moves the version, which is an authoring act and the
            // point at which the new transport is supposed to start applying.
            if (auto specular = section["Specular"]; specular)
            {
                parameters.Specular.LobeMix =
                    ReadFinite(specular["LobeMix"], defaults.Specular.LobeMix, "Specular.LobeMix");
                parameters.Specular.LobeRoughnessScale = ReadFinite(
                    specular["LobeRoughnessScale"], defaults.Specular.LobeRoughnessScale, "Specular.LobeRoughnessScale");
                parameters.Specular.NormalVarianceStrength =
                    ReadFinite(specular["NormalVarianceStrength"], defaults.Specular.NormalVarianceStrength,
                               "Specular.NormalVarianceStrength");
                parameters.Specular.DetailStrength = ReadFinite(
                    specular["DetailStrength"], defaults.Specular.DetailStrength, "Specular.DetailStrength");
                parameters.Specular.ExpressionDetailGain =
                    ReadFinite(specular["ExpressionDetailGain"], defaults.Specular.ExpressionDetailGain,
                               "Specular.ExpressionDetailGain");
            }

            // The oral surface (issue #1245). Same shape and same argument as
            // the two blocks above: a file written before #1245 has no Oral
            // node, every field keeps its default, and such a file is at
            // transport version 0..3 where the coat and the cavity weight are
            // not evaluated at all. So a .oloskin authored against any earlier
            // version round-trips to the SAME PIXELS.
            //
            // CoatRoughness and CoatIor default to NON-ZERO values (0.1 and
            // saliva's 1.33), so an old file loaded here does acquire them. That
            // is not a behaviour change for it, for the reason the note above
            // gives about NormalVarianceStrength: the fields are read only at
            // version 4, and CoatStrength — the field that decides whether they
            // are read at all — still defaults to 0.
            if (auto oral = section["Oral"]; oral)
            {
                parameters.Oral.CoatStrength =
                    ReadFinite(oral["CoatStrength"], defaults.Oral.CoatStrength, "Oral.CoatStrength");
                parameters.Oral.CoatRoughness =
                    ReadFinite(oral["CoatRoughness"], defaults.Oral.CoatRoughness, "Oral.CoatRoughness");
                parameters.Oral.CoatIor = ReadFinite(oral["CoatIor"], defaults.Oral.CoatIor, "Oral.CoatIor");
                parameters.Oral.CavityOcclusion =
                    ReadFinite(oral["CavityOcclusion"], defaults.Oral.CavityOcclusion, "Oral.CavityOcclusion");
            }

            // The eye (issue #1244). Same shape and same argument as the three
            // blocks above: a file written before #1244 has no Ocular node,
            // every field keeps its default, and such a file is at transport
            // version 0..4 where none of them is evaluated. So a .oloskin
            // authored against any earlier version round-trips to the SAME
            // PIXELS — which is the prior-on-disk-version cell of this task's
            // verification matrix, and SkinProfileTest pins it as an assertion
            // rather than as a claim.
            //
            // NINE OF THE TWELVE DEFAULT TO NON-ZERO — the five clinical
            // lengths, the index, the ring width, the pupil darkening and
            // RefractionStrength — so an old file loaded here does acquire
            // them. That is not a behaviour change for it, for the reason the
            // note above gives about CoatRoughness: they are read only at
            // version 5, and OcularStrength, the field that decides whether
            // they are read at all, still defaults to 0.
            if (auto ocular = section["Ocular"]; ocular)
            {
                const auto read = [&](const char* key, f32 fallback, const char* label)
                {
                    return ReadFinite(ocular[key], fallback, label);
                };
                parameters.Ocular.OcularStrength =
                    read("OcularStrength", defaults.Ocular.OcularStrength, "Ocular.OcularStrength");
                parameters.Ocular.RefractionStrength =
                    read("RefractionStrength", defaults.Ocular.RefractionStrength, "Ocular.RefractionStrength");
                parameters.Ocular.EyeRadiusMM =
                    read("EyeRadiusMM", defaults.Ocular.EyeRadiusMM, "Ocular.EyeRadiusMM");
                parameters.Ocular.CorneaRadiusMM =
                    read("CorneaRadiusMM", defaults.Ocular.CorneaRadiusMM, "Ocular.CorneaRadiusMM");
                parameters.Ocular.IrisRadiusMM =
                    read("IrisRadiusMM", defaults.Ocular.IrisRadiusMM, "Ocular.IrisRadiusMM");
                parameters.Ocular.PupilRadiusMM =
                    read("PupilRadiusMM", defaults.Ocular.PupilRadiusMM, "Ocular.PupilRadiusMM");
                parameters.Ocular.IrisPlaneDepthMM =
                    read("IrisPlaneDepthMM", defaults.Ocular.IrisPlaneDepthMM, "Ocular.IrisPlaneDepthMM");
                parameters.Ocular.CorneaIor =
                    read("CorneaIor", defaults.Ocular.CorneaIor, "Ocular.CorneaIor");
                parameters.Ocular.LimbalRingWidthMM =
                    read("LimbalRingWidthMM", defaults.Ocular.LimbalRingWidthMM, "Ocular.LimbalRingWidthMM");
                parameters.Ocular.LimbalRingStrength =
                    read("LimbalRingStrength", defaults.Ocular.LimbalRingStrength, "Ocular.LimbalRingStrength");
                parameters.Ocular.PupilDarkening =
                    read("PupilDarkening", defaults.Ocular.PupilDarkening, "Ocular.PupilDarkening");
                parameters.Ocular.IrisConcavity =
                    read("IrisConcavity", defaults.Ocular.IrisConcavity, "Ocular.IrisConcavity");
                parameters.Ocular.IrisColor = ReadFiniteVec3(ocular["IrisColor"], defaults.Ocular.IrisColor,
                                                             "Ocular.IrisColor");
            }

            if (!profile->SetParameters(parameters))
            {
                OLO_CORE_WARN("SkinProfileSerializer - '{}' had out-of-range parameters; they were clamped on load.",
                              profile->GetName());
            }
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SkinProfileSerializer::DeserializeFromYAML - {}", e.what());
            return false;
        }
    }

    void SkinProfileSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto profile = asset.As<SkinProfile>();
        if (!profile)
        {
            OLO_CORE_ERROR("SkinProfileSerializer::Serialize - asset cast failed ({})", metadata.FilePath.string());
            return;
        }

        const std::string yamlString = SerializeToYAML(profile);
        // metadata.FilePath is stored PROJECT-relative and already carries the
        // "Assets/" segment, so it joins onto the project directory — joining
        // onto GetAssetDirectory() yields ".../Assets/Assets/..." and every load
        // misses.
        auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("SkinProfileSerializer::Serialize - mkdir failed ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("SkinProfileSerializer::Serialize - open-for-write failed ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool SkinProfileSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("SkinProfileSerializer::TryLoadData - file missing ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("SkinProfileSerializer::TryLoadData - open failed ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto profile = Ref<SkinProfile>::Create();
        if (!DeserializeFromYAML(ss.str(), profile))
        {
            OLO_CORE_ERROR("SkinProfileSerializer::TryLoadData - YAML parse failed ({})", path.string());
            return false;
        }

        profile->SetHandle(metadata.Handle);
        asset = profile;
        return true;
    }

    bool SkinProfileSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto profile = AssetManager::GetAsset<SkinProfile>(handle);
        if (!profile)
        {
            OLO_CORE_ERROR("SkinProfileSerializer::SerializeToAssetPack - get-asset failed ({})", static_cast<u64>(handle));
            return false;
        }
        const std::string yamlString = SerializeToYAML(profile);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> SkinProfileSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto profile = Ref<SkinProfile>::Create();
        if (!DeserializeFromYAML(yamlString, profile))
        {
            OLO_CORE_ERROR("SkinProfileSerializer::DeserializeFromAssetPack - YAML parse failed (handle: {})",
                           static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }
        profile->SetHandle(assetInfo.Handle);
        return profile;
    }
} // namespace OloEngine
