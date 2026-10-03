#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpFrameGraphDeclarationStats.h"
#include "MCP/McpGpuBufferRead.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpTextureProbe.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/StorageBufferRegistry.h"
#include "OloEngine/Renderer/Texture.h"

#include <string>
#include <vector>

// Resource-probe MCP tools (issue #607): the frame-graph declaration cache's
// counters, asset-texture texel reads and storage-buffer reads. A TU of their
// own rather than more of McpToolsRender.cpp: none of them reads the frame, and
// that file is the busiest in the MCP module.

namespace OloEngine::MCP
{
    namespace
    {
        // ---- olo_frame_graph_declaration_stats (main-marshaled) --------------
        // How the render-graph declaration cache behaved since the last reset
        // (issue #1333). The shaping, including what a zero means with verify
        // mode off, lives in McpFrameGraphDeclarationStats.h.
        ToolResult Handle_FrameGraphDeclarationStats(IAutomationHost& host, const Json& args)
        {
            const bool reset = args.contains("reset") && args["reset"].is_boolean() && args["reset"].get<bool>();
            Json result = host.MarshalRead(
                [reset]
                {
                    // Read, then reset, in one main-thread job: no frame can land
                    // between the two, so the report is exactly what was cleared.
                    Json report = FrameGraphDeclaration::BuildStatsReport(Renderer3D::GetFrameGraphDeclarationStats(),
                                                                          Levers::VerifyDeclarationCache());
                    if (reset)
                        Renderer3D::ResetFrameGraphDeclarationStats();
                    report["reset"] = reset;
                    return report;
                });
            return ToolResult::Structured(result);
        }

        [[nodiscard]] const char* BackendToken()
        {
            return RendererAPI::GetAPI() == RendererAPI::API::Vulkan ? "vulkan" : "opengl";
        }

