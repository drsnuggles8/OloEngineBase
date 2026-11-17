// OLO_TEST_LAYER: meta
//
// Every stb header is included as <stb_image/...>, the copy the engine compiles.
//
// stb's writers are compiled once, in ThumbnailCapture.cpp, from <stb_image/stb_image_write.h>.
// Other dependencies bundle their own, older stb_image_write.h (bcdec and xatlas do), and their
// folders are on the include path, so a bare <stb_image_write.h> compiles against one of those.
// When stb changed stbi_write_png's stride from int to ptrdiff_t, six tests that included the
// bare name were calling a 64-bit parameter with a 32-bit argument. On Win64 that argument is
// on the stack, its upper half was whatever the stack held, and FluidVisualEvidenceTest died
// inside the PNG writer or not depending on the build.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The stb includes in `text` that do not name the engine's copy.
        [[nodiscard]] std::vector<std::string> BareStbIncludes(const std::string& text)
        {
            // The file's own name starts with stb_: imstb_truetype.h is Dear ImGui's.
            static const std::regex include(R"re(#\s*include\s*[<"]((?:[^>"]*/)?stb_[A-Za-z0-9_]+\.h)[>"])re");
            std::vector<std::string> bare;
            for (std::sregex_iterator it(text.begin(), text.end(), include), end; it != end; ++it)
            {
                const std::string path = (*it)[1].str();
                if (!path.starts_with("stb_image/"))
                    bare.push_back(path);
            }
            return bare;
        }
    } // namespace

    TEST(VendoredStbIncludeTest, EveryStbHeaderIsTheEnginesCopy)
    {
        const fs::path repo = fs::path(OLO_TEST_EDITOR_ROOT) / "..";
        sizet scanned = 0;
        for (const char* root : { "OloEngine/src", "OloEngine/tests", "OloEditor/src", "OloRuntime/src", "OloServer/src" })
        {
            const fs::path dir = repo / root;
            if (!fs::is_directory(dir))
                continue;
            for (const auto& entry : fs::recursive_directory_iterator(dir))
            {
                const auto extension = entry.path().extension();
                if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h" && extension != ".hpp"))
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                ++scanned;
                for (const std::string& path : BareStbIncludes(text))
                {
                    ADD_FAILURE() << entry.path().lexically_relative(repo).generic_string() << " includes <" << path
                                  << ">, which is another dependency's bundled stb. Include <stb_image/"
                                  << fs::path(path).filename().string() << "> instead";
                }
            }
        }
        EXPECT_GT(scanned, 1000u) << "the source tree was not found, so nothing was checked";
    }

    TEST(VendoredStbIncludeTest, TheRuleSeesBareAndForeignIncludes)
    {
        // Spelled in two pieces so this file does not trip the scan above.
        const std::string directive = std::string("#inc") + "lude ";
        EXPECT_EQ(BareStbIncludes(directive + "<stb_image_write.h>\n").size(), 1u);
        EXPECT_EQ(BareStbIncludes("#  inc" + std::string("lude \"stb_image.h\"\n")).size(), 1u);
        EXPECT_EQ(BareStbIncludes(directive + "<thirdparty/stb_image_write.h>\n").size(), 1u);
        EXPECT_EQ(BareStbIncludes(directive + "<stb_image_resize2.h>\n").size(), 1u) << "a digit in the name hid it";
        EXPECT_TRUE(BareStbIncludes(directive + "<stb_image/stb_image_write.h>\n" + directive + "<stb_image/stb_truetype.h>\n").empty());
        EXPECT_TRUE(BareStbIncludes(directive + "\"OloEngine/Renderer/Texture.h\"\n").empty());
        EXPECT_TRUE(BareStbIncludes(directive + "\"imstb_truetype.h\"\n").empty()) << "Dear ImGui's header is not stb's";
        EXPECT_TRUE(BareStbIncludes(directive + "<Foo/mystb_utils.h>\n").empty());
    }
} // namespace OloEngine::Tests
