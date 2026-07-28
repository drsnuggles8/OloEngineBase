// =============================================================================
// GLDebugMessageParseTest.cpp
//
// OLO_TEST_LAYER: unit
//
// Pins ParseProgramIDFromMessage (Platform/OpenGL/OpenGLDebug.h), the parse the
// debug callback's per-program NVIDIA-131218 policy keys off: the FIRST
// "Vertex shader in program N is being recompiled" for a given program is the
// expected one-time SPIR-V specialization at its first draw (logged INFO, no
// stack capture); a REPEAT for the same program is state thrash — the
// glClear-against-new-FBO class of bug — and keeps the WARN + call stack.
// Mis-parsing the id would either collapse every program to one bucket (id 0:
// only the first 131218 anywhere logs INFO, all others escalate to WARN) or
// give each message a unique bucket (nothing ever escalates), so the parse is
// the load-bearing part of that policy. Pure CPU, no GL context.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "Platform/OpenGL/OpenGLDebug.h"

namespace OloEngine::Tests
{
    TEST(GLDebugMessageParse, ParsesNvidiaRecompileMessage)
    {
        EXPECT_EQ(ParseProgramIDFromMessage(
                      "Program/shader state performance warning: Vertex shader in program 11 is being recompiled based on GL state."),
                  11u);
        EXPECT_EQ(ParseProgramIDFromMessage("Vertex shader in program 172 is being recompiled"), 172u);
    }

    TEST(GLDebugMessageParse, ReturnsZeroWhenNoProgramNamed)
    {
        EXPECT_EQ(ParseProgramIDFromMessage(nullptr), 0u);
        EXPECT_EQ(ParseProgramIDFromMessage(""), 0u);
        EXPECT_EQ(ParseProgramIDFromMessage("Buffer detailed info: memory usage notification"), 0u);
        // "program " present but no digits after it.
        EXPECT_EQ(ParseProgramIDFromMessage("shader in program is being recompiled"), 0u);
    }

    TEST(GLDebugMessageParse, ParsesFirstProgramReferenceOnly)
    {
        // Two ids in one message: the policy buckets by the first reference,
        // which is the program the driver is complaining about.
        EXPECT_EQ(ParseProgramIDFromMessage("program 7 shares state with program 9"), 7u);
    }
} // namespace OloEngine::Tests
