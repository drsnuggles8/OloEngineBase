"""Generate tileable water textures for OloEngine water shader.
Output: 4 PNG files (512x512) in SandboxProject/Assets/Textures/Water/
- WaterNormal0.png - Primary water normal map (large ripples)
- WaterNormal1.png - Secondary water normal map (fine detail)
- WaterFoam.png    - Foam pattern (grayscale)
- WaterNoise.png   - Sparkle noise (grayscale)
All textures are seamlessly tileable.
"""

import numpy as np
from PIL import Image
import os

SIZE = 512
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "..", "OloEditor", "SandboxProject", "Assets", "Textures", "Water")


def make_tileable_noise(size, scale, octaves=4, persistence=0.5, seed=42):
    """Generate tileable Perlin-like noise using FFT filtering."""
    rng = np.random.default_rng(seed)
    # Start with white noise
    noise = rng.standard_normal((size, size))

    # FFT
    f = np.fft.fft2(noise)
    f_shift = np.fft.fftshift(f)

    # Create frequency mask for desired scale
    cy, cx = size // 2, size // 2
    y, x = np.ogrid[-cy:size-cy, -cx:size-cx]
    r = np.sqrt(x*x + y*y).astype(np.float64)
    r[cy, cx] = 1.0  # avoid div by zero

    # Multi-octave spectral shaping (1/f noise with controlled falloff)
    spectrum = np.zeros_like(r)
    for i in range(octaves):
        freq = scale * (2.0 ** i)
        amp = persistence ** i
        # Band-pass around this frequency
        band = np.exp(-0.5 * ((r - freq) / (freq * 0.4)) ** 2)
        spectrum += band * amp

    f_shift *= spectrum
    result = np.real(np.fft.ifft2(np.fft.ifftshift(f_shift)))

    # Normalize to [0, 1]
    result = (result - result.min()) / (result.max() - result.min() + 1e-10)
    return result.astype(np.float32)


