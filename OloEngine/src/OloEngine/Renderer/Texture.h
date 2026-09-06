#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RendererResource.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // Defined in Renderer/TextureCompression.h — a mip chain of BCn block bytes.
    struct CompressedTextureImage;

    enum class ImageFormat
    {
        None = 0,
        R8,
        R8UI,
        R16UI,
        RG16UI,
        RGB8,
        RGBA8,
        RGBA16F,
        RGBA32F, // Unsupported
        R32F,    // Unsupported
        RG32F,   // Unsupported
        RGB32F,  // Unsupported
        DEPTH24STENCIL8,
        RG16F, // Keep appended to preserve legacy serialized enum values
        R32I,
        RG8, // 2-channel 8-bit (normal-map xy, two-channel masks). Appended to
             // preserve legacy serialised enum integer values used in asset packs.
        // Block-compressed formats (#440). Uploaded via glCompressedTextureSubImage2D
        // rather than a client pixel format; the sRGB variant of BC7 is selected from
        // TextureSpecification::SRGB. Appended to preserve legacy serialised values.
        BC7,  // BPTC RGBA — base color / albedo / emissive
        BC5,  // RGTC2 two-channel — tangent-space normal xy
        BC6H, // BPTC RGB half-float (unsigned) — HDR environment / IBL / emissive
        // Appended, like every entry since RG16F, to preserve legacy serialised
        // integer values. Added for Virtual Shadow Maps (issue #702): the page
        // pool stores raw float depth BITS and is resolved with imageAtomicMin,
        // which has no float form. Until this existed the Vulkan backend's
        // RHI::Format::R32UInt arm had nothing to map to and returned the NULL
        // texture handle, so the whole pool silently did not exist there.
        R32UI,
        // BPTC RGB half-float, SIGNED variant (issue #624), for HDR source data that
        // carries negative components. Appended for the same reason as R32UI above:
        // this enum's integer values are persisted verbatim in asset-pack texture
        // records and the IBL cache header, so slotting it next to BC6H would have
        // renumbered R32UI and made every legacy record holding 18 read back as a
        // block-compressed format.
        BC6HS,
        // 4x32-bit unsigned integer (issue #624). Appended, like every entry since
        // RG16F. One RGBA32UI texel is bit-compatible with one 16-byte BC block,
        // which is how the GPU BC6H encoder writes its output without taking an
        // SSBO binding — see Renderer/BC6HGpuEncoder.h.
        RGBA32UI,
        // RGTC1 single channel, 8 bytes per 4x4 block (issue #624). Appended, like
        // every entry since RG16F. Sampled as (R,R,R,1) via a texture swizzle set at
        // upload, so it is a drop-in for greyscale data that used to cost a full BC7.
        BC4
    };

    // True for the block-compressed ImageFormat values, which take the
    // glCompressedTextureSubImage2D upload path instead of a client pixel format.
    [[nodiscard("Store this!")]] constexpr bool IsCompressedFormat(ImageFormat format) noexcept
    {
        return format == ImageFormat::BC7 || format == ImageFormat::BC5 || format == ImageFormat::BC6H ||
               format == ImageFormat::BC6HS || format == ImageFormat::BC4;
    }

    // True for the integer (non-normalised) ImageFormat values — the ones a
    // shader reads through an isampler/usampler rather than a float sampler.
    //
    // These MUST be sampled with GL_NEAREST. GL requires a texture whose base
    // format is integer to use a NEAREST mag filter; with GL_LINEAR it is
    // *incomplete*, and sampling an incomplete texture yields zero — including
    // through texelFetch. NVIDIA quietly tolerates the linear filter, Mesa
    // does not, so a linear-filtered integer texture reads as all-zero on AMD
    // and correct on NVIDIA. That asymmetry silently erased every glyph the
    // Slug text renderer drew (its RG16UI band texture returned zero bands, so
    // each glyph covered no pixels) while leaving the geometry, draw calls and
    // logs looking perfectly healthy.
    [[nodiscard("Store this!")]] constexpr bool IsIntegerFormat(ImageFormat format) noexcept
    {
        return format == ImageFormat::R8UI || format == ImageFormat::R16UI ||
               format == ImageFormat::RG16UI || format == ImageFormat::R32I ||
               format == ImageFormat::R32UI || format == ImageFormat::RGBA32UI;
    }

    struct TextureSpecification
    {
        u32 Width = 1;
        u32 Height = 1;
        ImageFormat Format = ImageFormat::RGBA8;
        bool GenerateMips = true;
        // Explicit mip level count. 0 = auto (1 if !GenerateMips, full chain if GenerateMips).
        u32 MipLevels = 0;
        // Sample count. Values > 1 create multisample storage and force a
        // single mip level.
        u32 Samples = 1;
        // True for color textures (albedo, emissive, UI) — selects an sRGB
        // internal format so the GPU automatically converts sample data from
        // sRGB to linear. Leave false for data textures (normal, metallic,
        // roughness, AO) where the bytes already encode linear values.
        // Only meaningful for 8-bit color formats (RGB8 / RGBA8); ignored
        // for float / integer / depth formats.
        bool SRGB = false;
        // True for textures whose pixels are replaced every frame from the CPU
        // (e.g. streamed video frames). The OpenGL backend then uploads through a
        // double-buffered Pixel Buffer Object ring so the copy + DMA do not stall
        // the render thread. Ignored for multisample textures.
        bool Streaming = false;
    };

    class Texture : public RendererResource
    {
      public:
        virtual ~Texture() = default;

        virtual const TextureSpecification& GetSpecification() const = 0;

        [[nodiscard("Store this!")]] virtual u32 GetWidth() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetHeight() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691). Sibling of GetRendererID during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard("Store this!")]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        [[nodiscard("Store this!")]] virtual const std::string& GetPath() const = 0;

        virtual void SetData(void* data, u32 size) = 0;
        virtual void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) = 0;

        virtual void Bind(u32 slot) const = 0;

        [[nodiscard("Store this!")]] virtual bool IsLoaded() const = 0;

        [[nodiscard("Use for transparency")]] virtual bool HasAlphaChannel() const = 0;

        /**
         * @brief Read texture data back from GPU
         *
         * @param outData Vector to receive the texture data
         * @param mipLevel Mipmap level to read (0 = base level)
         * @return true if readback succeeded
         */
        virtual bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const = 0;

        // Compares IDENTITIES, not driver names (issue #691). GL recycles
        // object names, so a name comparison could report two genuinely different
        // textures as equal once one had been destroyed — the defect the
        // generation exists to make unrepresentable. A handle carries one, so
        // two distinct objects can never compare equal here.
        bool operator==(const Texture& other) const
        {
            return GetRHIHandle() == other.GetRHIHandle();
        }

        // Asset interface
        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::None;
        }
        AssetType GetAssetType() const override = 0;
    };

    class Texture2D : public Texture
    {
      public:
        virtual void SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize) = 0;

        [[nodiscard("Store this!")]] virtual u32 GetMipLevelCount() const = 0;

        // Rebuild mips 1..N from level 0 (issue #716).
        //
        // SetData and the mip-aware path in SubImage do this implicitly, so before
        // GPU-resident terrain authoring existed there was no producer that needed
        // to ask. A compute kernel writing level 0 through imageStore, or a
        // GPU-to-GPU region copy, is exactly such a producer: it leaves the coarse
        // levels holding pre-edit content, and NOTHING reports an error.
        //
        // That is not a cosmetic concern for the terrain heightmap. The tessellation
        // stage picks a mip from the triangle footprint and reads the height that
        // POSITIONS the vertex (include/TerrainHeightSampling.glsl), so a stale
        // chain means distant terrain keeps its old shape and snaps to the new one
        // as the camera closes — correct everywhere the artist is looking, wrong
        // everywhere they are not.
        //
        // No-op when the texture has a single level.
        virtual void RegenerateMips() = 0;

        // Recreate the texture with new dimensions (same spec otherwise).
        // Needed because glTextureStorage2D allocates immutable storage.
        virtual void Resize(u32 width, u32 height) = 0;

        // Re-read this texture's source file and refresh its GPU contents *in place*,
        // preserving object identity so existing Ref<Texture2D> holders (materials,
        // sprites, decals, UI) see the new pixels without being re-pointed on a
        // hot-reload (issue #544). Returns false when it can't refresh in place — no
        // source path, or a cooked block-compressed container — so the caller falls
        // back to replacing the object. Default is a no-op refusal.
        virtual bool Reload()
        {
            return false;
        }

        // Resolve a stored source path (what GetPath() reports) to a path the
        // process can actually open. Used by Reload()'s re-read and by the
        // asset-pack serializer, which stores the same spelling and re-reads the
        // loose file at cook and load time.
        //
        // The two spellings that reach here are NOT interchangeable (#1067).
        // Texture2D::Create(path) stores whatever the caller passed — usually
        // absolute. The asset system instead stores the PROJECT-RELATIVE spelling
        // ("Assets/Textures/Foo.png"), deliberately, so saved scenes stay portable
        // across machines; but the editor's working directory is OloEditor/, one
        // level above the project, so re-reading that spelling verbatim resolves
        // against the wrong base and can never find the file. Relative paths are
        // therefore resolved against Project::GetProjectDirectory(), exactly as
        // every AssetSerializer does.
        //
        // Returns an empty path when the source cannot be resolved — no source
        // path, or a relative one that exists neither under the active project nor
        // relative to the working directory — having logged why. Callers refuse the
        // reload rather than reading from a base that happens to be the CWD.
        [[nodiscard("Store this!")]] static std::filesystem::path ResolveStoredSourcePath(std::string_view sourcePath);

        static Ref<Texture2D> Create(const TextureSpecification& specification);
        // Create a GPU texture from an offline block-compressed (BC7/BC5) mip chain,
        // uploaded via glCompressedTextureSubImage2D. On hardware lacking the required
        // BPTC/RGTC support the blocks are decompressed on the CPU and uploaded as an
        // uncompressed RGBA8 texture so nothing breaks (#440).
        static Ref<Texture2D> Create(const CompressedTextureImage& compressedImage);
        // Load a texture from disk. Pass srgb=true for color textures (albedo,
        // emissive, UI) so the GPU converts samples from sRGB to linear on
        // read. Leave srgb=false (default) for data textures (normal map,
        // metallic-roughness, AO, heightmap) where bytes are already linear.
        // `path` is read from directly (stbi_load gets it verbatim) and becomes
        // GetPath()'s value UNLESS `identityPath` is non-empty, in which case
        // `identityPath` is stored instead — used by the asset pipeline, which
        // must read from an absolute, project-rooted path (the process's cwd is
        // not the project directory) while still reporting the project-relative
        // path scene YAML expects back from GetPath().
        static Ref<Texture2D> Create(const std::string& path, bool srgb = false, const std::string& identityPath = "");

        // Asset interface
        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::Texture2D;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }
    };
} // namespace OloEngine
