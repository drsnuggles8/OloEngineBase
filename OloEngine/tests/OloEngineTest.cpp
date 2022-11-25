// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
// TODO(olbu): Implement more tests, look at https://github.com/turanszkij/WickedEngine/blob/master/Tests/Tests.cpp for some ideas

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return ::RUN_ALL_TESTS();
}
