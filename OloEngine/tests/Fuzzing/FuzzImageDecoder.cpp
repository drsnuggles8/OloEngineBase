// =============================================================================
// FuzzImageDecoder.cpp
//
// libFuzzer harness for `stb_image`'s memory decoder. The engine calls
// `stbi_load` / `stbi_loadf` / `stbi_load_16` against arbitrary on-disk files
// (PNG / JPG / BMP / TGA / PSD / HDR / ...), which means every byte in those
// files is attacker-controlled from the engine's perspective. stb_image is
// a single-header C library that has historically shipped CVE-worthy decoder
// bugs (integer overflows, OOB reads in exotic PNG/BMP chunks). We drive it
// through `stbi_load_from_memory` so ASan/UBSan can observe the whole decode
// path with zero filesystem churn.
//
// Discovered crashers are minimised by libFuzzer into
// `artifact-<harness>-<hash>` files sitting next to the fuzzer binary; CI
// uploads them as build artefacts so they survive the runner cleanup.
// =============================================================================

#include <stb_image/stb_image.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// stb_image's size argument is `int`, so clamp pathological inputs. The
	// engine itself never loads files > 2 GiB and the fuzzer will still
	// exercise plenty of the interesting surface below this cap.
	if (size == 0 || size > static_cast<size_t>(1 << 28))
		return 0;

	int w = 0;
	int h = 0;
	int channels = 0;

	// 8-bit path covers PNG/JPG/BMP/TGA/PSD/GIF/PIC/PNM.
	stbi_uc* img8 = ::stbi_load_from_memory(data, static_cast<int>(size),
	                                        &w, &h, &channels, 0);
	if (img8)
		::stbi_image_free(img8);

	// 16-bit path covers PNG 16-bit and PSD 16-bit — distinct decoders
	// that share the chunk parsers but diverge in pixel-conversion paths.
	w = h = channels = 0;
	stbi_us* img16 = ::stbi_load_16_from_memory(data, static_cast<int>(size),
	                                            &w, &h, &channels, 0);
	if (img16)
		::stbi_image_free(img16);

	// HDR path — completely separate parser for Radiance RGBE files.
	w = h = channels = 0;
	float* imgf = ::stbi_loadf_from_memory(data, static_cast<int>(size),
	                                       &w, &h, &channels, 0);
	if (imgf)
		::stbi_image_free(imgf);

	return 0;
}
