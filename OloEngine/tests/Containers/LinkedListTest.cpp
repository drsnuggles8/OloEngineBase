// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Containers/LinkedList.h"
#include "OloEngine/Containers/String.h"

#include <type_traits>
#include <utility>

using namespace OloEngine;

namespace
{
    using List = TDoubleLinkedList<FString>;
    using ConstIterator = decltype(begin(std::declval<const List&>()));
    static_assert(std::is_same_v<decltype(*std::declval<ConstIterator>()), const FString&>);
    static_assert(std::is_same_v<decltype(std::declval<ConstIterator>().operator->()), const FString*>);
    static_assert(std::is_same_v<decltype(std::declval<ConstIterator>().GetNode()), const List::TDoubleLinkedListNode*>);
    static_assert(!std::is_assignable_v<decltype(*std::declval<ConstIterator>()), FString>);
} // namespace

TEST(DoubleLinkedList, ConstIterationUsesStoredNodesAndPreservesOrder)
{
    List values;
    EXPECT_EQ(begin(std::as_const(values)), end(std::as_const(values)));
    values.AddTail(FString("first"));
    values.AddTail(FString("second"));
    values.AddTail(FString("third"));

    const List& readOnly = values;
    auto it = begin(readOnly);
    EXPECT_EQ(it.GetNode(), readOnly.GetHead());
    EXPECT_EQ(&*it, &values.GetHead()->GetValue());
    EXPECT_EQ(*it++, "first");
    EXPECT_EQ(it->Len(), 6);
    EXPECT_EQ(*it++, "second");
    EXPECT_EQ(*it, "third");
    EXPECT_EQ(*--it, "second");

    i32 visited = 0;
    for (const FString& value : readOnly)
    {
        EXPECT_EQ(value, visited == 0 ? "first" : visited == 1 ? "second"
                                                               : "third");
        ++visited;
    }
    EXPECT_EQ(visited, 3);

    for (FString& value : values)
        value += "!";
    EXPECT_EQ(readOnly.GetHead()->GetValue(), "first!");
    EXPECT_EQ(readOnly.GetTail()->GetValue(), "third!");
}
