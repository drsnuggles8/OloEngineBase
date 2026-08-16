#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// VisualScriptNodeLibraryTest — the registry's structural invariants
// (issue #634, AC#4: "40+ standard nodes across Events / Flow / Math-Logic /
// Variables / ECS / Spawn / Queries / Utility, behind a shared type-name
// registry/factory").
//
// The count assertion is the acceptance criterion made checkable. The rest are
// invariants the compiler and the (future) editor canvas BOTH depend on and
// that nothing else would catch:
//
//   * a duplicate type name silently replaces the earlier registration, so a
//     saved graph starts running a different node's body;
//   * a node with neither an Evaluate nor an Execute body compiles fine and
//     then does nothing at runtime;
//   * a Pure node carrying exec pins is a contradiction — the VM pull-evaluates
//     it and would never run its exec side;
//   * an Event node with an exec INPUT can never be entered.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"

#include <set>
#include <string>

using namespace OloEngine;
using namespace OloEngine::VisualScript;

namespace
{
    class VisualScriptNodeLibraryTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            NodeRegistry::EnsureStandardLibrary();
        }
    };
} // namespace

TEST_F(VisualScriptNodeLibraryTest, ShipsAtLeastFortyNodes)
{
    EXPECT_GE(NodeRegistry::Get().GetCount(), 40u)
        << "issue #634 AC#4 asks for 40+ standard nodes; the registry is the count";
}

TEST_F(VisualScriptNodeLibraryTest, EveryRequiredCategoryIsPopulated)
{
    std::set<std::string> categories;
    for (const NodeTypeDescriptor* type : NodeRegistry::Get().GetSorted())
    {
        categories.insert(type->m_Category);
    }

    for (const char* required : { "Events", "Flow", "Math", "Logic", "Vector", "Variables", "Entity", "Queries", "Utility", "Functions", "Scripting" })
    {
        EXPECT_TRUE(categories.contains(required)) << "no nodes registered in category '" << required << "'";
    }
}

TEST_F(VisualScriptNodeLibraryTest, EveryNodeIsWellFormed)
{
    for (const NodeTypeDescriptor* type : NodeRegistry::Get().GetSorted())
    {
        SCOPED_TRACE(type->m_TypeName);

        EXPECT_FALSE(type->m_TypeName.empty());
        EXPECT_FALSE(type->m_DisplayName.empty());
        EXPECT_FALSE(type->m_Category.empty());
        EXPECT_FALSE(type->m_Tooltip.empty()) << "the context-search menu shows this";

        const bool isPure = HasFlag(type->m_Flags, NodeFlags::Pure);
        if (isPure)
        {
            EXPECT_TRUE(static_cast<bool>(type->m_Evaluate)) << "a Pure node is pull-evaluated and needs an Evaluate body";
            EXPECT_FALSE(static_cast<bool>(type->m_Execute)) << "a Pure node's Execute body would never run";
        }
        else
        {
            EXPECT_TRUE(static_cast<bool>(type->m_Execute)) << "a non-Pure node is entered by an exec token and needs an Execute body";
        }

        // Pin names must be unique WITHIN a direction: link endpoints resolve by
        // (node, direction, name), so two same-named output pins make one of
        // them unreachable. Across directions they may repeat (Set Variable's
        // "Value" in and "Out" out are separate pins).
        std::set<std::string> inputs;
        std::set<std::string> outputs;
        for (const PinDescriptor& pin : type->m_Pins)
        {
            EXPECT_FALSE(pin.m_Name.empty());
            auto& seen = pin.m_Direction == PinDirection::Input ? inputs : outputs;
            EXPECT_TRUE(seen.insert(pin.m_Name).second) << "duplicate pin name '" << pin.m_Name << "'";
        }
    }
}

TEST_F(VisualScriptNodeLibraryTest, PureNodesHaveNoExecPins)
{
    for (const NodeTypeDescriptor* type : NodeRegistry::Get().GetSorted())
    {
        if (!HasFlag(type->m_Flags, NodeFlags::Pure))
        {
            continue;
        }
        SCOPED_TRACE(type->m_TypeName);
        for (const PinDescriptor& pin : type->m_Pins)
        {
            EXPECT_FALSE(IsExecPin(pin.m_Type)) << "pin '" << pin.m_Name << "' is an exec pin on a Pure node";
        }
    }
}

