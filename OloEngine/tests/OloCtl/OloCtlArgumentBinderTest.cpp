// OLO_TEST_LAYER: unit
//
// Binding `--since-id 12` to `{"sinceId": 12}` from a declared schema (#1125).
//
// The binder types values from the command's own InputSchema and validates
// nothing else — `required`, `enum` and the rest belong to the host, which
// enforces them for an MCP call too. So what is pinned here is the coercion, the
// spellings, and the three local errors the host could never produce because the
// request would not be built: an unknown option, a missing value, and a value
// that is not of the declared type at all.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloCtl/ArgumentBinder.h"

#include <string>
#include <vector>

using OloCtl::BindArguments;
using OloCtl::BindResult;
using OloCtl::FlagSpelling;

using Json = nlohmann::json;

namespace
{
    const Json kSchema{ { "type", "object" },
                        { "properties",
                          { { "sinceId", { { "type", "integer" } } },
                            { "name", { { "type", "string" } } },
                            { "strength", { { "type", "number" } } },
                            { "verboseOutput", { { "type", "boolean" } } },
                            { "names", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                            { "options", { { "type", "object" } } },
                            { "anything", Json::object() } } } };

    BindResult Bind(const std::vector<std::string>& tokens, Json base = Json::object())
    {
        return BindArguments(kSchema, tokens, std::move(base));
    }
} // namespace

TEST(OloCtlArgumentBinder, KebabAndVerbatimSpellingsBothResolve)
{
    EXPECT_EQ(FlagSpelling("sinceId"), "since-id");
    EXPECT_EQ(FlagSpelling("since_id"), "since-id");
    EXPECT_EQ(FlagSpelling("name"), "name");

    const BindResult kebab = Bind({ "--since-id", "12" });
    ASSERT_TRUE(kebab.Ok) << kebab.Error;
    EXPECT_EQ(kebab.Arguments, (Json{ { "sinceId", 12 } }));

    const BindResult verbatim = Bind({ "--sinceId=12" });
    ASSERT_TRUE(verbatim.Ok) << verbatim.Error;
    EXPECT_EQ(verbatim.Arguments, (Json{ { "sinceId", 12 } }));
}

TEST(OloCtlArgumentBinder, ValuesAreTypedByTheDeclaredSchema)
{
    const BindResult bound = Bind({ "--since-id", "12", "--name", "12", "--strength", "0.5" });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_TRUE(bound.Arguments["sinceId"].is_number_integer());
    EXPECT_TRUE(bound.Arguments["name"].is_string()) << "a string property must not become a number";
    EXPECT_DOUBLE_EQ(bound.Arguments["strength"].get<double>(), 0.5);
}

TEST(OloCtlArgumentBinder, ABareBooleanFlagIsTrueAndNoPrefixIsFalse)
{
    const BindResult bare = Bind({ "--verbose-output" });
    ASSERT_TRUE(bare.Ok) << bare.Error;
    EXPECT_EQ(bare.Arguments["verboseOutput"], true);

    const BindResult negated = Bind({ "--no-verbose-output" });
    ASSERT_TRUE(negated.Ok) << negated.Error;
    EXPECT_EQ(negated.Arguments["verboseOutput"], false);

    const BindResult explicitValue = Bind({ "--verbose-output", "false" });
    ASSERT_TRUE(explicitValue.Ok) << explicitValue.Error;
    EXPECT_EQ(explicitValue.Arguments["verboseOutput"], false);
}

TEST(OloCtlArgumentBinder, ABareBooleanFlagDoesNotSwallowTheNextOption)
{
    const BindResult bound = Bind({ "--verbose-output", "--name", "x" });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_EQ(bound.Arguments["verboseOutput"], true);
    EXPECT_EQ(bound.Arguments["name"], "x");
}

TEST(OloCtlArgumentBinder, RepeatingAnArrayOptionAccumulates)
{
    const BindResult bound = Bind({ "--names", "a", "--names", "b" });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_EQ(bound.Arguments["names"], Json::array({ "a", "b" }));
}

TEST(OloCtlArgumentBinder, AJsonArrayLiteralIsAcceptedForAnArrayOption)
{
    const BindResult bound = Bind({ "--names", R"(["a","b"])" });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_EQ(bound.Arguments["names"], Json::array({ "a", "b" }));
}

// Anything else repeated is a typo, and the second value silently replacing the
// first is how a mistyped command line becomes a wrong answer.
TEST(OloCtlArgumentBinder, RepeatingANonArrayOptionIsAnError)
{
    const BindResult bound = Bind({ "--name", "a", "--name", "b" });
    EXPECT_FALSE(bound.Ok);
    EXPECT_NE(bound.Error.find("more than once"), std::string::npos);
}

TEST(OloCtlArgumentBinder, AnUnknownOptionListsWhatIsAccepted)
{
    const BindResult bound = Bind({ "--sinceid2", "1" });
    EXPECT_FALSE(bound.Ok);
    EXPECT_NE(bound.Error.find("--sinceid2"), std::string::npos);
    EXPECT_NE(bound.Error.find("--since-id"), std::string::npos);
}

// The failure this guards is a WRONG ANSWER, not an error: `--names --verbose-output`
// used to bind `names` to `["--verbose-output"]`, silently drop `verboseOutput`, run
// the command and exit 0. The bare-boolean branch always had this guard; every other
// type needed it too.
TEST(OloCtlArgumentBinder, AnOptionNeverSwallowsTheNextOptionAsItsValue)
{
    const BindResult array = Bind({ "--names", "--verbose-output" });
    EXPECT_FALSE(array.Ok);
    EXPECT_NE(array.Error.find("expects a value"), std::string::npos) << array.Error;

    const BindResult string = Bind({ "--name", "--verbose-output" });
    EXPECT_FALSE(string.Ok);

    // Including an option this command does not declare: a stray `--typo` after an
    // option is a mistyped flag far more often than a value, and binding it silently
    // is the same wrong answer.
    const BindResult unknownNext = Bind({ "--name", "--not-a-flag" });
    EXPECT_FALSE(unknownNext.Ok);

    // ...and the error says how to mean it literally.
    EXPECT_NE(string.Error.find("--name=--verbose-output"), std::string::npos) << string.Error;
}

// The documented escape hatch for a value that really does start with two dashes.
TEST(OloCtlArgumentBinder, AnEqualsSignPassesAValueThatBeginsWithDashes)
{
    const BindResult bound = Bind({ "--name=--verbose-output" });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_EQ(bound.Arguments["name"], "--verbose-output");
}

// A single dash is not an option, so a negative number is still a value.
TEST(OloCtlArgumentBinder, ANegativeNumberIsStillAValue)
{
    const BindResult bound = Bind({ "--strength", "-0.5" });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_DOUBLE_EQ(bound.Arguments["strength"].get<double>(), -0.5);
}

TEST(OloCtlArgumentBinder, AnOptionWithNoValueIsAnError)
{
    const BindResult bound = Bind({ "--name" });
    EXPECT_FALSE(bound.Ok);
    EXPECT_NE(bound.Error.find("expects a value"), std::string::npos);
}

TEST(OloCtlArgumentBinder, APositionalArgumentIsAnError)
{
    const BindResult bound = Bind({ "hello" });
    EXPECT_FALSE(bound.Ok);
    EXPECT_NE(bound.Error.find("Unexpected argument"), std::string::npos);
}

TEST(OloCtlArgumentBinder, AMistypedNumberIsRejectedLocally)
{
    EXPECT_FALSE(Bind({ "--since-id", "twelve" }).Ok);
    EXPECT_FALSE(Bind({ "--since-id", "12x" }).Ok);
    EXPECT_FALSE(Bind({ "--strength", "" }).Ok);
}

// CLAUDE.md -> Conventions: every float crossing a boundary is finiteness-checked.
// strtod parses both of these happily, so without the check they would travel to
// a renderer uniform.
TEST(OloCtlArgumentBinder, NonFiniteNumbersAreRefused)
{
    const BindResult nan = Bind({ "--strength", "nan" });
    EXPECT_FALSE(nan.Ok);
    EXPECT_NE(nan.Error.find("finite"), std::string::npos);
    EXPECT_FALSE(Bind({ "--strength", "inf" }).Ok);
    EXPECT_FALSE(Bind({ "--strength", "-inf" }).Ok);
}

TEST(OloCtlArgumentBinder, AnObjectOptionNeedsJson)
{
    const BindResult bad = Bind({ "--options", "not json" });
    EXPECT_FALSE(bad.Ok);

    const BindResult good = Bind({ "--options", R"({"a":1})" });
    ASSERT_TRUE(good.Ok) << good.Error;
    EXPECT_EQ(good.Arguments["options"], (Json{ { "a", 1 } }));
}

// A property whose schema declares no type: JSON if it parses, the raw string
// otherwise. Documented in --help, so it is a rule rather than a guess.
TEST(OloCtlArgumentBinder, AnUntypedOptionTakesJsonThenFallsBackToAString)
{
    const BindResult asJson = Bind({ "--anything", "[1,2]" });
    ASSERT_TRUE(asJson.Ok) << asJson.Error;
    EXPECT_EQ(asJson.Arguments["anything"], Json::array({ 1, 2 }));

    const BindResult asString = Bind({ "--anything", "hello there" });
    ASSERT_TRUE(asString.Ok) << asString.Error;
    EXPECT_EQ(asString.Arguments["anything"], "hello there");
}

TEST(OloCtlArgumentBinder, NamedOptionsOverrideTheArgumentsJsonBase)
{
    const BindResult bound = Bind({ "--name", "flag" }, Json{ { "name", "base" }, { "sinceId", 3 } });
    ASSERT_TRUE(bound.Ok) << bound.Error;
    EXPECT_EQ(bound.Arguments["name"], "flag");
    EXPECT_EQ(bound.Arguments["sinceId"], 3) << "a base key with no matching option must survive";
}

TEST(OloCtlArgumentBinder, ACommandWithNoDeclaredPropertiesRejectsEveryOption)
{
    const Json empty{ { "type", "object" } };
    const BindResult none = BindArguments(empty, {});
    EXPECT_TRUE(none.Ok);
    EXPECT_EQ(none.Arguments, Json::object());

    const BindResult unexpected = BindArguments(empty, { "--anything", "1" });
    EXPECT_FALSE(unexpected.Ok);
    EXPECT_NE(unexpected.Error.find("takes no arguments"), std::string::npos);
}
