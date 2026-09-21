// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GBufferCoverageChannelContractTest.cpp
//
// G-Buffer RT3 carries four channels since issue #1256 — velocity in .rg, the
// subject's COVERAGE in .b and its MATERIAL PROFILE in .a — and this file is
// the forcing function that keeps it that way.
//
// WHY A SOURCE-SCANNING TEST RATHER THAN A RENDER TEST. The failure this
// guards against is an MRT output left partially written. GLSL says an
// unwritten output component is UNDEFINED, not zero, so a shader that still
// assigns a vec2 to a vec4 attachment does not fail to compile, does not warn,
// and does not produce a wrong image reliably — it produces whatever the
// previous tile left in that memory. On one driver that reads as coverage 0
// (the strand vanishes from history rejection), on another as coverage 1 (the
// term never fires), and on a third it is stable enough to pass every capture
// you take. GBuffer.h has carried the same warning for RT5 since #865, and it
// is enforced the same way: by reading the shaders.
//
// So the contracts below are about DECLARATIONS AND ASSIGNMENTS, not pixels:
//
//   1. The C++ attachment table still says RGBA16F. If this fails the shaders
//      are writing four channels into a two-channel target and the coverage
//      term is reading someone else's memory.
//   2. Every velocity output is declared vec4.
//   3. Every assignment to one writes four components.
//
// Contracts 2 and 3 are what make a NEW G-Buffer writer's omission visible:
// the author cannot add a `o_Velocity = someVec2;` without this file failing.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] fs::path ShaderRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        }

        [[nodiscard]] std::string ReadWholeFile(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        [[nodiscard]] std::vector<fs::path> AllShaderFiles()
        {
            std::vector<fs::path> files;
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(ShaderRoot(), ec))
            {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".glsl")
                    files.push_back(entry.path());
            }
            return files;
        }

        // `o_Velocity` (forward scene FB) and `o_GBufferVelocity` (deferred)
        // are the two names the engine uses for this attachment.
        //
        // Matched by NAME at any location, not at `location = 3`. The upscale
        // pass declares its copy at location 1 — it writes RT1 of the FSR1
        // depth+velocity FBO — so anchoring on 3 skipped the one shader whose
        // own comment warns that dropping `.ba` "would silently kill the
        // coverage term at every non-native resolution".
        const std::regex kDeclaration{ R"(layout\(location = \d+\) out\s+(\w+)\s+o_(?:GBuffer)?Velocity\s*;)" };
        const std::regex kAssignment{ R"(o_(?:GBuffer)?Velocity\s*=\s*([^;]+);)" };
    } // namespace

    TEST(GBufferCoverageChannelContract, VelocityAttachmentIsFourChannel)
    {
        EXPECT_EQ(GBuffer::s_ColorAttachmentFormats[GBuffer::AttachmentIndex::Velocity],
                  FramebufferTextureFormat::RGBA16F)
            << "G-Buffer RT3 must stay RGBA16F: .b is coverage and .a is the material profile (#1256). "
               "Narrowing it back to RG16F would leave every writer storing four channels into two.";
    }

    TEST(GBufferCoverageChannelContract, ForwardSceneTargetMatchesTheGBuffer)
    {
        // The forward path feeds the SAME temporal resolve, so the two layouts
        // have to agree or the coverage term works in Deferred and reads
        // rubbish in Forward / Forward+.
        const auto attachments = SceneRenderPass::SceneMRTAttachments().Attachments;
        ASSERT_GT(attachments.size(), 3u);
        EXPECT_EQ(attachments[3].TextureFormat, FramebufferTextureFormat::RGBA16F)
            << "The forward scene FB's velocity attachment must match G-Buffer RT3.";
    }

    TEST(GBufferCoverageChannelContract, EveryVelocityOutputIsDeclaredVec4)
    {
        std::vector<std::string> offenders;
        u32 declarationsChecked = 0u;
        for (const auto& path : AllShaderFiles())
        {
            const std::string src = ReadWholeFile(path);
            for (std::sregex_iterator it{ src.begin(), src.end(), kDeclaration }, end; it != end; ++it)
            {
                ++declarationsChecked;
                if ((*it)[1].str() != "vec4")
                    offenders.push_back(path.filename().string() + " declares " + (*it)[1].str());
            }
        }

        // A floor, because every assertion in this file is over a SCAN: if
        // AllShaderFiles() came back empty — a moved shader root, a swallowed
        // filesystem error — the loop body never runs and the test passes
        // having checked nothing. The sibling GBufferBakedGIContractTest guards
        // its own scan the same way.
        EXPECT_GE(declarationsChecked, 25u)
            << "only " << declarationsChecked << " velocity declarations found; the shader scan is not "
                                                 "reaching the tree (expected ~30). This test cannot pass by finding nothing.";

        std::string message;
        for (const auto& offender : offenders)
            message += "\n  " + offender;
        EXPECT_TRUE(offenders.empty())
            << "These shaders declare the velocity attachment with the wrong type." << message
            << "\nRT3 is RGBA16F since #1256: .rg velocity, .b coverage, .a material profile.";
    }

    TEST(GBufferCoverageChannelContract, EveryVelocityWriteCoversAllFourChannels)
    {
        std::vector<std::string> offenders;
        for (const auto& path : AllShaderFiles())
        {
            const std::string src = ReadWholeFile(path);
            // Only shaders that actually declare the attachment — a pass that
            // merely mentions the name in a comment is not a writer.
            if (!std::regex_search(src, kDeclaration))
                continue;

            for (std::sregex_iterator it{ src.begin(), src.end(), kAssignment }, end; it != end; ++it)
            {
                const std::string rhs = (*it)[1].str();
                // Either a vec4 construction, or a straight copy of another
                // four-channel sample (the upscale pass resamples RT3 whole).
                const bool fourChannel = rhs.find("vec4(") != std::string::npos ||
                                         (rhs.find("texture(") != std::string::npos &&
                                          rhs.find(".rg") == std::string::npos);
                if (!fourChannel)
                    offenders.push_back(path.filename().string() + ":  = " + rhs);
            }
        }

        std::string message;
        for (const auto& offender : offenders)
            message += "\n  " + offender;
        EXPECT_TRUE(offenders.empty())
            << "These velocity writes do not cover all four channels." << message
            << "\nAn unwritten MRT component is UNDEFINED, not zero, so a two-channel write leaves the "
               "coverage and profile lanes reading whatever was in that memory. Opaque surfaces write "
               "vec4(velocity, 1.0, 0.0); a subject with real coverage writes its own value in .b.";
    }

    TEST(GBufferCoverageChannelContract, EverySubjectWritesRealCoverageOnEveryPathItRendersOn)
    {
        // The whole point of the channel, and the one contract that failed in
        // the field rather than here.
        //
        // A subject that renders on BOTH paths has two shaders, and the first
        // revision of #1256 wired only the `_GBuffer` (deferred) ones. The
        // editor runs FORWARD by default, so every foliage pixel a user
        // actually saw had an inert coverage lane reading a flat 1.0 — while
        // this suite, the model tests and a headless groom evidence test were
        // all green. It was caught by a live capture.
        //
        // So the pairs are listed explicitly, forward beside deferred, and the
        // check is that each one carries ITS OWN coverage term rather than the
        // opaque default. A subject whose two paths disagree is the failure
        // this case exists to name.
        struct Subject
        {
            const char* File;
            const char* Expected;
            const char* Path;
        };
        constexpr Subject kSubjects[] = {
            // Groom has ONE shader serving both paths, so there is no pair.
            { "GroomStrand.glsl", "alpha", "forward + deferred" },
            { "Foliage_Instance.glsl", "color.a", "forward" },
            { "Foliage_Instance_GBuffer.glsl", "alpha", "deferred" },
            { "Foliage_Impostor.glsl", "card.Coverage", "forward" },
            { "Foliage_Impostor_GBuffer.glsl", "card.Coverage", "deferred" },
        };

        for (const auto& [file, expected, path] : kSubjects)
        {
            const std::string src = ReadWholeFile(ShaderRoot() / file);
            ASSERT_FALSE(src.empty()) << file << " could not be read";

            // EVERY assignment, not just the first: a shader with more than one
            // velocity write (a branchy fragment stage) must not pass on the
            // strength of whichever one the regex happened to reach first.
            bool carriesCoverage = false;
            u32 assignments = 0u;
            for (std::sregex_iterator it{ src.begin(), src.end(), kAssignment }, last; it != last; ++it)
            {
                ++assignments;
                if ((*it)[1].str().find(expected) != std::string::npos)
                    carriesCoverage = true;
            }

            EXPECT_GT(assignments, 0u) << file << " no longer writes the velocity attachment at all";
            EXPECT_TRUE(carriesCoverage)
                << file << " (" << path << ") does not write its own coverage into .b — expected the '"
                << expected << "' term. Writing the opaque 1.0 here leaves the coverage channel inert "
                               "for this subject on this path, which looks identical to the feature working.";
        }
    }

} // namespace OloEngine::Tests