TEST_F(VisualScriptNodeLibraryTest, EventNodesHaveNoExecInputAndOneExecOutput)
{
    u32 eventNodes = 0;
    for (const NodeTypeDescriptor* type : NodeRegistry::Get().GetSorted())
    {
        if (!HasFlag(type->m_Flags, NodeFlags::Event))
        {
            continue;
        }
        ++eventNodes;
        SCOPED_TRACE(type->m_TypeName);

        u32 execOutputs = 0;
        for (const PinDescriptor& pin : type->m_Pins)
        {
            if (!IsExecPin(pin.m_Type))
            {
                continue;
            }
            EXPECT_EQ(pin.m_Direction, PinDirection::Output)
                << "an event node with an exec INPUT can never be entered — the VM starts runs AT it";
            ++execOutputs;
        }
        EXPECT_EQ(execOutputs, 1u) << "an event node hands control to exactly one branch";
    }
    EXPECT_GE(eventNodes, 5u) << "AC#4 lists OnBeginPlay, OnUpdate, collision, trigger and custom events";
}

TEST_F(VisualScriptNodeLibraryTest, LatentNodesAreNotPure)
{
    // A Latent node parks a resume record keyed by its own node index. Marking
    // one Pure would mean it is pull-evaluated — possibly several times per
    // step — and each pull would park another record.
    for (const NodeTypeDescriptor* type : NodeRegistry::Get().GetSorted())
    {
        if (HasFlag(type->m_Flags, NodeFlags::Latent))
        {
            SCOPED_TRACE(type->m_TypeName);
            EXPECT_FALSE(HasFlag(type->m_Flags, NodeFlags::Pure));
        }
    }
}

TEST_F(VisualScriptNodeLibraryTest, WellKnownTypeNamesResolve)
{
    // The compiler special-cases these by name. A rename that misses one of
    // these constants produces a graph that compiles and never runs, which is
    // the single hardest failure here to notice.
    for (const std::string_view name : { NodeTypes::kOnBeginPlay, NodeTypes::kOnUpdate, NodeTypes::kOnEndPlay,
                                         NodeTypes::kOnCollisionEnter, NodeTypes::kOnTriggerEnter, NodeTypes::kCustomEvent,
                                         NodeTypes::kOnGameplayEvent, NodeTypes::kFunctionEntry, NodeTypes::kFunctionReturn,
                                         NodeTypes::kFunctionCall, NodeTypes::kGetVariable, NodeTypes::kSetVariable,
                                         NodeTypes::kSequence })
    {
        EXPECT_NE(NodeRegistry::Get().Find(name), nullptr) << "well-known node type '" << name << "' is not registered";
    }
}

TEST_F(VisualScriptNodeLibraryTest, SortOrderIsByCategoryThenDisplayName)
{
    // The editor's context-search menu and any generated documentation read this
    // order, so it must be a real, checkable ordering rather than "whatever the
    // hash map happened to yield". Asserting adjacent pairs is what actually
    // pins it — comparing GetSorted() against a second GetSorted() call only
    // proves the cache is a cache.
    const auto sorted = NodeRegistry::Get().GetSorted();
    ASSERT_GE(sorted.size(), 2u);

    for (sizet i = 1; i < sorted.size(); ++i)
    {
        const NodeTypeDescriptor* previous = sorted[i - 1];
        const NodeTypeDescriptor* current = sorted[i];
        SCOPED_TRACE(previous->m_TypeName + " then " + current->m_TypeName);

        // Non-strict: two node types may legitimately share a category, and two
        // in the same category may share a display name.
        const bool ordered = previous->m_Category < current->m_Category ||
                             (previous->m_Category == current->m_Category && previous->m_DisplayName <= current->m_DisplayName);
        EXPECT_TRUE(ordered) << "sorted order must be (category, display name) ascending";
    }
}

TEST_F(VisualScriptNodeLibraryTest, RegisterRejectsMalformedDescriptors)
{
    NodeRegistry registry;
    EXPECT_FALSE(registry.Register(NodeTypeDescriptor{})) << "an empty type name must be refused";

    NodeTypeDescriptor bodyless;
    bodyless.m_TypeName = "Test.Bodyless";
    EXPECT_FALSE(registry.Register(std::move(bodyless)))
        << "a node with neither an Evaluate nor an Execute body would compile and then do nothing";

    NodeTypeDescriptor good;
    good.m_TypeName = "Test.Good";
    good.m_Execute = [](NodeContext&) {};
    EXPECT_TRUE(registry.Register(std::move(good)));
    EXPECT_EQ(registry.GetCount(), 1u);
}
