#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// Steam Lua binding coverage (#644). Modelled on
// Networking/NetworkScriptBindingCoverageTest.cpp, and using the same real-bindings fixture
// shape as Lua/LuaBindingTest.cpp.
//
// The `Steam` table in LuaScriptGlue.cpp is a hand-maintained list. A function that is renamed,
// dropped, or never registered compiles and links perfectly and fails at RUNTIME — on the first
// script that calls it, in whatever game happens to use that feature. That is the quiet-failure
// shape this repo writes coverage tests for.
//
// Two things are checked, and the second is the one that matters:
//
//   1. every documented Steam.* function EXISTS on the table;
//   2. every one of them is CALLABLE WITH STEAM ABSENT and returns a sane inert value.
//
// (2) is the script-layer expression of the whole degradation contract: a game script must be
// able to call Steam unconditionally on a machine with no Steam client, no SDK and no Valve
// account, and simply have nothing happen. If any of these errored instead, every script that
// touched Steam would break for every contributor without Steamworks — which is nearly all of
// them, and all of CI.
//
// Runs on every configuration. Needs no Steam client. Engine services are not initialised, which
// is exactly the "Steam absent" state under test.

#include "OloEngine/Scripting/Lua/LuaScriptGlue.h"
#include "Platform/Steam/SteamManager.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    // The full documented surface of the Steam table. Adding a binding without adding it here
    // leaves it untested; removing one without removing it here fails loudly. That friction is
    // the point.
    const std::vector<std::string>& ExpectedSteamFunctions()
    {
        static const std::vector<std::string> s_Functions{
            "isAvailable",
            "getAppID",
            "getPersonaName",

            "unlockAchievement",
            "clearAchievement",
            "isAchievementUnlocked",
            "storeStats",

            "setRichPresence",
            "clearRichPresence",

            "isOverlayActive",

            "isCloudEnabled",
            "cloudWrite",
            "cloudRead",
            "cloudExists",
            "cloudDelete",
            "cloudEnumerate",
            "getCloudQuota",
        };
        return s_Functions;
    }

    class SteamScriptBindingTest : public ::testing::Test
    {
      protected:
        sol::state lua;

        void SetUp() override
        {
            lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table);
            OloEngine::LuaScriptGlue::RegisterAllTypes(lua);
        }

        // Run a chunk, failing with Lua's own message rather than a bare bool.
        void RunScript(const std::string& source)
        {
            const sol::protected_function_result result = lua.safe_script(source, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error error = result;
                FAIL() << "Lua error: " << error.what() << "\n--- script ---\n"
                       << source;
            }
        }
    };

    TEST_F(SteamScriptBindingTest, SteamTableExists)
    {
        const sol::object steam = lua["Steam"];
        ASSERT_TRUE(steam.valid()) << "the global `Steam` table was never registered";
        EXPECT_EQ(steam.get_type(), sol::type::table);
    }

    TEST_F(SteamScriptBindingTest, EveryDocumentedFunctionIsRegistered)
    {
        const sol::object steamObject = lua["Steam"];
        ASSERT_TRUE(steamObject.valid());
        const sol::table steam = steamObject.as<sol::table>();

        std::vector<std::string> missing;
        for (const std::string& name : ExpectedSteamFunctions())
        {
            const sol::object entry = steam[name];
            if (!entry.valid() || entry.get_type() != sol::type::function)
            {
                missing.push_back(name);
            }
        }

        std::string report;
        for (const std::string& name : missing)
        {
            report += "\n  Steam." + name;
        }
        EXPECT_TRUE(missing.empty()) << "missing or non-function Steam bindings:" << report;
    }

    // The INVERSE direction, and the one that actually decays over time.
    //
    // The check above only catches a binding that was removed. A binding that is ADDED without
    // being listed sails through — it is registered, callable from scripts, and covered by
    // nothing: not the "callable when Steam is absent" test below, not the documented surface.
    // That is how an undocumented Steam function ends up shipping with no degradation guarantee.
    // Both directions together are what make the list a contract instead of a wish.
    TEST_F(SteamScriptBindingTest, NoUndocumentedFunctionsOnTheSteamTable)
    {
        const sol::object steamObject = lua["Steam"];
        ASSERT_TRUE(steamObject.valid());
        const sol::table steam = steamObject.as<sol::table>();

        const std::vector<std::string>& expected = ExpectedSteamFunctions();

        std::vector<std::string> undocumented;
        steam.for_each(
            [&](const sol::object& key, const sol::object& value)
            {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::function)
                {
                    return;
                }
                const std::string name = key.as<std::string>();
                if (std::find(expected.begin(), expected.end(), name) == expected.end())
                {
                    undocumented.push_back(name);
                }
            });

        std::string report;
        for (const std::string& name : undocumented)
        {
            report += "\n  Steam." + name;
        }
        EXPECT_TRUE(undocumented.empty())
            << "Steam bindings exist that ExpectedSteamFunctions() does not list. Add them there "
               "(and to the absent-Steam callability test) so they carry the same degradation "
               "guarantee as the rest:"
            << report;
    }

    // THE test in this file. Every binding, called with Steam absent, must return rather than
    // error. A script doing `if Steam.unlockAchievement("X") then ... end` has to work on a
    // machine that has never had Steam installed.
    TEST_F(SteamScriptBindingTest, EveryFunctionIsCallableAndInertWhenSteamIsAbsent)
    {
        ASSERT_FALSE(OloEngine::SteamManager::IsAvailable())
            << "this test is meaningless if Steam is actually available";

        RunScript(R"lua(
            assert(Steam.isAvailable() == false, "isAvailable should be false with no Steam")
            assert(Steam.getAppID() == 0, "getAppID should be 0 with no Steam")
            assert(Steam.getPersonaName() == "", "getPersonaName should be empty with no Steam")

            assert(Steam.unlockAchievement("ACH_ANY") == false)
            assert(Steam.clearAchievement("ACH_ANY") == false)
            assert(Steam.isAchievementUnlocked("ACH_ANY") == false)
            assert(Steam.storeStats() == false)

            assert(Steam.setRichPresence("status", "testing") == false)
            Steam.clearRichPresence()

            assert(Steam.isOverlayActive() == false)

            assert(Steam.isCloudEnabled() == false)
            assert(Steam.cloudWrite("slot.sav", "payload") == false)
            assert(Steam.cloudRead("slot.sav") == nil, "cloudRead should be nil, not empty string")
            assert(Steam.cloudExists("slot.sav") == false)
            assert(Steam.cloudDelete("slot.sav") == false)

            local files = Steam.cloudEnumerate()
            assert(type(files) == "table", "cloudEnumerate should return a table")
            assert(#files == 0, "cloudEnumerate should be empty with no Steam")

            local total, available = Steam.getCloudQuota()
            assert(total == 0 and available == 0)
        )lua");
    }

    // Degenerate arguments must not crash the binding layer either — scripts pass empty strings
    // far more often than anyone expects.
    TEST_F(SteamScriptBindingTest, DegenerateArgumentsDoNotError)
    {
        RunScript(R"lua(
            Steam.unlockAchievement("")
            Steam.clearAchievement("")
            Steam.isAchievementUnlocked("")
            Steam.setRichPresence("", "")
            Steam.cloudWrite("", "")
            Steam.cloudRead("")
            Steam.cloudExists("")
            Steam.cloudDelete("")
        )lua");
    }

    // cloudRead returning nil rather than "" is a deliberate contract: it lets scripts write
    // `local data = Steam.cloudRead(name) or default`, which is the idiom they will reach for.
    TEST_F(SteamScriptBindingTest, CloudReadReturnsNilNotEmptyStringWhenAbsent)
    {
        RunScript(R"lua(
            local data = Steam.cloudRead("definitely-missing.sav") or "fallback"
            assert(data == "fallback", "cloudRead must be nil-able so `or` works")
        )lua");
    }
} // namespace