        // ---- olo_texture_probe (main-marshaled) ------------------------------
        // Texels of an asset Texture2D (issue #607, from #1078). Parsing, the
        // format refusals, the row mapping and what each value means are in
        // McpTextureProbe.h; this resolves the asset and does the read.
        ToolResult Handle_TextureProbe(IAutomationHost& host, const Json& args)
        {
            TextureProbe::Request request;
            if (auto error = TextureProbe::ParseRequest(args, request))
                return ToolResult::Error(*error);

            Json result = host.MarshalRead([request]() -> Json
                                           {
                namespace Probe = TextureProbe;
                if (!Project::GetActive() || !Project::HasAssetManager())
                    return Json{ { "__error", "No project is open." } };

                AssetHandle handle = request.Handle;
                if (handle == 0)
                {
                    auto editorAssets = Project::GetAssetManager().As<EditorAssetManager>();
                    if (!editorAssets)
                        return Json{ { "__error", "'path' needs the editor asset manager; pass 'handle' instead." } };
                    // The registry stores project-relative paths; also accept one
                    // relative to the asset directory, the form the Content
                    // Browser shows.
                    handle = editorAssets->GetAssetHandleFromFilePath(request.Path);
                    if (handle == 0)
                        handle = editorAssets->GetAssetHandleFromFilePath(Project::GetAssetFileSystemPath(request.Path));
                    if (handle == 0)
                        return Json{ { "__error", "No registered asset at '" + request.Path +
                                                      "' (tried project-relative and asset-directory-relative)." } };
                }
                const std::string handleText = std::to_string(static_cast<u64>(handle));
                const AssetType type = AssetManager::GetAssetType(handle);
                if (type == AssetType::None)
                    return Json{ { "__error", "No asset is registered under handle " + handleText + "." } };
                if (type != AssetType::Texture2D)
                    return Json{ { "__error", "Asset " + handleText + " is a " + std::string(AssetUtils::AssetTypeToString(type)) +
                                                  ", not a Texture2D." } };
                // IsAssetValid loads the asset if needed and is false for a file
                // that failed to load. Asked first because GetAsset would hand
                // back the magenta placeholder, and probing that would be a lie.
                if (!AssetManager::IsAssetValid(handle))
                    return Json{ { "__error", "Texture2D asset " + handleText + " is missing or failed to load; see the editor log." } };
                const Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(handle);
                if (!texture || !texture->IsLoaded())
                    return Json{ { "__error", "Texture2D asset " + handleText + " has no GPU storage." } };

                const TextureSpecification& spec = texture->GetSpecification();
                Probe::Texture info;
                info.Handle = static_cast<u64>(handle);
                info.Format = spec.Format;
                info.SRGB = spec.SRGB;
                info.Width = texture->GetWidth();
                info.Height = texture->GetHeight();
                info.MipLevels = texture->GetMipLevelCount();
                info.Path = std::string(texture->GetPath());
                info.Backend = BackendToken();

                const Probe::ReadPlan plan = Probe::PlanRead(spec.Format);
                if (!plan.Ok)
                    return Json{ { "__error", plan.Refusal } };
                if (request.Mip >= info.MipLevels)
                    return Json{ { "__error", "mip " + std::to_string(request.Mip) + " does not exist; the texture has " +
                                                  std::to_string(info.MipLevels) + " level(s)." } };

                const RHI::ResourceHandle rhi = texture->GetRHIHandle();
                RHI::TextureFormatInfo storage;
                if (!RenderCommand::QueryTextureFormat(rhi, request.Mip, storage))
                    return Json{ { "__error", "The backend cannot describe this texture's storage at mip " +
                                                  std::to_string(request.Mip) + "; refusing to read with a guessed layout." } };
                info.StorageFormat = storage.Token;

                u32 mipWidth = 0;
                u32 mipHeight = 0;
                RenderCommand::GetTextureDimensions(rhi, request.Mip, mipWidth, mipHeight);
                if (mipWidth == 0 || mipHeight == 0)
                    return Json{ { "__error", "mip " + std::to_string(request.Mip) + " has no storage." } };

                // Image origin assumes the loader flipped the rows, which only
                // happens to a file-backed texture.
                const Probe::Origin origin = request.RequestedOrigin.value_or(
                    info.Path.empty() ? Probe::Origin::Storage : Probe::Origin::Image);
                const Probe::MappedRegion region = Probe::MapRegion(request, origin, mipWidth, mipHeight);
                if (!region.Ok)
                    return Json{ { "__error", region.Error } };

                const sizet texels = static_cast<sizet>(request.Width) * request.Height;
                std::vector<f32> floats;
                std::vector<i32> ints;
                RHI::Format destination = RHI::Format::RGBA32Float;
                void* destinationData = nullptr;
                sizet destinationBytes = 0;
                if (plan.Kind == Probe::ValueKind::Int)
                {
                    ints.assign(texels, 0);
                    destination = spec.Format == ImageFormat::R32UI ? RHI::Format::R32UInt : RHI::Format::R32Int;
                    destinationData = ints.data();
                    destinationBytes = ints.size() * sizeof(i32);
                }
                else
                {
                    floats.assign(texels * plan.ReadChannels, 0.0f);
                    if (plan.ReadChannels == 1)
                        destination = RHI::Format::R32Float;
                    else if (plan.ReadChannels == 2)
                        destination = RHI::Format::RG32Float;
                    destinationData = floats.data();
                    destinationBytes = floats.size() * sizeof(f32);
                }
                if (!RenderCommand::ReadTextureSubImage(rhi, request.Mip, static_cast<i32>(region.StorageX),
                                                        static_cast<i32>(region.StorageY), 0, request.Width,
                                                        request.Height, 1u, destination, destinationBytes, destinationData))
                    return Json{ { "__error", std::string("Readback of the ") + Probe::FormatToken(spec.Format) + " texture (" +
                                                  storage.Token + " storage) failed on " + BackendToken() + "; see the editor log." } };

                return Probe::BuildReport(info, request, plan, origin, mipWidth, mipHeight, floats, ints); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_gpu_buffer_read (main-marshaled) ----------------------------
        // A byte range of a live storage buffer (issue #607, from #1105).
        // Selection, range checks and decoding are in McpGpuBufferRead.h.
        ToolResult Handle_GpuBufferRead(IAutomationHost& host, const Json& args)
        {
            GpuBufferRead::Request request;
            if (auto error = GpuBufferRead::ParseRequest(args, request))
                return ToolResult::Error(*error);

            Json result = host.MarshalRead([request]() -> Json
                                           {
                namespace Read = GpuBufferRead;
                const TArray<StorageBufferRegistry::Entry> live = StorageBufferRegistry::Get().Snapshot();
                std::vector<Read::Candidate> candidates;
                candidates.reserve(static_cast<sizet>(live.Num()));
                for (const auto& entry : live)
                    candidates.push_back(Read::Candidate{ entry.Id, entry.Buffer->GetBinding(), entry.Buffer->GetSize(),
                                                          entry.Usage, entry.Owner.ToStdString(), entry.Pooled });

                const Read::Selection selection = Read::SelectBuffer(request, candidates);
                if (!selection.Chosen)
                    return Json{ { "__error", selection.Error }, { "candidates", selection.Candidates } };

                const Read::Range range = Read::ResolveRange(request, selection.Chosen->SizeBytes);
                if (!range.Ok)
                    return Json{ { "__error", range.Error } };

                Ref<StorageBuffer> buffer;
                for (const auto& entry : live)
                {
                    if (entry.Id == selection.Chosen->Id)
                        buffer = entry.Buffer;
                }

                // GL: a compute write is visible to glGetNamedBufferSubData only
                // after a buffer-update barrier. Vulkan's GetData does its own
                // flush, copy and fence.
                if (RendererAPI::GetAPI() == RendererAPI::API::OpenGL)
                    RenderCommand::MemoryBarrier(MemoryBarrierFlags::BufferUpdate);

                std::vector<u8> bytes(range.LengthBytes, 0);
                // A failed readback zero-fills; reporting those zeros as the
                // buffer's contents is the exact false answer this tool exists
                // to prevent.
                if (!buffer->GetData(bytes.data(), range.LengthBytes, range.OffsetBytes))
                    return Json{ { "__error", "The readback of buffer " + std::to_string(selection.Chosen->Id) +
                                                  " failed on " + BackendToken() + "; see the editor log. No data was read." } };
                return Read::BuildReport(*selection.Chosen, range, request.DecodeAs, bytes, BackendToken()); });

            if (result.is_object() && result.contains("__error"))
            {
                // The candidates are the useful half of an ambiguous-binding
                // error, so they travel in the message rather than being dropped.
                std::string message = result["__error"].get<std::string>();
                if (result.contains("candidates") && !result["candidates"].empty())
                    message += " Candidates: " + result["candidates"].dump();
                return ToolResult::Error(message);
            }
            return ToolResult::Structured(result);
        }
    } // namespace

    void RegisterResourceTools(AutomationRegistry& registry)
    {
        {
            ToolDef tool;
            tool.Name = "olo_frame_graph_declaration_stats";
            tool.Toolset = "render";
            tool.Title = "Frame-graph declaration cache stats";
            // Mutating only because of the optional 'reset' argument.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "How the render-graph declaration cache has behaved since the last reset (issue #1333): frames, "
                "compiles (the declaration key moved and every pass Setup() re-ran), cache hits, redundant compiles "
                "(a compile whose plan matched the one it replaced: over-invalidation), and, ONLY under the "
                "OLO_RG_VERIFY_DECLARATION_CACHE lever, verified hits and stale-cache detections (a cache hit whose "
                "rebuilt plan differed: a declaration input missing from FrameGraphDeclarationConfig). Check "
                "'anyStale' first, and read 'verifyMode' before trusting a zero there: with verify mode off nothing "
                "is checked. 'lastCompileCause' names the configuration fields (and for PassStates the passes) that "
                "moved the key last. Pass reset:true to clear the counters after this read, for an A/B window.";
            tool.InputSchema =
                Schema::Object()
                    .Prop("reset", Schema::Bool().Desc("Clear the counters after reading them. The response is the pre-reset snapshot."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("verifyMode", Schema::Bool().Desc("OLO_RG_VERIFY_DECLARATION_CACHE is on: cache hits are rebuilt and compared."))
                    .Prop("anyStale", Schema::Bool().Desc("True when a stale-cache detection has been counted."))
                    .Prop("frames", Schema::Int().Min(0))
                    .Prop("compiles", Schema::Int().Min(0))
                    .Prop("cacheHits", Schema::Int().Min(0))
                    .Prop("redundantCompiles", Schema::Int().Min(0).Desc("Compiles whose plan matched the one they replaced (over-invalidation)."))
                    .Prop("verifiedHits", Schema::Int().Min(0).Desc("Verify mode only."))
                    .Prop("staleCacheDetections", Schema::Int().Min(0).Desc("Verify mode only: cache hits whose rebuilt plan differed."))
                    .Prop("cacheHitRate", Schema::Raw(Json{ { "type", Json::array({ "number", "null" }) } }).Desc("cacheHits / (compiles + cacheHits); null before the first frame."))
                    .Prop("timing", Schema::Object()
                                        .Prop("compileMicrosTotal", Schema::Number())
                                        .Prop("cacheHitMicrosTotal", Schema::Number())
                                        .Prop("digestMicrosTotal", Schema::Number())
                                        .Prop("lastCompileMicros", Schema::Number())
                                        .Prop("lastCacheHitMicros", Schema::Number())
                                        .Prop("meanCompileMicros", Schema::NullableNumber())
                                        .Prop("meanCacheHitMicros", Schema::NullableNumber()))
                    .Prop("lastCompileCause", Schema::String().Desc("Configuration fields (and passes, for PassStates) that moved the key on the last compile."))
                    .Prop("lastStaleCacheDetail", Schema::String().Desc("Plan entries that differed on the last stale-cache detection."))
                    .Prop("reset", Schema::Bool().Desc("The counters were cleared after this read."))
                    .Prop("note", Schema::String().Desc("Present when verify mode is off, or when a stale plan was detected."))
                    .Required({ "verifyMode", "anyStale", "frames", "compiles", "cacheHits", "redundantCompiles", "staleCacheDetections", "lastCompileCause", "reset" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_FrameGraphDeclarationStats;
            registry.Register(std::move(tool));
        }

        {
            const Schema::Node texel =
                Schema::Object()
                    .Prop("x", Schema::Int().Min(0))
                    .Prop("y", Schema::Int().Min(0))
                    .Prop("raw", Schema::Array(Schema::Int().Min(0).Max(255)).Desc("unorm8 only: the stored bytes."))
                    .Prop("encoded", Schema::Array(Schema::Number()).Desc("unorm8 only: raw / 255."))
                    .Prop("linear", Schema::Array(Schema::Number()).Desc("unorm8 sRGB only: the decoded value a sampler returns (alpha is never decoded)."))
                    .Prop("value", Schema::Raw(Json::object()).Desc("float: per-channel numbers; int: one integer."));
            ToolDef tool;
            tool.Name = "olo_texture_probe";
            tool.Toolset = "assets";
            tool.Title = "Asset texture probe";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read texels of an ASSET Texture2D on the GPU (the render-target tools cannot reach assets): what the "
                "loader actually uploaded. Pass 'handle' or 'path' (project-relative, or relative to the asset "
                "directory), optional 'mip' and a region x/y/w/h of at most 256 texels. Reports format, sRGB flag, "
                "dimensions, mip count and the backend's storage format. 8-bit texels carry 'raw' (stored byte), "
                "'encoded' (raw/255, the sRGB-encoded value as stored) and, for an sRGB texture, 'linear' (what a "
                "shader samples); float formats carry 'value'. Block-compressed (BCn) textures are REFUSED, never "
                "returned as zeros. origin:'image' (the default for a file-backed texture) addresses the source "
                "image top-left; origin:'storage' addresses rows as uploaded, which the loaders store bottom-up. "
                "Loads the asset if it is not resident yet.";
            tool.InputSchema =
                Schema::Object()
                    .Prop("handle", Schema::Raw(Json{ { "type", Json::array({ "string", "integer" }) } }).Desc("Texture2D asset handle (a decimal string is safest)."))
                    .Prop("path", Schema::String().Desc("Texture file path, project-relative or asset-directory-relative."))
                    .Prop("mip", Schema::Int().Min(0).Desc("Mip level (default 0)."))
                    .Prop("x", Schema::Int().Min(0).Desc("Left texel column (default 0)."))
                    .Prop("y", Schema::Int().Min(0).Desc("Top texel row in the chosen origin (default 0)."))
                    .Prop("w", Schema::Int().Min(1).Desc("Region width (default 1)."))
                    .Prop("h", Schema::Int().Min(1).Desc("Region height (default 1). w*h <= 256."))
                    .Prop("origin", Schema::String().Enum({ "image", "storage" }).Desc("'image': source-file top-left (default for file-backed textures). 'storage': rows as uploaded."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("handle", Schema::String())
                    .Prop("path", Schema::String())
                    .Prop("backend", Schema::String().Enum({ "opengl", "vulkan" }))
                    .Prop("format", Schema::String().Desc("The engine ImageFormat."))
                    .Prop("storageFormat", Schema::String().Desc("The backend's own description of the storage."))
                    .Prop("srgb", Schema::Bool())
                    .Prop("width", Schema::Int().Min(0))
                    .Prop("height", Schema::Int().Min(0))
                    .Prop("mipLevels", Schema::Int().Min(1))
                    .Prop("mip", Schema::Int().Min(0))
                    .Prop("mipWidth", Schema::Int().Min(1))
                    .Prop("mipHeight", Schema::Int().Min(1))
                    .Prop("origin", Schema::String().Enum({ "image", "storage" }))
                    .Prop("region", Schema::Object().Prop("x", Schema::Int()).Prop("y", Schema::Int()).Prop("w", Schema::Int()).Prop("h", Schema::Int()))
                    .Prop("channels", Schema::Int().Min(1).Max(4))
                    .Prop("valueKind", Schema::String().Enum({ "unorm8", "float", "int" }))
                    .Prop("reads", Schema::String().Desc("What each reported value is."))
                    .Prop("texels", Schema::Array(texel).Desc("Row-major in the chosen origin."))
                    .Required({ "handle", "backend", "format", "srgb", "width", "height", "mipLevels", "origin", "valueKind", "reads", "texels" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_TextureProbe;
            registry.Register(std::move(tool));
        }

        {
            const Schema::Node candidate =
                Schema::Object()
                    .Prop("id", Schema::Int().Min(1))
                    .Prop("binding", Schema::Int().Min(0))
                    .Prop("sizeBytes", Schema::Int().Min(0))
                    .Prop("usage", Schema::String())
                    .Prop("names", Schema::Array(Schema::String()).Desc("SSBO_* constants for the binding; empty for a pooled buffer."))
                    .Prop("owner", Schema::String().Desc("Memory-owner scope at creation, e.g. TransientPool; empty when none."))
                    .Prop("pooled", Schema::Bool().Desc("A pooled buffer: its binding is nominal, so binding/name selection skips it."));
            ToolDef tool;
            tool.Name = "olo_gpu_buffer_read";
            tool.Toolset = "render";
            tool.Title = "Storage buffer read";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read a byte range of a live storage buffer (SSBO) back from the GPU. Select it by 'binding', by "
                "'name' (an SSBO_* constant from ShaderBindingLayout.h, with or without the prefix, e.g. "
                "FPLUS_LIGHT_GRID) or by 'id'. Several live buffers can share a binding (one per particle system); "
                "then the call fails and lists them as candidates, and 'id' picks one. Pooled (TransientPool) buffers "
                "carry a nominal binding, so only 'id' reaches them. 'offsetBytes' (default 0), "
                "'lengthBytes' (default up to 256, at most 4096) and 'format' (u32 default, i32, f32, vec4, hex) "
                "shape the read. Unknown names, bindings with no live buffer and ranges past the end are errors. "
                "The read is synchronous and fenced: it sees the work submitted so far.";
            tool.InputSchema =
                Schema::Object()
                    .Prop("binding", Schema::Int().Min(0).Desc("SSBO binding number the buffer was created for."))
                    .Prop("name", Schema::String().Desc("SSBO_* constant name, e.g. SSBO_FPLUS_LIGHT_GRID."))
                    .Prop("id", Schema::Int().Min(1).Desc("A buffer id from a previous reply's candidates."))
                    .Prop("offsetBytes", Schema::Int().Min(0).Desc("Byte offset (default 0; a multiple of 4 unless format is hex)."))
                    .Prop("lengthBytes", Schema::Int().Min(1).Max(GpuBufferRead::kMaxReadBytes).Desc("Bytes to read (default: up to 256)."))
                    .Prop("format", Schema::String().Enum({ "u32", "i32", "f32", "vec4", "hex" }).Desc("How to decode (default u32)."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("buffer", candidate)
                    .Prop("backend", Schema::String().Enum({ "opengl", "vulkan" }))
                    .Prop("offsetBytes", Schema::Int().Min(0))
                    .Prop("lengthBytes", Schema::Int().Min(1))
                    .Prop("format", Schema::String().Enum({ "u32", "i32", "f32", "vec4", "hex" }))
                    .Prop("elementBytes", Schema::Int().Min(1))
                    .Prop("values", Schema::Array().Desc("Decoded elements; hex: one string per 16 bytes. A non-finite float is the string nan, inf or -inf."))
                    .Prop("reads", Schema::String())
                    .Required({ "buffer", "backend", "offsetBytes", "lengthBytes", "format", "values" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_GpuBufferRead;
            registry.Register(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
