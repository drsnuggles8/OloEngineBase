#include "OloEnginePCH.h"
#include "BenchmarkManifest.h"

#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace OloEngine::Benchmark
{
    namespace
    {
        // Collects every problem instead of stopping at the first, so one
        // parse round-trip reports the whole repair list.
        struct ErrorList
        {
            std::ostringstream Out;
            bool Any = false;

            void Add(const std::string& message)
            {
                Out << (Any ? "\n" : "") << message;
                Any = true;
            }
        };

        // Strict-map guard: every key in `node` must appear in `allowed`.
        void RequireKnownKeys(const YAML::Node& node, std::initializer_list<std::string_view> allowed,
                              std::string_view context, ErrorList& errors)
        {
            if (!node.IsMap())
            {
                return;
            }
            for (const auto& kv : node)
            {
                const auto key = kv.first.as<std::string>("");
                if (std::ranges::find(allowed, key) == allowed.end())
                {
                    errors.Add(std::string(context) + ": unknown key '" + key +
                               "' (unknown keys are fatal — see docs/guides/renderer-benchmarks.md)");
                }
            }
        }

        bool IsPlainId(std::string_view value)
        {
            return !value.empty() && std::ranges::all_of(value,
                                                         [](char c)
                                                         {
                                                             return (c >= 'a' && c <= 'z') ||
                                                                    (c >= '0' && c <= '9') || c == '-';
                                                         });
        }

        // File-stem safety for attachment names (they become <Name>.png/.hdr).
        bool IsPlainFileStem(std::string_view value)
        {
            return !value.empty() && std::ranges::all_of(value,
                                                         [](char c)
                                                         {
                                                             return (c >= 'a' && c <= 'z') ||
                                                                    (c >= 'A' && c <= 'Z') ||
                                                                    (c >= '0' && c <= '9') || c == '-' ||
                                                                    c == '_';
                                                         });
        }

        std::optional<ManifestCamera> ParseCamera(const YAML::Node& node, ErrorList& errors, sizet index)
        {
            const std::string context = "Cameras[" + std::to_string(index) + "]";
            RequireKnownKeys(node,
                             { "Id", "Position", "YawDegrees", "PitchDegrees", "FovDegrees", "Near", "Far",
                               "WarmupFrames" },
                             context, errors);

            ManifestCamera camera;
            camera.Id = node["Id"].as<std::string>("");
            if (!IsPlainId(camera.Id))
            {
                errors.Add(context + ": Id must be non-empty [a-z0-9-]");
            }
            if (!node["Position"])
            {
                errors.Add(context + ": Position is required");
            }
            else
            {
                camera.Position = node["Position"].as<glm::vec3>(glm::vec3(0.0f));
            }
            camera.YawDegrees = node["YawDegrees"].as<f32>(0.0f);
            camera.PitchDegrees = node["PitchDegrees"].as<f32>(0.0f);
            camera.FovDegrees = node["FovDegrees"].as<f32>(60.0f);
            camera.NearClip = node["Near"].as<f32>(0.05f);
            camera.FarClip = node["Far"].as<f32>(1000.0f);
            if (node["WarmupFrames"])
            {
                camera.WarmupFrames = node["WarmupFrames"].as<u32>(0u);
                if (*camera.WarmupFrames == 0u)
                {
                    // Zero frames between a camera cut and its capture would
                    // export the PREVIOUS camera's frame under this camera's
                    // id — a silently mislabeled image (and a typo'd
                    // non-numeric value coerces to 0 via the default).
                    errors.Add(context + ": WarmupFrames must be >= 1");
                }
            }

            const bool finite = std::isfinite(camera.Position.x) && std::isfinite(camera.Position.y) &&
                                std::isfinite(camera.Position.z) && std::isfinite(camera.YawDegrees) &&
                                std::isfinite(camera.PitchDegrees) && std::isfinite(camera.FovDegrees) &&
                                std::isfinite(camera.NearClip) && std::isfinite(camera.FarClip);
            if (!finite)
            {
                errors.Add(context + ": non-finite float value");
            }
            if (camera.FovDegrees <= 1.0f || camera.FovDegrees >= 179.0f)
            {
                errors.Add(context + ": FovDegrees must be in (1, 179)");
            }
            if (!(camera.NearClip > 0.0f) || !(camera.FarClip > camera.NearClip))
            {
                errors.Add(context + ": need 0 < Near < Far");
            }
            return camera;
        }
    } // namespace

    bool BenchmarkManifest::SupportsBackend(std::string_view backend) const
    {
        return std::ranges::find(SupportedBackends, backend) != SupportedBackends.end();
    }

    const std::vector<std::string>* BenchmarkManifest::UnsupportedFor(std::string_view backend) const
    {
        const auto it = UnsupportedAttachments.find(std::string(backend));
        return it != UnsupportedAttachments.end() ? &it->second : nullptr;
    }

    std::optional<BenchmarkManifest> LoadBenchmarkManifest(const std::filesystem::path& path, std::string& outError)
    {
        outError.clear();

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            outError = "cannot open manifest file: " + path.string();
            return std::nullopt;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string bytes = buffer.str();

        YAML::Node root;
        try
        {
            root = YAML::Load(bytes);
        }
        catch (const YAML::Exception& e)
        {
            outError = "YAML parse error in " + path.string() + ": " + e.what();
            return std::nullopt;
        }
        if (!root.IsMap())
        {
            outError = "manifest root must be a YAML map: " + path.string();
            return std::nullopt;
        }

        ErrorList errors;
        RequireKnownKeys(root,
                         { "ManifestVersion", "Id", "Product", "Scene", "Backends", "Camera", "Cameras", "Output",
                           "RendererSettings", "Exposure", "Determinism", "Warmup", "Attachments", "Tolerance",
                           "Assets", "Description" },
                         "manifest", errors);

        BenchmarkManifest manifest;
        // FNV-1a 64 over the manifest file's bytes — names a manifest revision
        // in result.json (provenance metadata, not security).
        manifest.SourceHash = Hash::FNV1a64(bytes.data(), bytes.size());

        manifest.ManifestVersion = root["ManifestVersion"].as<u32>(0u);
        if (manifest.ManifestVersion != 1u)
        {
            errors.Add("ManifestVersion must be 1 (got " + std::to_string(manifest.ManifestVersion) + ")");
        }

        manifest.Id = root["Id"].as<std::string>("");
        if (!IsPlainId(manifest.Id))
        {
            errors.Add("Id must be non-empty [a-z0-9-] (it names the result directory)");
        }

        if (const auto product = root["Product"].as<std::string>(""); product == "golden")
        {
            manifest.Product = ManifestProduct::Golden;
        }
        else if (product == "diagnostic")
        {
            manifest.Product = ManifestProduct::Diagnostic;
        }
        else if (product == "hero")
        {
            manifest.Product = ManifestProduct::Hero;
        }
        else
        {
            errors.Add("Product must be golden | diagnostic | hero");
        }

        manifest.ScenePath = root["Scene"].as<std::string>("");
        if (manifest.ScenePath.empty())
        {
            errors.Add("Scene is required (project-relative .olo path)");
        }

        if (const auto backends = root["Backends"]; backends)
        {
            RequireKnownKeys(backends, { "Supported", "Features" }, "Backends", errors);
            for (const auto& b : backends["Supported"])
            {
                const auto name = b.as<std::string>("");
                if (name != "opengl" && name != "vulkan")
                {
                    errors.Add("Backends.Supported entries must be opengl | vulkan (got '" + name + "')");
                }
                manifest.SupportedBackends.push_back(name);
            }
            if (const auto features = backends["Features"]; features && features.IsMap())
            {
                for (const auto& kv : features)
                {
                    const auto backendName = kv.first.as<std::string>("");
                    RequireKnownKeys(kv.second, { "UnsupportedAttachments" },
                                     "Backends.Features." + backendName, errors);
                    auto& list = manifest.UnsupportedAttachments[backendName];
                    for (const auto& att : kv.second["UnsupportedAttachments"])
                    {
                        list.push_back(att.as<std::string>(""));
                    }
                }
            }
        }
        if (manifest.SupportedBackends.empty())
        {
            errors.Add("Backends.Supported must list at least one backend");
        }

        if (root["Camera"] && root["Cameras"])
        {
            errors.Add("declare either Camera (single) or Cameras (list), not both");
        }
        if (const auto camera = root["Camera"]; camera)
        {
            if (auto parsed = ParseCamera(camera, errors, 0))
            {
                manifest.Cameras.push_back(std::move(*parsed));
            }
        }
        else if (const auto cameras = root["Cameras"]; cameras && cameras.IsSequence())
        {
            sizet index = 0;
            for (const auto& entry : cameras)
            {
                if (auto parsed = ParseCamera(entry, errors, index++))
                {
                    manifest.Cameras.push_back(std::move(*parsed));
                }
            }
        }
        if (manifest.Cameras.empty())
        {
            errors.Add("at least one camera is required (Camera: or Cameras:)");
        }
        for (sizet i = 0; i < manifest.Cameras.size(); ++i)
        {
            for (sizet j = i + 1; j < manifest.Cameras.size(); ++j)
            {
                if (manifest.Cameras[i].Id == manifest.Cameras[j].Id)
                {
                    errors.Add("duplicate camera Id '" + manifest.Cameras[i].Id + "'");
                }
            }
        }

        if (const auto output = root["Output"]; output)
        {
            RequireKnownKeys(output, { "Resolution", "RenderScale" }, "Output", errors);
            if (const auto res = output["Resolution"]; res && res.IsSequence() && res.size() == 2)
            {
                manifest.Width = res[0].as<u32>(0u);
                manifest.Height = res[1].as<u32>(0u);
            }
            else
            {
                errors.Add("Output.Resolution must be [width, height]");
            }
            manifest.RenderScale = output["RenderScale"].as<f32>(1.0f);
        }
        else
        {
            errors.Add("Output is required");
        }
        if (manifest.Width < 64 || manifest.Height < 64 || manifest.Width > 8192 || manifest.Height > 8192)
        {
            errors.Add("Output.Resolution must be within [64, 8192] per axis");
        }
        // The capture readback reads whole textures; a render scale below 1.0
        // renders into a sub-viewport of those textures and would silently
        // capture the dead margin (docs/agent-rules/notes-renderer.md §4).
        // The isfinite check is load-bearing: NaN makes the epsilon compare
        // FALSE, which would pass a corrupt manifest straight through the
        // "must be 1.0" gate into Renderer3D::SetRenderScale.
        if (!std::isfinite(manifest.RenderScale) || std::abs(manifest.RenderScale - 1.0f) > 1e-6f)
        {
            errors.Add("Output.RenderScale must be a finite 1.0 in schema v1 (sub-scale capture is not supported)");
        }

        if (const auto rs = root["RendererSettings"]; rs)
        {
            RequireKnownKeys(rs,
                             { "Path", "EnableDDGI", "DepthPrepassEnabled", "OcclusionCullingEnabled",
                               "HZBOcclusionCullingEnabled", "TAAEnabled" },
                             "RendererSettings", errors);
            if (rs["Path"])
            {
                const auto pathName = rs["Path"].as<std::string>("");
                if (pathName == "Forward")
                {
                    manifest.RendererSettings.Path = RenderingPath::Forward;
                }
                else if (pathName == "ForwardPlus")
                {
                    manifest.RendererSettings.Path = RenderingPath::ForwardPlus;
                }
                else if (pathName == "Deferred")
                {
                    manifest.RendererSettings.Path = RenderingPath::Deferred;
                }
                else
                {
                    errors.Add("RendererSettings.Path must be Forward | ForwardPlus | Deferred");
                }
            }
            if (rs["EnableDDGI"])
            {
                manifest.RendererSettings.EnableDDGI = rs["EnableDDGI"].as<bool>(false);
            }
            if (rs["DepthPrepassEnabled"])
            {
                manifest.RendererSettings.DepthPrepassEnabled = rs["DepthPrepassEnabled"].as<bool>(false);
            }
            if (rs["OcclusionCullingEnabled"])
            {
                manifest.RendererSettings.OcclusionCullingEnabled = rs["OcclusionCullingEnabled"].as<bool>(false);
            }
            if (rs["HZBOcclusionCullingEnabled"])
            {
                manifest.RendererSettings.HZBOcclusionCullingEnabled =
                    rs["HZBOcclusionCullingEnabled"].as<bool>(false);
            }
            if (rs["TAAEnabled"])
            {
                manifest.RendererSettings.TAAEnabled = rs["TAAEnabled"].as<bool>(false);
            }
        }

        if (const auto exposure = root["Exposure"]; exposure)
        {
            RequireKnownKeys(exposure, { "Mode", "Exposure" }, "Exposure", errors);
            if (const auto mode = exposure["Mode"].as<std::string>(""); mode == "Manual")
            {
                manifest.Exposure = ExposureMode::Manual;
            }
            else if (mode == "Auto")
            {
                manifest.Exposure = ExposureMode::Auto;
            }
            else
            {
                errors.Add("Exposure.Mode must be Manual | Auto");
            }
            manifest.ExposureValue = exposure["Exposure"].as<f32>(1.0f);
            if (!std::isfinite(manifest.ExposureValue) || manifest.ExposureValue <= 0.0f)
            {
                errors.Add("Exposure.Exposure must be a finite positive value");
            }
        }
        else
        {
            errors.Add("Exposure is required (Mode + Exposure)");
        }

        if (const auto det = root["Determinism"]; det)
        {
            RequireKnownKeys(det, { "Seed", "StartTimeSeconds", "FixedDtSeconds" }, "Determinism", errors);
            manifest.Seed = det["Seed"].as<u64>(0ull);
            manifest.StartTimeSeconds = det["StartTimeSeconds"].as<f32>(0.0f);
            manifest.FixedDtSeconds = det["FixedDtSeconds"].as<f32>(1.0f / 60.0f);
            if (!std::isfinite(manifest.StartTimeSeconds) || manifest.StartTimeSeconds < 0.0f)
            {
                errors.Add("Determinism.StartTimeSeconds must be finite and >= 0");
            }
            if (!std::isfinite(manifest.FixedDtSeconds) || manifest.FixedDtSeconds <= 0.0f ||
                manifest.FixedDtSeconds > 0.1f)
            {
                errors.Add("Determinism.FixedDtSeconds must be in (0, 0.1] "
                           "(several engine dt accumulators clamp at 0.1s)");
            }
        }
        else
        {
            errors.Add("Determinism is required (Seed + StartTimeSeconds + FixedDtSeconds)");
        }

        if (const auto warmup = root["Warmup"]; warmup)
        {
            RequireKnownKeys(warmup, { "Frames", "PerFeature" }, "Warmup", errors);
            manifest.WarmupFrames = warmup["Frames"].as<u32>(0u);
            if (const auto perFeature = warmup["PerFeature"]; perFeature && perFeature.IsMap())
            {
                for (const auto& kv : perFeature)
                {
                    manifest.WarmupPerFeature[kv.first.as<std::string>("")] = kv.second.as<u32>(0u);
                }
            }
            u32 maxPerFeature = 0;
            for (const auto& [feature, frames] : manifest.WarmupPerFeature)
            {
                maxPerFeature = std::max(maxPerFeature, frames);
            }
            if (manifest.WarmupFrames == 0)
            {
                errors.Add("Warmup.Frames must be >= 1 (the first frame after a resize is black)");
            }
            if (manifest.WarmupFrames < maxPerFeature)
            {
                errors.Add("Warmup.Frames (" + std::to_string(manifest.WarmupFrames) +
                           ") is smaller than the largest Warmup.PerFeature entry (" +
                           std::to_string(maxPerFeature) + ") — the self-description would be a lie");
            }
        }
        else
        {
            errors.Add("Warmup is required (Frames [+ PerFeature])");
        }

        if (const auto attachments = root["Attachments"]; attachments && attachments.IsSequence())
        {
            sizet index = 0;
            for (const auto& entry : attachments)
            {
                const std::string context = "Attachments[" + std::to_string(index++) + "]";
                RequireKnownKeys(entry, { "Name", "Source", "Format", "Normalize", "Derive" }, context, errors);
                ManifestAttachment attachment;
                attachment.Name = entry["Name"].as<std::string>("");
                attachment.Source = entry["Source"].as<std::string>("");
                if (!IsPlainFileStem(attachment.Name))
                {
                    errors.Add(context + ": Name must be a plain file stem [A-Za-z0-9_-]");
                }
                if (attachment.Source.empty())
                {
                    errors.Add(context + ": Source (render-graph resource name) is required");
                }
                if (const auto format = entry["Format"].as<std::string>("png"); format == "png")
                {
                    attachment.Format = AttachmentFormat::Png;
                }
                else if (format == "hdr")
                {
                    attachment.Format = AttachmentFormat::Hdr;
                }
                else
                {
                    errors.Add(context + ": Format must be png | hdr");
                }
                if (const auto normalize = entry["Normalize"].as<std::string>("auto"); normalize == "auto")
                {
                    attachment.Normalize = AttachmentNormalize::Auto;
                }
                else if (normalize == "none")
                {
                    attachment.Normalize = AttachmentNormalize::None;
                }
                else if (normalize == "on")
                {
                    attachment.Normalize = AttachmentNormalize::On;
                }
                else
                {
                    errors.Add(context + ": Normalize must be auto | none | on");
                }
                if (const auto derive = entry["Derive"].as<std::string>("none"); derive == "none")
                {
                    attachment.Derive = AttachmentDerive::None;
                }
                else if (derive == "linear-depth")
                {
                    attachment.Derive = AttachmentDerive::LinearDepth;
                }
                else if (derive == "channel-r")
                {
                    attachment.Derive = AttachmentDerive::ChannelR;
                }
                else if (derive == "channel-g")
                {
                    attachment.Derive = AttachmentDerive::ChannelG;
                }
                else if (derive == "channel-b")
                {
                    attachment.Derive = AttachmentDerive::ChannelB;
                }
                else if (derive == "channel-a")
                {
                    attachment.Derive = AttachmentDerive::ChannelA;
                }
                else
                {
                    errors.Add(context +
                               ": Derive must be none | linear-depth | channel-r | channel-g | channel-b | channel-a");
                }
                manifest.Attachments.push_back(std::move(attachment));
            }
        }
        if (manifest.Attachments.empty())
        {
            errors.Add("Attachments must list at least one output (the Beauty capture)");
        }
        for (sizet i = 0; i < manifest.Attachments.size(); ++i)
        {
            for (sizet j = i + 1; j < manifest.Attachments.size(); ++j)
            {
                if (manifest.Attachments[i].Name == manifest.Attachments[j].Name)
                {
                    errors.Add("duplicate attachment Name '" + manifest.Attachments[i].Name + "'");
                }
            }
        }

        if (const auto tolerance = root["Tolerance"]; tolerance)
        {
            RequireKnownKeys(tolerance, { "RepeatRmse" }, "Tolerance", errors);
            manifest.RepeatRmseTolerance = tolerance["RepeatRmse"].as<f32>(0.0f);
            if (!std::isfinite(manifest.RepeatRmseTolerance) || manifest.RepeatRmseTolerance < 0.0f)
            {
                errors.Add("Tolerance.RepeatRmse must be finite and >= 0");
            }
        }
        else
        {
            errors.Add("Tolerance is required (RepeatRmse — the documented run-twice bound)");
        }

        if (const auto assets = root["Assets"]; assets && assets.IsSequence())
        {
            sizet index = 0;
            for (const auto& entry : assets)
            {
                const std::string context = "Assets[" + std::to_string(index++) + "]";
                RequireKnownKeys(entry, { "Path", "Origin", "License" }, context, errors);
                ManifestAssetRecord record;
                record.Path = entry["Path"].as<std::string>("");
                record.Origin = entry["Origin"].as<std::string>("");
                record.License = entry["License"].as<std::string>("");
                if (record.Path.empty() || record.License.empty())
                {
                    errors.Add(context + ": Path and License are required "
                                         "(recording asset origin/license is an issue-#974 acceptance criterion)");
                }
                manifest.Assets.push_back(std::move(record));
            }
        }

        if (errors.Any)
        {
            outError = path.string() + ":\n" + errors.Out.str();
            return std::nullopt;
        }
        return manifest;
    }
} // namespace OloEngine::Benchmark
