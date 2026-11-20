# Keeper surface source credits

`SurfaceSources.zip` contains nine 8-bit NumPy arrays extracted from the 1K
Poly Haven JPEGs below. The original photographs supply fabric and leather
detail. The head geometry, skin pigmentation and gansey stitch pattern are
generated in this repository. The asset generator reads the archive offline.
[Poly Haven asset licence (CC0)](https://polyhaven.com/license)

| Asset | Artist | Keeper material |
| --- | --- | --- |
| [Brown Leather](https://polyhaven.com/a/brown_leather) | Rob Tuytel | Leather grain and roughness on the boots and cap peak |
| [Denim Fabric 04](https://polyhaven.com/a/denim_fabric_04) | colormass, photography. Rico Cilliers, processing. | Cotton ground for the oiled coat and twill trousers |
| [Poly Wool Herringbone](https://polyhaven.com/a/poly_wool_herringbone) | colormass, photography. Rico Cilliers, processing. | Wool crown of the cap |

The archive keeps the luminance of the RGB diffuse image (`_value`),
red and green of the OpenGL tangent-space normal (`_xy`), and green of
the ambient occlusion/roughness/metallic image (`_rough`). The luminance
conversion uses Pillow's RGB-to-L transform. `keeper_finishes.py` samples
these arrays into 1,024 × 1,024 charts and applies the dyes, salt bloom
and boot polish. Each digest below identifies the original JPEG bytes.

| Source JPEG | Dimensions (px) | SHA-256 |
| --- | --- | --- |
| [brown_leather Diffuse](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/brown_leather/brown_leather_albedo_1k.jpg) | 1024 × 1024 | `7e9e1d6566d1fd91d4ceb4966c4820361e85d53a7a828b6bab3b2a093a876cd1` |
| [brown_leather nor_gl](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/brown_leather/brown_leather_nor_gl_1k.jpg) | 1024 × 1024 | `506a0b7650c2dea285e247f26ed2d0d6ec38fc10dd584359f0d203e9c082236d` |
| [brown_leather arm](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/brown_leather/brown_leather_arm_1k.jpg) | 1024 × 1024 | `8f40d242830b2c1310470abfaf2df15f52658d01c21d97dd96f0679589677de1` |
| [denim_fabric_04 Diffuse](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/denim_fabric_04/denim_fabric_04_diff_1k.jpg) | 1024 × 1011 | `fc2527c166196684e588ef858daf977285b6701a3c4a68ef71da0c8f01f00263` |
| [denim_fabric_04 nor_gl](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/denim_fabric_04/denim_fabric_04_nor_gl_1k.jpg) | 1024 × 1011 | `d042e595687ae08f3c8e1188d3ea8cd45dbbf5f7777d2538671d41f08e3d1f15` |
| [denim_fabric_04 arm](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/denim_fabric_04/denim_fabric_04_arm_1k.jpg) | 1024 × 1011 | `fc7ebe19687f90d2e306389615eed1df592e36a5a6d84e5221abcc414ee2063f` |
| [poly_wool_herringbone Diffuse](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/poly_wool_herringbone/poly_wool_herringbone_diff_1k.jpg) | 1024 × 1045 | `fd889a46f617efd331e522e36b8ad2548e16e1cb018c001872f17537d7666af5` |
| [poly_wool_herringbone nor_gl](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/poly_wool_herringbone/poly_wool_herringbone_nor_gl_1k.jpg) | 1024 × 1045 | `314ac7c1b7c101a48106e05199bfdb24e33671e58a6ccb334249b441d8e8ac98` |
| [poly_wool_herringbone arm](https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/poly_wool_herringbone/poly_wool_herringbone_arm_1k.jpg) | 1024 × 1045 | `f565985fe68d42b6638080c52628f13312b178001d758ef89fe76adb773fb227` |
