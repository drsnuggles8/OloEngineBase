#include "OloEnginePCH.h"
#include "BenchmarkManifest.h"

#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/YAMLConverters.h"
#include "OloEngine/Renderer/PathTracing/GpuPathTracerTypes.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

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

        // ---- ManifestVersion 2 asset provenance (issue #1239) ---------------
        // Each provenance field is a CLOSED vocabulary, parsed through one
        // helper so a typo ("comitted") is a named parse error rather than a
        // silent fall-through to the first enumerator. That matters more here
        // than anywhere else in the schema: the redistribution class is what
        // decides whether bytes are allowed in a public repository.
        template<typename T>
        std::optional<T> ParseEnumField(const YAML::Node& node, std::string_view key,
                                        std::initializer_list<std::pair<std::string_view, T>> vocabulary,
                                        const std::string& context, bool required, ErrorList& errors)
        {
            const auto allowedList = [&vocabulary]
            {
                std::string allowed;
                for (const auto& option : vocabulary)
                {
                    allowed += (allowed.empty() ? "" : " | ");
                    allowed += option.first;
                }
                return allowed;
            };

            const auto field = node[std::string(key)];
            if (!field)
            {
                if (required)
                {
                    errors.Add(context + ": " + std::string(key) + " is required in ManifestVersion 2 (" +
                               allowedList() + ")");
                }
                return std::nullopt;
            }
            const auto text = field.as<std::string>("");
            for (const auto& option : vocabulary)
            {
                if (text == option.first)
                {
                    return option.second;
                }
            }
            errors.Add(context + ": " + std::string(key) + " must be one of " + allowedList() + " (got '" + text +
                       "')");
            return std::nullopt;
        }

        // 64 lowercase hex characters. Deliberately strict about case so the
        // value a user pastes from `sha256sum` compares byte-for-byte against
        // the manifest without a normalisation step nobody would remember.
        bool IsSha256Hex(std::string_view value)
        {
            return value.size() == 64 && std::ranges::all_of(value,
                                                             [](char c)
                                                             {
                                                                 return (c >= '0' && c <= '9') ||
                                                                        (c >= 'a' && c <= 'f');
                                                             });
        }

        void ParseAssetProvenance(const YAML::Node& entry, const std::string& context, bool required,
                                  ManifestAssetRecord& record, ErrorList& errors)
        {
            record.Redistribution = ParseEnumField<AssetRedistribution>(
                entry, "Redistribution",
                { { "committed", AssetRedistribution::Committed },
                  { "fetch-required", AssetRedistribution::FetchRequired },
                  { "local-only", AssetRedistribution::LocalOnly } },
                context, required, errors);
            record.LicenseVerified = ParseEnumField<LicenseVerification>(
                entry, "LicenseVerified",
                { { "in-repo-file", LicenseVerification::InRepoFile },
                  { "upstream-declared", LicenseVerification::UpstreamDeclared },
                  { "unverified", LicenseVerification::Unverified } },
                context, required, errors);
            record.Units = ParseEnumField<AssetUnits>(entry, "Units",
                                                      { { "metres", AssetUnits::Metres },
                                                        { "centimetres", AssetUnits::Centimetres },
                                                        { "unitless", AssetUnits::Unitless } },
                                                      context, required, errors);
            record.UpAxis = ParseEnumField<AssetUpAxis>(entry, "UpAxis",
                                                        { { "+Y", AssetUpAxis::YUp },
                                                          { "+Z", AssetUpAxis::ZUp },
                                                          { "n/a", AssetUpAxis::NotApplicable } },
                                                        context, required, errors);
            record.ColorSpace = ParseEnumField<AssetColorSpace>(entry, "ColorSpace",
                                                                { { "srgb", AssetColorSpace::Srgb },
                                                                  { "linear", AssetColorSpace::Linear },
                                                                  { "n/a", AssetColorSpace::NotApplicable } },
                                                                context, required, errors);

            record.Version = entry["Version"].as<std::string>("");
            record.Sha256 = entry["Sha256"].as<std::string>("");
            record.Acquisition = entry["Acquisition"].as<std::string>("");

            if (!required)
            {
                // v1 manifests may carry the fields but are not held to them —
                // still validate the Sha256 FORMAT when one is present, since a
                // malformed hash is useless in either version.
                if (!record.Sha256.empty() && !IsSha256Hex(record.Sha256))
                {
                    errors.Add(context + ": Sha256 must be 64 lowercase hex characters");
                }
                return;
            }

            if (record.Origin.empty())
            {
                errors.Add(context + ": Origin is required in ManifestVersion 2");
            }
            if (record.Version.empty())
            {
                errors.Add(context + ": Version is required in ManifestVersion 2 "
                                     "(upstream tag/commit, or 'generated' for in-repo output)");
            }
            if (!IsSha256Hex(record.Sha256))
            {
                errors.Add(context + ": Sha256 must be 64 lowercase hex characters "
                                     "(tools/benchmark/reference_assets.py --write-hashes fills these in)");
            }
            if (record.Acquisition.empty())
            {
                errors.Add(context + ": Acquisition is required in ManifestVersion 2 "
                                     "(the reproducible local acquisition path — a URL, or the procedure for a "
                                     "local-only asset)");
            }
            // The one cross-field rule worth enforcing: claiming a licence was
            // verified against an in-repo file only means something if the
            // bytes are actually in the repo.
            if (record.Redistribution && record.LicenseVerified &&
                *record.LicenseVerified == LicenseVerification::InRepoFile &&
                *record.Redistribution != AssetRedistribution::Committed)
            {
                errors.Add(context + ": LicenseVerified 'in-repo-file' requires Redistribution 'committed' "
                                     "(there is no in-repo licence file for an asset this repo does not ship)");
            }
        }

        // ManifestVersion 2: per-frame camera movement (issue #1239).
        std::optional<ManifestCameraMotion> ParseCameraMotion(const YAML::Node& node, const std::string& parentContext,
                                                              u32 manifestVersion, ErrorList& errors)
        {
            const std::string context = parentContext + ".Motion";
            if (manifestVersion < 2u)
            {
                errors.Add(context + ": camera Motion requires ManifestVersion 2");
                return std::nullopt;
            }
            RequireKnownKeys(node, { "VelocityPerSecond", "YawRateDegreesPerSecond", "PitchRateDegreesPerSecond" },
                             context, errors);

            ManifestCameraMotion motion;
            bool decoded = true;
            // `.as<T>(fallback)` SWALLOWS a decode failure and hands back the
            // fallback, so `VelocityPerSecond: [.nan, 0, 0]` — or any typo —
            // would read as a legal zero and the shot would silently not move.
            // Decode without a fallback and report the failure by name instead.
            const auto decodeInto = [&node, &context, &errors, &decoded]<typename T>(std::string_view key, T& out)
            {
                const auto field = node[std::string(key)];
                if (!field)
                {
                    return; // absent: the member keeps its declared default
                }
                try
                {
                    out = field.as<T>();
                }
                catch (const YAML::Exception&)
                {
                    errors.Add(context + ": " + std::string(key) +
                               " is not a valid value (a malformed number must not read as zero here — "
                               "that would turn a moving shot into a still one)");
                    decoded = false;
                }
            };
            decodeInto("VelocityPerSecond", motion.VelocityPerSecond);
            decodeInto("YawRateDegreesPerSecond", motion.YawRateDegreesPerSecond);
            decodeInto("PitchRateDegreesPerSecond", motion.PitchRateDegreesPerSecond);
            if (!decoded)
            {
                return std::nullopt;
            }

            if (!std::isfinite(motion.VelocityPerSecond.x) || !std::isfinite(motion.VelocityPerSecond.y) ||
                !std::isfinite(motion.VelocityPerSecond.z) || !std::isfinite(motion.YawRateDegreesPerSecond) ||
                !std::isfinite(motion.PitchRateDegreesPerSecond))
            {
                errors.Add(context + ": non-finite float value");
                return std::nullopt;
            }
            // An all-zero Motion block is a still shot wearing a moving shot's
            // clothes: the manifest would advertise a velocity-buffer stress
            // that the capture does not produce. Say so instead of shrugging.
            if (motion.VelocityPerSecond == glm::vec3(0.0f) && motion.YawRateDegreesPerSecond == 0.0f &&
                motion.PitchRateDegreesPerSecond == 0.0f)
            {
                errors.Add(context + ": all motion rates are zero — omit the Motion block for a still camera");
            }
            return motion;
        }

        std::optional<ManifestCamera> ParseCamera(const YAML::Node& node, ErrorList& errors, sizet index,
                                                  u32 manifestVersion)
        {
            const std::string context = "Cameras[" + std::to_string(index) + "]";
            RequireKnownKeys(node,
                             { "Id", "Position", "YawDegrees", "PitchDegrees", "FovDegrees", "Near", "Far",
                               "WarmupFrames", "Motion" },
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
            if (const auto motion = node["Motion"]; motion)
            {
                camera.Motion = ParseCameraMotion(motion, context, manifestVersion, errors);
            }
            return camera;
        }
    } // namespace

    ManifestCameraPose CameraPoseAtFrame(const ManifestCamera& camera, u32 frameInCamera, f32 fixedDtSeconds)
    {
        ManifestCameraPose pose;
        pose.Position = camera.Position;
        pose.YawDegrees = camera.YawDegrees;
        pose.PitchDegrees = camera.PitchDegrees;
        if (!camera.Motion)
        {
            return pose;
        }
        // Integrate from the DECLARED pose rather than accumulating frame to
        // frame: a closed form gives every host the same answer regardless of
        // where it starts or how it batches frames, and cannot drift.
        const f32 elapsed = static_cast<f32>(frameInCamera) * fixedDtSeconds;
        pose.Position += camera.Motion->VelocityPerSecond * elapsed;
        pose.YawDegrees += camera.Motion->YawRateDegreesPerSecond * elapsed;
        pose.PitchDegrees += camera.Motion->PitchRateDegreesPerSecond * elapsed;
        return pose;
    }

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
        if (manifest.ManifestVersion != 1u && manifest.ManifestVersion != 2u)
        {
            // v1: issue #974's capture schema. v2: adds asset provenance and
            // camera motion (issue #1239). Both stay readable — a v1 manifest
            // is not silently upgraded, because the v2 provenance fields are
            // REQUIRED and a silent upgrade would turn every existing manifest
            // into a parse error at an unrelated moment.
            errors.Add("ManifestVersion must be 1 or 2 (got " + std::to_string(manifest.ManifestVersion) + ")");
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
            if (auto parsed = ParseCamera(camera, errors, 0, manifest.ManifestVersion))
            {
                manifest.Cameras.push_back(std::move(*parsed));
            }
        }
        else if (const auto cameras = root["Cameras"]; cameras && cameras.IsSequence())
        {
            sizet index = 0;
            for (const auto& entry : cameras)
            {
                if (auto parsed = ParseCamera(entry, errors, index++, manifest.ManifestVersion))
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
                               "HZBOcclusionCullingEnabled", "TAAEnabled", "GpuPathTracerEnabled",
                               "GpuPathTracerSamplesPerFrame" },
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
            if (rs["GpuPathTracerEnabled"])
            {
                // Decoded explicitly: `as<bool>(false)` would turn a typo
                // ("ture", a number) into a silently disabled tracer, and a
                // benchmark that meant to measure it would measure the raster.
                bool enabled = false;
                if (!rs["GpuPathTracerEnabled"].IsScalar() ||
                    !YAML::convert<bool>::decode(rs["GpuPathTracerEnabled"], enabled))
                {
                    errors.Add("RendererSettings.GpuPathTracerEnabled must be true or false");
                }
                else
                {
                    manifest.RendererSettings.GpuPathTracerEnabled = enabled;
                }
            }
            if (rs["GpuPathTracerSamplesPerFrame"])
            {
                const auto samples = rs["GpuPathTracerSamplesPerFrame"].as<i64>(0);
                if (samples < 1 || samples > static_cast<i64>(kGpuPathTracerMaxSamplesPerFrame))
                {
                    errors.Add("RendererSettings.GpuPathTracerSamplesPerFrame must be in [1, " +
                               std::to_string(kGpuPathTracerMaxSamplesPerFrame) + "]");
                }
                else
                {
                    manifest.RendererSettings.GpuPathTracerSamplesPerFrame = static_cast<u32>(samples);
                }
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
            const bool requireProvenance = manifest.ManifestVersion >= 2u;
            sizet index = 0;
            for (const auto& entry : assets)
            {
                const std::string context = "Assets[" + std::to_string(index++) + "]";
                RequireKnownKeys(entry,
                                 { "Path", "Origin", "License", "Redistribution", "LicenseVerified", "Units",
                                   "UpAxis", "ColorSpace", "Version", "Sha256", "Acquisition" },
                                 context, errors);
                ManifestAssetRecord record;
                record.Path = entry["Path"].as<std::string>("");
                record.Origin = entry["Origin"].as<std::string>("");
                record.License = entry["License"].as<std::string>("");
                if (record.Path.empty() || record.License.empty())
                {
                    errors.Add(context + ": Path and License are required "
                                         "(recording asset origin/license is an issue-#974 acceptance criterion)");
                }
                ParseAssetProvenance(entry, context, requireProvenance, record, errors);
                manifest.Assets.push_back(std::move(record));
            }
        }
        else if (manifest.ManifestVersion >= 2u)
        {
            // A v2 manifest with no Assets block would satisfy "every asset is
            // documented" vacuously. Every fixture renders SOMETHING, so an
            // empty provenance list means the block was forgotten.
            errors.Add("Assets is required and must be a non-empty sequence in ManifestVersion 2 "
                       "(every asset a fixture renders carries provenance — issue #1239)");
        }

        if (errors.Any)
        {
            outError = path.string() + ":\n" + errors.Out.str();
            return std::nullopt;
        }
        return manifest;
    }
} // namespace OloEngine::Benchmark