def noise_to_normal(heightmap, strength=1.5):
    """Convert heightmap to normal map using Sobel-like central differences.
    Returns RGB array in [0, 255] with tangent-space normals encoded as (R,G,B) = (nx*0.5+0.5, ny*0.5+0.5, nz*0.5+0.5).
    """
    size = heightmap.shape[0]

    # Central differences (tileable via roll)
    dx = np.roll(heightmap, -1, axis=1) - np.roll(heightmap, 1, axis=1)
    dy = np.roll(heightmap, -1, axis=0) - np.roll(heightmap, 1, axis=0)

    # Build normal vectors
    nx = -dx * strength
    ny = -dy * strength
    nz = np.ones_like(nx)

    # Normalize
    length = np.sqrt(nx*nx + ny*ny + nz*nz)
    nx /= length
    ny /= length
    nz /= length

    # Encode to [0, 255]
    r = ((nx * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)
    g = ((ny * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)
    b = ((nz * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)

    return np.stack([r, g, b], axis=-1)


def generate_water_normal_0():
    """Large-scale water ripple normal map.
    Uses low-frequency noise with strong directional bias for wave-like patterns.
    """
    # Base noise at several octave scales
    h1 = make_tileable_noise(SIZE, scale=6, octaves=3, persistence=0.6, seed=100)
    h2 = make_tileable_noise(SIZE, scale=12, octaves=3, persistence=0.5, seed=200)
    h3 = make_tileable_noise(SIZE, scale=24, octaves=2, persistence=0.4, seed=300)

    # Add directional wave pattern (sinusoidal ridges)
    y_coords = np.linspace(0, 2 * np.pi * 3, SIZE, endpoint=False)
    x_coords = np.linspace(0, 2 * np.pi * 2, SIZE, endpoint=False)
    xx, yy = np.meshgrid(x_coords, y_coords)
    waves = np.sin(yy + h1 * 2.0) * 0.3 + np.sin(xx * 1.5 + yy * 0.5 + h2 * 1.5) * 0.2

    height = h1 * 0.4 + h2 * 0.3 + h3 * 0.15 + waves * 0.15
    height = (height - height.min()) / (height.max() - height.min())

    normal_rgb = noise_to_normal(height, strength=2.0)
    return Image.fromarray(normal_rgb, 'RGB')


def generate_water_normal_1():
    """Fine-detail water normal map.
    Higher frequency, more chaotic for micro-surface detail.
    """
    h1 = make_tileable_noise(SIZE, scale=16, octaves=4, persistence=0.55, seed=400)
    h2 = make_tileable_noise(SIZE, scale=32, octaves=3, persistence=0.5, seed=500)
    h3 = make_tileable_noise(SIZE, scale=64, octaves=2, persistence=0.4, seed=600)

    # Cross-hatch wave pattern for more realistic detail
    y_coords = np.linspace(0, 2 * np.pi * 8, SIZE, endpoint=False)
    x_coords = np.linspace(0, 2 * np.pi * 6, SIZE, endpoint=False)
    xx, yy = np.meshgrid(x_coords, y_coords)
    cross = np.sin(xx + yy + h1 * 3.0) * 0.15 + np.sin(xx - yy * 0.7 + h2 * 2.0) * 0.1

    height = h1 * 0.35 + h2 * 0.3 + h3 * 0.15 + cross * 0.2
    height = (height - height.min()) / (height.max() - height.min())

    normal_rgb = noise_to_normal(height, strength=1.5)
    return Image.fromarray(normal_rgb, 'RGB')


def generate_foam_texture():
    """Foam pattern: soft organic patches with streaky breakup.
    Grayscale texture where white = foam, black = no foam.
    Uses low-frequency base with medium detail — no speckle.
    """
    # Low-frequency base for large foam patches
    n1 = make_tileable_noise(SIZE, scale=5, octaves=3, persistence=0.55, seed=700)
    # Medium frequency for shape variation
    n2 = make_tileable_noise(SIZE, scale=10, octaves=3, persistence=0.5, seed=800)
    # Detail for organic breakup edges
    n3 = make_tileable_noise(SIZE, scale=20, octaves=3, persistence=0.45, seed=900)
    # Very low frequency for regional variation
    n4 = make_tileable_noise(SIZE, scale=3, octaves=2, persistence=0.5, seed=950)

    # Blend for soft foam shapes
    foam_base = n1 * 0.45 + n2 * 0.35 + n3 * 0.2
    # Soft threshold — wide ramp for smooth foam edges
    foam = np.clip((foam_base - 0.4) / 0.35, 0, 1)
    # Smooth edge breakup
    foam *= (0.6 + n3 * 0.4)
    # Large-scale regional modulation
    foam *= (0.4 + n4 * 0.6)

    foam = foam.clip(0, 1)
    # Mild gamma for contrast without hard edges
    foam = foam ** 0.8

    foam_uint8 = (foam * 255).clip(0, 255).astype(np.uint8)
    return Image.fromarray(foam_uint8, 'L')


def generate_noise_texture():
    """Sparkle/noise texture for specular breakup.
    High-frequency grayscale noise.
    """
    n1 = make_tileable_noise(SIZE, scale=32, octaves=4, persistence=0.6, seed=1000)
    n2 = make_tileable_noise(SIZE, scale=64, octaves=3, persistence=0.5, seed=1100)
    n3 = make_tileable_noise(SIZE, scale=16, octaves=3, persistence=0.5, seed=1200)

    # Combine for varied noise
    noise = n1 * 0.4 + n2 * 0.35 + n3 * 0.25
    # Increase contrast for sparkle effect
    noise = (noise - 0.3) / 0.4
    noise = noise.clip(0, 1)
    noise = noise ** 1.5  # Push dark areas darker

    noise_uint8 = (noise * 255).clip(0, 255).astype(np.uint8)
    return Image.fromarray(noise_uint8, 'L')


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    print("Generating WaterNormal0.png (large ripples)...")
    img = generate_water_normal_0()
    img.save(os.path.join(OUTPUT_DIR, "WaterNormal0.png"))

    print("Generating WaterNormal1.png (fine detail)...")
    img = generate_water_normal_1()
    img.save(os.path.join(OUTPUT_DIR, "WaterNormal1.png"))

    print("Generating WaterFoam.png (foam pattern)...")
    img = generate_foam_texture()
    img.save(os.path.join(OUTPUT_DIR, "WaterFoam.png"))

    print("Generating WaterNoise.png (sparkle noise)...")
    img = generate_noise_texture()
    img.save(os.path.join(OUTPUT_DIR, "WaterNoise.png"))

    print(f"Done! Textures saved to {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
