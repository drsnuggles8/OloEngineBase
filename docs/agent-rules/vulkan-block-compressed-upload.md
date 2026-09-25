# Uploading a cooked block-compressed texture on Vulkan

*Issue #1453. Until then `Texture2D::Create(CompressedTextureImage)` returned null on Vulkan, so
every cooked texture was simply missing there.*

## The rule

`VulkanTexture2D(const CompressedTextureImage&)` uploads the cooked chain as-is, and every one of
these points is load-bearing:

1. **Copy every cooked level; never generate one.** `vkCmdBlitImage` cannot write a
   block-compressed image, and the cooked chain may be deliberately SHORT: an alpha cutout's chain
   stops at 64 texels (`alpha-cutout-mip-chains.md`). The image gets exactly `image.MipLevels()`
   levels; `RegenerateMips` and `Resize` refuse on a compressed texture, as on GL.
2. **The copy extent is the level's texel extent, whole.** A block copy's extent must be a multiple
   of the block size *unless it reaches the subresource edge*. A whole level always does, which is
   also what makes the 2x2 and 1x1 tail levels legal: each is one block covering texels that do
   not exist. Row length 0 means tightly packed blocks. Staging offsets stay block multiples
   because every level is a whole number of blocks (8 bytes for BC4, 16 for the rest).
3. **No colour-attachment, storage or host-transfer usage.** No BC format supports the first two,
   and naming them fails image creation. The host-image-copy route writes mip 0 only.
4. **BC4 needs an (R, R, R, 1) swizzle on every sampled view.** The engine samples BC4 that way on
   both backends; hardware returns (R, 0, 0, 1). GL sets a texture swizzle. Vulkan has no view
   objects for sampled use (descriptor heap), so the mapping lives in
   `VulkanImageInfo::Components` and every sampled view description copies it: the draw bind, the
   heap resolver, the staged descriptor and the ImGui view. The slot cache hashes the components.
   A storage view must stay identity.
5. **Ask per format, refuse by name.** Enable `textureCompressionBC` when supported. Before
   creating the image, check `SAMPLED_IMAGE | TRANSFER_DST | SAMPLED_IMAGE_FILTER_LINEAR` in
   `optimalTilingFeatures`. A device without them gets an error naming the format and an
   *unloaded* texture, not null: callers already read `IsLoaded()`.
6. **Validate the chain before creating anything.** A level whose byte count does not match its
   extent would leave a mip unwritten, which samples undefined memory. GL skips such a level;
   Vulkan refuses the texture.
7. **`HasAlphaChannel` is the cook's measurement.** An opaque BC7 albedo reported as alpha-bearing
   sorts into the transparent pass. Vulkan used to say yes for every BC7.

## How it is tested

`VulkanCompressedTextureUploadTest` encodes each level of each chain from a different solid
colour, then draws each level with `textureLod` and reads the attachment back. A level copied to
the wrong mip, at the wrong offset or with the wrong extent reads the wrong colour. Cases: BC7 sRGB
and linear, BC5, BC4, BC6H unsigned and signed; 16x16, 20x12 (partial blocks plus two sub-block
tail levels) and a capped 64x64 three-level chain. The fixture requires zero validation errors
with sync validation on.
