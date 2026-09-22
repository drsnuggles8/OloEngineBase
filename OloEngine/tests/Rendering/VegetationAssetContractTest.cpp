// =============================================================================
// VegetationAssetContractTest.cpp
//
// The committed vegetation (issue #1398) is ART, and art has no compiler. These
// are the four properties of it that fail SILENTLY — the scene loads, the frame
// renders, and something is wrong in a way no existing test notices:
//
//   1. THE AUTHORING FRAME. FoliageRenderer assumes every plant mesh is base at
//      the origin with unit height, rescales nothing, and only WARNS when a mesh
//      is not — "it is scaled by the instance's height * scale as-is, so every
//      plant is drawn Nx the authored height, near mesh and far impostor alike".
//      A warning in a log nobody reads is not a guard. This is that guard.
//
//   2. THE MATERIAL SPLIT. A per-submesh albedo only happens when the OBJ's
//      `usemtl` names resolve in the MTL and those textures exist; when they do
//      not, Material::GetAlbedoMap() returns null and FoliageRenderer falls back
//      to the LAYER albedo for every submesh. That fallback is the defect this
//      issue is about, and it looks like nothing at all going wrong.
//
//   3. THE CUTOUT IS A DELIBERATE NUMBER. #1398's headline measurement: the one
//      grass cutout every species used passed 17.9% of its texels at the
//      authored cutoff, so a pine canopy discarded four pixels in five. The
//      floor below is what stops any future import drifting back there.
//
//   4. THE BILLBOARD IS A PICTURE OF THE PLANT. Its coverage is asserted from
//      BOTH sides on purpose. A floor alone passes a solid opaque rectangle,
//      which is the failure mode of pointing a card at a source UV atlas —
//      scanned plants ship those, and their unused space is opaque.
//
// Pure CPU: parses the committed OBJ/MTL and decodes the committed PNGs. No GL
// context, so it runs everywhere the suite does.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr const char* kVegetationRoot = "OloEditor/SandboxProject/Assets/Models/Vegetation";
        constexpr const char* kSceneRoot = "OloEditor/SandboxProject/Assets/Scenes";

        // The authoring tolerance FoliageRenderer itself uses before it warns.
        constexpr f32 kUnitTolerance = 0.05f;

        // Issue #1398 measured the retired shared cutout at 17.9%. Nothing that
        // ships as foliage may sit anywhere near that again.
        constexpr f32 kRetiredCutoutCoverage = 17.9f;
        constexpr f32 kFoliageCoverageFloor = 30.0f;

        // A billboard is a picture of a plant: neither empty nor a filled square.
        constexpr f32 kCardCoverageFloor = 3.0f;
        constexpr f32 kCardCoverageCeiling = 65.0f;

        constexpr f32 kAlphaCutoff = 0.5f;

        /// Resolve a repo-relative directory whether the suite runs from the repo
        /// root (ctest) or from inside OloEditor/ (editor-launched runs).
        [[nodiscard]] fs::path ResolveRepoPath(const char* relative)
        {
            const fs::path candidates[] = {
                fs::path(relative),
                fs::current_path() / relative,
                fs::current_path().parent_path() / relative,
                fs::current_path().parent_path().parent_path() / relative,
            };
            for (const auto& candidate : candidates)
            {
                std::error_code ec;
                if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec))
                    return fs::canonical(candidate, ec);
            }
            return {};
        }

        struct ObjGroup
        {
            std::string m_Material;
            std::vector<std::array<u32, 3>> m_PositionIndices;
            std::vector<std::array<u32, 3>> m_UVIndices;
        };

        struct Obj
        {
            std::vector<glm::vec3> m_Positions;
            std::vector<glm::vec2> m_UVs;
            std::vector<ObjGroup> m_Groups;
            std::string m_MtlLib;
        };

        [[nodiscard]] Obj ParseObj(const fs::path& path)
        {
            Obj obj;
            std::ifstream file(path);
            std::string line;
            ObjGroup current;
            current.m_Material = "default";
            while (std::getline(file, line))
            {
                std::istringstream stream(line);
                std::string tag;
                stream >> tag;
                if (tag == "v")
                {
                    glm::vec3 p{};
                    stream >> p.x >> p.y >> p.z;
                    obj.m_Positions.push_back(p);
                }
                else if (tag == "vt")
                {
                    glm::vec2 uv{};
                    stream >> uv.x >> uv.y;
                    obj.m_UVs.push_back(uv);
                }
                else if (tag == "mtllib")
                {
                    stream >> obj.m_MtlLib;
                }
                else if (tag == "usemtl")
                {
                    if (!current.m_PositionIndices.empty())
                        obj.m_Groups.push_back(std::move(current));
                    current = ObjGroup{};
                    stream >> current.m_Material;
                }
                else if (tag == "f")
                {
                    std::array<u32, 3> positions{};
                    std::array<u32, 3> uvs{};
                    bool ok = true;
                    for (u32 corner = 0; corner < 3; ++corner)
                    {
                        std::string token;
                        if (!(stream >> token))
                        {
                            ok = false;
                            break;
                        }
                        const auto firstSlash = token.find('/');
                        const auto secondSlash = token.find('/', firstSlash + 1);
                        positions[corner] = static_cast<u32>(std::stoul(token.substr(0, firstSlash))) - 1u;
                        uvs[corner] = (firstSlash != std::string::npos && secondSlash > firstSlash + 1)
                                          ? static_cast<u32>(std::stoul(
                                                token.substr(firstSlash + 1, secondSlash - firstSlash - 1))) -
                                                1u
                                          : 0u;
                    }
                    if (ok)
                    {
                        current.m_PositionIndices.push_back(positions);
                        current.m_UVIndices.push_back(uvs);
                    }
                }
            }
            if (!current.m_PositionIndices.empty())
                obj.m_Groups.push_back(std::move(current));
            return obj;
        }

        struct MtlEntry
        {
            std::string m_Albedo;    ///< map_Kd, as written in the MTL
            bool m_IsCutout = false; ///< the MTL declares map_d — this material is alpha-tested
        };

        /// material name -> its map_Kd, and whether the MTL calls it a cutout.
        ///
        /// `map_d` is the AUTHORED statement that a material is alpha-tested;
        /// the importer writes it only for a submesh whose albedo carries a real
        /// mask. Reading intent from the MTL rather than inferring it from the
        /// pixels is what lets the coverage check below stay honest — see the
        /// comment there.
        [[nodiscard]] std::map<std::string, MtlEntry> ParseMtl(const fs::path& path)
        {
            std::map<std::string, MtlEntry> maps;
            std::ifstream file(path);
            std::string line;
            std::string material;
            while (std::getline(file, line))
            {
                std::istringstream stream(line);
                std::string tag;
                stream >> tag;
                if (tag == "newmtl")
                {
                    stream >> material;
                    maps.try_emplace(material, MtlEntry{});
                }
                else if (tag == "map_Kd" && !material.empty())
                {
                    std::string texture;
                    stream >> texture;
                    maps[material].m_Albedo = texture;
                }
                else if (tag == "map_d" && !material.empty())
                {
                    maps[material].m_IsCutout = true;
                }
            }
            return maps;
        }

        /// The species the importer is configured to produce, read from
        /// tools/vegetation-import/recipes.json.
        ///
        /// Read rather than duplicated: a hard-coded list in this file would be a
        /// second place to update and would drift. The recipe file is the
        /// authoritative statement of what this repository imports, so "a recipe
        /// exists but its directory does not" is exactly the failure to catch.
        [[nodiscard]] std::vector<std::string> RecipeSpeciesNames(std::string& outFailure)
        {
            const fs::path root = ResolveRepoPath("tools/vegetation-import");
            if (root.empty())
            {
                outFailure = "tools/vegetation-import not found";
                return {};
            }
            std::ifstream file(root / "recipes.json");
            if (!file.is_open())
            {
                outFailure = "recipes.json could not be opened";
                return {};
            }
            nlohmann::json recipes;
            try
            {
                file >> recipes;
            }
            catch (const std::exception& e)
            {
                outFailure = std::string("recipes.json does not parse: ") + e.what();
                return {};
            }
            const auto speciesIt = recipes.find("species");
            if (speciesIt == recipes.end() || !speciesIt->is_array())
            {
                outFailure = "recipes.json has no top-level 'species' array";
                return {};
            }
            std::vector<std::string> names;
            for (const auto& entry : *speciesIt)
            {
                const auto nameIt = entry.find("name");
                if (nameIt != entry.end() && nameIt->is_string())
                    names.push_back(nameIt->get<std::string>());
            }
            return names;
        }

        struct Image
        {
            i32 m_Width = 0;
            i32 m_Height = 0;
            std::vector<u8> m_RGBA;

            [[nodiscard]] bool Valid() const
            {
                return m_Width > 0 && m_Height > 0;
            }

            [[nodiscard]] f32 AlphaAt(f32 u, f32 v) const
            {
                const f32 wrappedU = u - std::floor(u);
                const f32 wrappedV = v - std::floor(v);
                const i32 x = std::clamp(static_cast<i32>(wrappedU * static_cast<f32>(m_Width - 1)), 0, m_Width - 1);
                const i32 y = std::clamp(static_cast<i32>(wrappedV * static_cast<f32>(m_Height - 1)), 0, m_Height - 1);
                return static_cast<f32>(m_RGBA[(static_cast<sizet>(y) * static_cast<sizet>(m_Width) + static_cast<sizet>(x)) * 4u + 3u]) / 255.0f;
            }
        };

        [[nodiscard]] Image LoadImage(const fs::path& path)
        {
            Image image;
            i32 channels = 0;
            u8* pixels = stbi_load(path.string().c_str(), &image.m_Width, &image.m_Height, &channels, 4);
            if (pixels == nullptr)
                return {};
            image.m_RGBA.assign(pixels, pixels + static_cast<sizet>(image.m_Width) * static_cast<sizet>(image.m_Height) * 4u);
            stbi_image_free(pixels);
            return image;
        }

        /// Fraction of a submesh's SURFACE whose albedo survives the alpha test,
        /// area-weighted. Deliberately not the texture's flat average: a UV atlas
        /// has unused space, and on a scanned plant that space is opaque, so the
        /// flat figure describes a canopy that does not exist.
        [[nodiscard]] f32 SurfaceCoverage(const Obj& obj, const ObjGroup& group, const Image& image)
        {
            // Fixed barycentric taps rather than random ones: a contract test that
            // reports a different number each run cannot be reasoned about.
            static constexpr glm::vec3 kTaps[] = {
                { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
                { 0.6f, 0.2f, 0.2f },
                { 0.2f, 0.6f, 0.2f },
                { 0.2f, 0.2f, 0.6f },
                { 0.5f, 0.5f, 0.0f },
                { 0.0f, 0.5f, 0.5f },
                { 0.5f, 0.0f, 0.5f },
            };

            f64 weighted = 0.0;
            f64 total = 0.0;
            for (sizet t = 0; t < group.m_PositionIndices.size(); ++t)
            {
                const auto& pi = group.m_PositionIndices[t];
                const auto& ti = group.m_UVIndices[t];
                if (pi[0] >= obj.m_Positions.size() || pi[1] >= obj.m_Positions.size() || pi[2] >= obj.m_Positions.size())
                    continue;
                if (ti[0] >= obj.m_UVs.size() || ti[1] >= obj.m_UVs.size() || ti[2] >= obj.m_UVs.size())
                    continue;

                const glm::vec3& p0 = obj.m_Positions[pi[0]];
                const glm::vec3& p1 = obj.m_Positions[pi[1]];
                const glm::vec3& p2 = obj.m_Positions[pi[2]];
                const f64 area = 0.5 * static_cast<f64>(glm::length(glm::cross(p1 - p0, p2 - p0)));
                if (!(area > 0.0))
                    continue;

                u32 passed = 0;
                for (const glm::vec3& tap : kTaps)
                {
                    const glm::vec2 uv = tap.x * obj.m_UVs[ti[0]] + tap.y * obj.m_UVs[ti[1]] + tap.z * obj.m_UVs[ti[2]];
                    // OBJ v runs upward from the bottom; image rows run downward.
                    if (image.AlphaAt(uv.x, 1.0f - uv.y) >= kAlphaCutoff)
                        ++passed;
                }
                weighted += area * (static_cast<f64>(passed) / static_cast<f64>(std::size(kTaps)));
                total += area;
            }
            return total > 0.0 ? static_cast<f32>(100.0 * weighted / total) : 0.0f;
        }

        [[nodiscard]] f32 TexelCoverage(const Image& image)
        {
            sizet passed = 0;
            const sizet texels = static_cast<sizet>(image.m_Width) * static_cast<sizet>(image.m_Height);
            for (sizet i = 0; i < texels; ++i)
            {
                if (image.m_RGBA[i * 4u + 3u] >= static_cast<u8>(kAlphaCutoff * 255.0f))
                    ++passed;
            }
            return texels > 0 ? static_cast<f32>(100.0 * static_cast<f64>(passed) / static_cast<f64>(texels)) : 0.0f;
        }

        [[nodiscard]] std::vector<fs::path> SpeciesDirectories()
        {
            std::vector<fs::path> out;
            const fs::path root = ResolveRepoPath(kVegetationRoot);
            if (root.empty())
                return out;
            for (const auto& entry : fs::directory_iterator(root))
            {
                if (!entry.is_directory())
                    continue;
                const std::string name = entry.path().filename().string();
                if (name.starts_with("_"))
                    continue; // _raw/ holds fetched source scans, not committed art
                if (fs::exists(entry.path() / (name + ".obj")))
                    out.push_back(entry.path());
            }
            std::sort(out.begin(), out.end());
            return out;
        }
    } // namespace

    // -------------------------------------------------------------------------

    TEST(VegetationAssetContract, ThereIsVegetationToCheck)
    {
        // The guard that stops every other case in this file passing vacuously
        // if the asset root ever moves.
        const std::vector<fs::path> species = SpeciesDirectories();
        ASSERT_FALSE(species.empty())
            << "no imported vegetation found under " << kVegetationRoot
            << " — rebuild it with: python tools/vegetation-import/import_vegetation.py";

        // BY NAME, not by count. A count is satisfied by the wrong set: drop the
        // pine and six directories still clear any "at least four" bar, and
        // every other case in this file then iterates the survivors and passes.
        // The named check is the only one that fails when a species goes missing.
        std::string failure;
        const std::vector<std::string> expected = RecipeSpeciesNames(failure);
        ASSERT_FALSE(expected.empty()) << "could not read the expected species set: " << failure;

        std::set<std::string> present;
        for (const fs::path& dir : species)
            present.insert(dir.filename().string());

        for (const std::string& want : expected)
        {
            EXPECT_TRUE(present.contains(want))
                << "recipes.json imports '" << want << "' but " << kVegetationRoot << "/" << want
                << "/ is missing — rebuild it with: python tools/vegetation-import/import_vegetation.py " << want;
        }
    }

    TEST(VegetationAssetContract, EverySpeciesCarriesItsProvenance)
    {
        for (const fs::path& dir : SpeciesDirectories())
        {
            const std::string name = dir.filename().string();
            // CC0 does not require attribution, but the repository's convention
            // (OloEditor/assets/models/InfiniteScanHead/) records it anyway, and
            // the benchmark manifests declare these assets LicenseVerified
            // 'in-repo-file' — which is a lie without the file.
            EXPECT_TRUE(fs::exists(dir / "LICENSE.md")) << name << " has no LICENSE.md";
            EXPECT_TRUE(fs::exists(dir / "README.md")) << name << " has no README.md";
            EXPECT_TRUE(fs::exists(dir / (name + ".mtl"))) << name << " has no .mtl";
            EXPECT_TRUE(fs::exists(dir / "Textures" / (name + "_card.png")))
                << name << " has no billboard for the far LOD rung";
        }
    }

    TEST(VegetationAssetContract, MeshesAreAuthoredBaseAtOriginUnitHeight)
    {
        for (const fs::path& dir : SpeciesDirectories())
        {
            const std::string name = dir.filename().string();
            const Obj obj = ParseObj(dir / (name + ".obj"));
            ASSERT_FALSE(obj.m_Positions.empty()) << name << ": no vertices";

            f32 minY = obj.m_Positions.front().y;
            f32 maxY = minY;
            for (const glm::vec3& p : obj.m_Positions)
            {
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
            // FoliageRenderer warns about this and carries on, so a mesh that
            // violates it renders at the wrong size in the near mesh AND the far
            // impostor, consistently, with nothing failing.
            EXPECT_NEAR(minY, 0.0f, kUnitTolerance) << name << ": base is not at the origin";
            EXPECT_NEAR(maxY, 1.0f, kUnitTolerance) << name << ": not unit height";
        }
    }

    TEST(VegetationAssetContract, EverySubmeshResolvesItsOwnAlbedo)
    {
        for (const fs::path& dir : SpeciesDirectories())
        {
            const std::string name = dir.filename().string();
            const Obj obj = ParseObj(dir / (name + ".obj"));
            ASSERT_FALSE(obj.m_MtlLib.empty()) << name << ": OBJ names no mtllib";
            const auto maps = ParseMtl(dir / obj.m_MtlLib);
            ASSERT_FALSE(obj.m_Groups.empty()) << name << ": OBJ has no usemtl group";

            for (const ObjGroup& group : obj.m_Groups)
            {
                const auto entry = maps.find(group.m_Material);
                ASSERT_NE(entry, maps.end())
                    << name << ": usemtl '" << group.m_Material << "' has no newmtl. FoliageRenderer "
                    << "would fall back to the LAYER albedo for this submesh, which is the defect #1398 is about";
                ASSERT_FALSE(entry->second.m_Albedo.empty())
                    << name << ": material '" << group.m_Material << "' has no map_Kd";
                EXPECT_TRUE(fs::exists(dir / entry->second.m_Albedo))
                    << name << ": map_Kd '" << entry->second.m_Albedo << "' does not exist";
            }
        }
    }

    TEST(VegetationAssetContract, FoliageCutoutIsADeliberateNumber)
    {
        for (const fs::path& dir : SpeciesDirectories())
        {
            const std::string name = dir.filename().string();
            const Obj obj = ParseObj(dir / (name + ".obj"));
            const auto maps = ParseMtl(dir / obj.m_MtlLib);

            u32 cutoutSubmeshes = 0;
            for (const ObjGroup& group : obj.m_Groups)
            {
                const auto entry = maps.find(group.m_Material);
                if (entry == maps.end() || entry->second.m_Albedo.empty())
                    continue;

                // Which submeshes carry a coverage claim is read from the MTL's
                // `map_d`, NOT from how opaque the texture happens to be.
                //
                // An earlier version skipped any material whose texture measured
                // over 99% opaque, reasoning that trunks are opaque. That test
                // could not fail: a foliage albedo that lost its cutout and went
                // opaque is EXACTLY the regression this case exists to catch, and
                // it would have been skipped for being opaque. The authored
                // intent has to come from the asset, not from the pixels being
                // checked against it.
                if (!entry->second.m_IsCutout)
                    continue;
                ++cutoutSubmeshes;

                const Image image = LoadImage(dir / entry->second.m_Albedo);
                ASSERT_TRUE(image.Valid()) << name << ": cannot decode " << entry->second.m_Albedo;

                const f32 coverage = SurfaceCoverage(obj, group, image);
                // One floor, not two. An earlier version also asserted
                // `> kRetiredCutoutCoverage * 1.5` (26.85%), which is strictly
                // weaker than the 30% floor above it and could therefore never
                // fail on its own — an assertion that cannot fail reads as extra
                // coverage and is not.
                EXPECT_GT(coverage, kFoliageCoverageFloor)
                    << name << "/" << group.m_Material << ": " << coverage << "% of the drawn surface survives the "
                    << "alpha test. The shared grass cutout this replaced passed " << kRetiredCutoutCoverage << "%";
                EXPECT_LT(TexelCoverage(image), 99.0f)
                    << name << "/" << group.m_Material << " is declared a cutout (map_d) but its albedo is "
                    << "effectively opaque — the alpha channel was lost somewhere in the import";
            }

            // A species whose cutout submeshes all vanished would otherwise pass
            // this case by having nothing left to check.
            EXPECT_GT(cutoutSubmeshes, 0u)
                << name << " declares no alpha-tested submesh at all; every plant here has foliage";
        }
    }

    TEST(VegetationAssetContract, BillboardIsAPictureOfThePlant)
    {
        for (const fs::path& dir : SpeciesDirectories())
        {
            const std::string name = dir.filename().string();
            const Image card = LoadImage(dir / "Textures" / (name + "_card.png"));
            ASSERT_TRUE(card.Valid()) << name << ": cannot decode its billboard";

            const f32 coverage = TexelCoverage(card);
            EXPECT_GT(coverage, kCardCoverageFloor)
                << name << ": billboard is " << coverage << "% opaque — the far rung would draw nothing";
            // The ceiling is the load-bearing half. A source UV atlas pointed at
            // a card measures near 100% because its unused space is opaque, and
            // it would draw a rectangle of texture sheet where a plant belongs.
            EXPECT_LT(coverage, kCardCoverageCeiling)
                << name << ": billboard is " << coverage << "% opaque — that is a filled rectangle, "
                << "not a cut-out plant. Is it pointed at a source atlas rather than a bake?";
        }
    }

    TEST(VegetationAssetContract, NoSceneStillUsesTheRetiredStandIns)
    {
        // The regression guard. pine.obj, palm.obj and grass.png stay in the tree
        // as fixtures for the tests that construct layers directly, so nothing
        // stops a scene quietly naming them again.
        const fs::path root = ResolveRepoPath(kSceneRoot);
        ASSERT_FALSE(root.empty()) << "scene root not found";

        static constexpr const char* kRetired[] = {
            "Models/Vegetation/pine.obj",
            "Models/Vegetation/palm.obj",
            "assets/textures/grass.png",
        };

        u32 scanned = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".olo")
                continue;
            ++scanned;
            std::ifstream file(entry.path());
            std::ostringstream buffer;
            buffer << file.rdbuf();
            const std::string text = buffer.str();
            for (const char* retired : kRetired)
            {
                EXPECT_EQ(text.find(retired), std::string::npos)
                    << entry.path().filename().string() << " still names '" << retired
                    << "'. Every species wearing one grass cutout is issue #1398";
            }
        }
        EXPECT_GT(scanned, 0u) << "no scenes scanned — the guard would pass vacuously";
    }
} // namespace OloEngine::Tests
