#include <gtest/gtest.h>
#include "OloEngine/Core/Reflection/StringUtils.h"

using namespace OloEngine::Core::Reflection::StringUtils;

TEST(ReflectionStringTest, BasicStringSplit)
{
	constexpr std::string_view test1 = "InputA, InputB";
	constexpr auto result1 = SplitString<2>(test1, ", ");
	
	EXPECT_EQ(result1[0], "InputA");
	EXPECT_EQ(result1[1], "InputB");
	
	constexpr std::string_view test2 = "&AddNodeReflected::in_InputA, &AddNodeReflected::in_InputB";
	constexpr auto result2 = SplitString<2>(test2, ", ");
	
	EXPECT_EQ(result2[0], "&AddNodeReflected::in_InputA");
	EXPECT_EQ(result2[1], "&AddNodeReflected::in_InputB");
}

TEST(ReflectionStringTest, PrefixAndSuffixRemoval)
{
	constexpr std::string_view test = "in_InputA";
	constexpr auto result = RemovePrefixAndSuffix(test);
	
	EXPECT_EQ(result, "InputA");
}

TEST(ReflectionStringTest, NamespaceRemoval)
{
	constexpr std::array<std::string_view, 2> input = {"&AddNodeReflected::in_InputA", "&AddNodeReflected::in_InputB"};
	constexpr auto result = RemoveNamespace<2>(input);
	
	EXPECT_EQ(result[0], "in_InputA");
	EXPECT_EQ(result[1], "in_InputB");
}