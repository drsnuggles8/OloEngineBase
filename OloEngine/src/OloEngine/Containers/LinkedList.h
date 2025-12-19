// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file LinkedList.h
 * @brief Various linked list containers
 *
 * Contains:
 * - TLinkedList: Non-intrusive single linked list with separate element storage
 * - TDoubleLinkedList: Non-intrusive double linked list
 * - TList: Simple single linked list
 *
 * For intrusive linked lists, see IntrusiveLinkedList.h
 *
 * Ported from Unreal Engine's Containers/List.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/IntrusiveLinkedList.h"

namespace OloEngine
{
    // Forward declarations
    template<class ElementType>
    class TLinkedList;
    template<class ElementType>
    class TDoubleLinkedList;

    /**
     * @class TLinkedListIterator
     * @brief Iterator for non-intrusive linked lists
     */
    template<class ContainerType, class ElementType>
    class TLinkedListIterator : public TLinkedListIteratorBase<ContainerType>
    {
        using Super = TLinkedListIteratorBase<ContainerType>;

      public:
        [[nodiscard]] explicit TLinkedListIterator(ContainerType* FirstLink)
            : Super(FirstLink)
        {
        }

        /**
         * @brief Arrow operator
         * @return Pointer to the element
         */
        OLO_FINLINE ElementType* operator->() const
        {
            OLO_CORE_ASSERT(this->m_CurrentLink, "Invalid linked list iterator");
            return &(this->m_CurrentLink->GetValue());
        }

        /**
         * @brief Dereference operator
         * @return Reference to the element
         */
        OLO_FINLINE ElementType& operator*() const
        {
            OLO_CORE_ASSERT(this->m_CurrentLink, "Invalid linked list iterator");
            return this->m_CurrentLink->GetValue();
        }
    };

    /**
     * @class TLinkedList
     * @brief Non-intrusive single linked list
     *
     * Unlike TIntrusiveLinkedList, this stores elements separately from the links.
     * This allows storing types that don't inherit from the list node class.
     *
     * Example:
     * @code
     * TLinkedList<int>* Head = nullptr;
     *
     * auto* Node = new TLinkedList<int>(42);
     * Node->LinkHead(Head);
     *
     * for (auto It = TLinkedList<int>::TIterator(Head); It; ++It)
     * {
     *     printf("%d\n", *It);
     * }
     * @endcode
     */
    template<class ElementType>
    class TLinkedList : public TLinkedListBase<TLinkedList<ElementType>, ElementType, TLinkedListIterator>
    {
        using Super = TLinkedListBase<TLinkedList<ElementType>, ElementType, TLinkedListIterator>;

      public:
        /**
         * @brief Default constructor
         */
        [[nodiscard]] TLinkedList()
            : Super(), m_Element()
        {
        }

        /**
         * @brief Constructor with element value
         * @param InElement The element value
         */
        [[nodiscard]] explicit TLinkedList(const ElementType& InElement)
            : Super(), m_Element(InElement)
        {
        }

        /**
         * @brief Move constructor with element value
         * @param InElement The element value to move
         */
        [[nodiscard]] explicit TLinkedList(ElementType&& InElement)
            : Super(), m_Element(MoveTemp(InElement))
        {
        }

        /**
         * @brief Get the element value (const)
         */
        [[nodiscard]] OLO_FINLINE const ElementType& GetValue() const
        {
            return m_Element;
        }

        /**
         * @brief Get the element value (mutable)
         */
        [[nodiscard]] OLO_FINLINE ElementType& GetValue()
        {
            return m_Element;
        }

        /**
         * @brief Dereference operator (const)
         */
        [[nodiscard]] OLO_FINLINE const ElementType& operator*() const
        {
            return m_Element;
        }

        /**
         * @brief Dereference operator (mutable)
         */
        [[nodiscard]] OLO_FINLINE ElementType& operator*()
        {
            return m_Element;
        }

      private:
        ElementType m_Element;
    };

    // ========================================================================
    // TDoubleLinkedList - Double linked list implementation
    // ========================================================================

    /**
     * @class TDoubleLinkedListIterator
     * @brief Iterator for double linked lists
     */
    template<class ElementType>
    class TDoubleLinkedListIterator
    {
      public:
        using NodeType = typename TDoubleLinkedList<ElementType>::TDoubleLinkedListNode;

        [[nodiscard]] explicit TDoubleLinkedListIterator(NodeType* StartingNode)
            : m_CurrentNode(StartingNode)
        {
        }

        /**
         * @brief Advance to next element
         */
        TDoubleLinkedListIterator& operator++()
        {
            OLO_CORE_ASSERT(m_CurrentNode, "Invalid iterator");
            m_CurrentNode = m_CurrentNode->GetNextNode();
            return *this;
        }

        TDoubleLinkedListIterator operator++(int)
        {
            auto Tmp = *this;
            ++(*this);
            return Tmp;
        }

        /**
         * @brief Go to previous element
         */
        TDoubleLinkedListIterator& operator--()
        {
            OLO_CORE_ASSERT(m_CurrentNode, "Invalid iterator");
            m_CurrentNode = m_CurrentNode->GetPrevNode();
            return *this;
        }

        TDoubleLinkedListIterator operator--(int)
        {
            auto Tmp = *this;
            --(*this);
            return Tmp;
        }

        /**
         * @brief Get the node
         */
        [[nodiscard]] NodeType* GetNode() const
        {
            return m_CurrentNode;
        }

        /**
         * @brief Dereference to get element
         */
        [[nodiscard]] ElementType& operator*() const
        {
            OLO_CORE_ASSERT(m_CurrentNode, "Invalid iterator");
            return m_CurrentNode->GetValue();
        }

        [[nodiscard]] ElementType* operator->() const
        {
            OLO_CORE_ASSERT(m_CurrentNode, "Invalid iterator");
            return &m_CurrentNode->GetValue();
        }

        [[nodiscard]] explicit operator bool() const
        {
            return m_CurrentNode != nullptr;
        }

        [[nodiscard]] bool operator==(const TDoubleLinkedListIterator& Other) const
        {
            return m_CurrentNode == Other.m_CurrentNode;
        }

        [[nodiscard]] bool operator!=(const TDoubleLinkedListIterator& Other) const
        {
            return m_CurrentNode != Other.m_CurrentNode;
        }

      private:
        NodeType* m_CurrentNode;
    };

    /**
     * @class TDoubleLinkedList
     * @brief Non-intrusive double linked list
     *
     * A double linked list with nodes containing the element value.
     * Supports forward and backward iteration.
     *
     * Example:
     * @code
     * TDoubleLinkedList<int> List;
     * List.AddTail(1);
     * List.AddTail(2);
     * List.AddHead(0);
     *
     * for (auto It = List.GetHead(); It; ++It)
     * {
     *     printf("%d\n", *It);
     * }
     * @endcode
     */
    template<class ElementType>
    class TDoubleLinkedList
    {
      public:
        /**
         * @class TDoubleLinkedListNode
         * @brief Node in the double linked list
         */
        class TDoubleLinkedListNode
        {
          public:
            friend class TDoubleLinkedList;

            /**
             * @brief Constructor
             * @param InValue Value to store
             */
            [[nodiscard]] explicit TDoubleLinkedListNode(const ElementType& InValue)
                : m_Value(InValue), m_NextNode(nullptr), m_PrevNode(nullptr)
            {
            }

            /**
             * @brief Move constructor
             * @param InValue Value to move
             */
            [[nodiscard]] explicit TDoubleLinkedListNode(ElementType&& InValue)
                : m_Value(MoveTemp(InValue)), m_NextNode(nullptr), m_PrevNode(nullptr)
            {
            }

            /**
             * @brief Get the value (const)
             */
            [[nodiscard]] const ElementType& GetValue() const
            {
                return m_Value;
            }

            /**
             * @brief Get the value (mutable)
             */
            [[nodiscard]] ElementType& GetValue()
            {
                return m_Value;
            }

            /**
             * @brief Get next node
             */
            [[nodiscard]] TDoubleLinkedListNode* GetNextNode()
            {
                return m_NextNode;
            }
            [[nodiscard]] const TDoubleLinkedListNode* GetNextNode() const
            {
                return m_NextNode;
            }

            /**
             * @brief Get previous node
             */
            [[nodiscard]] TDoubleLinkedListNode* GetPrevNode()
            {
                return m_PrevNode;
            }
            [[nodiscard]] const TDoubleLinkedListNode* GetPrevNode() const
            {
                return m_PrevNode;
            }

          protected:
            ElementType m_Value;
            TDoubleLinkedListNode* m_NextNode;
            TDoubleLinkedListNode* m_PrevNode;
        };

        using TIterator = TDoubleLinkedListIterator<ElementType>;
        using TConstIterator = TDoubleLinkedListIterator<const ElementType>;

        /**
         * @brief Default constructor - empty list
         */
        TDoubleLinkedList()
            : m_HeadNode(nullptr), m_TailNode(nullptr), m_ListSize(0)
        {
        }

        /**
         * @brief Destructor - frees all nodes
         */
        ~TDoubleLinkedList()
        {
            Empty();
        }

        // Non-copyable for now (could add copy semantics later)
        TDoubleLinkedList(const TDoubleLinkedList&) = delete;
        TDoubleLinkedList& operator=(const TDoubleLinkedList&) = delete;

        // Movable
        TDoubleLinkedList(TDoubleLinkedList&& Other) noexcept
            : m_HeadNode(Other.m_HeadNode), m_TailNode(Other.m_TailNode), m_ListSize(Other.m_ListSize)
        {
            Other.m_HeadNode = nullptr;
            Other.m_TailNode = nullptr;
            Other.m_ListSize = 0;
        }

        TDoubleLinkedList& operator=(TDoubleLinkedList&& Other) noexcept
        {
            if (this != &Other)
            {
                Empty();
                m_HeadNode = Other.m_HeadNode;
                m_TailNode = Other.m_TailNode;
                m_ListSize = Other.m_ListSize;
                Other.m_HeadNode = nullptr;
                Other.m_TailNode = nullptr;
                Other.m_ListSize = 0;
            }
            return *this;
        }

        // ====== Element Access ======

        /**
         * @brief Get head node
         */
        [[nodiscard]] TDoubleLinkedListNode* GetHead()
        {
            return m_HeadNode;
        }
        [[nodiscard]] const TDoubleLinkedListNode* GetHead() const
        {
            return m_HeadNode;
        }

        /**
         * @brief Get tail node
         */
        [[nodiscard]] TDoubleLinkedListNode* GetTail()
        {
            return m_TailNode;
        }
        [[nodiscard]] const TDoubleLinkedListNode* GetTail() const
        {
            return m_TailNode;
        }

        // ====== Capacity ======

        /**
         * @brief Get number of elements
         */
        [[nodiscard]] i32 Num() const
        {
            return m_ListSize;
        }

        /**
         * @brief Check if list is empty
         */
        [[nodiscard]] bool IsEmpty() const
        {
            return m_ListSize == 0;
        }

        // ====== Modifiers ======

        /**
         * @brief Add element at head
         * @param InValue Value to add
         * @return The new node
         */
        TDoubleLinkedListNode* AddHead(const ElementType& InValue)
        {
            return AddHead(new TDoubleLinkedListNode(InValue));
        }

        TDoubleLinkedListNode* AddHead(ElementType&& InValue)
        {
            return AddHead(new TDoubleLinkedListNode(MoveTemp(InValue)));
        }

        /**
         * @brief Add existing node at head
         * @param NewNode Node to add
         * @return The node
         */
        TDoubleLinkedListNode* AddHead(TDoubleLinkedListNode* NewNode)
        {
            if (NewNode == nullptr)
            {
                return nullptr;
            }

            if (m_HeadNode == nullptr)
            {
                m_HeadNode = m_TailNode = NewNode;
                NewNode->m_NextNode = nullptr;
                NewNode->m_PrevNode = nullptr;
            }
            else
            {
                NewNode->m_NextNode = m_HeadNode;
                NewNode->m_PrevNode = nullptr;
                m_HeadNode->m_PrevNode = NewNode;
                m_HeadNode = NewNode;
            }

            ++m_ListSize;
            return NewNode;
        }

        /**
         * @brief Add element at tail
         * @param InValue Value to add
         * @return The new node
         */
        TDoubleLinkedListNode* AddTail(const ElementType& InValue)
        {
            return AddTail(new TDoubleLinkedListNode(InValue));
        }

        TDoubleLinkedListNode* AddTail(ElementType&& InValue)
        {
            return AddTail(new TDoubleLinkedListNode(MoveTemp(InValue)));
        }

        /**
         * @brief Add existing node at tail
         * @param NewNode Node to add
         * @return The node
         */
        TDoubleLinkedListNode* AddTail(TDoubleLinkedListNode* NewNode)
        {
            if (NewNode == nullptr)
            {
                return nullptr;
            }

            if (m_TailNode == nullptr)
            {
                m_HeadNode = m_TailNode = NewNode;
                NewNode->m_NextNode = nullptr;
                NewNode->m_PrevNode = nullptr;
            }
            else
            {
                NewNode->m_PrevNode = m_TailNode;
                NewNode->m_NextNode = nullptr;
                m_TailNode->m_NextNode = NewNode;
                m_TailNode = NewNode;
            }

            ++m_ListSize;
            return NewNode;
        }

        /**
         * @brief Insert node before another node
         * @param NewNode Node to insert
         * @param NodeBefore Node to insert before
         */
        void InsertNode(TDoubleLinkedListNode* NewNode, TDoubleLinkedListNode* NodeBefore)
        {
            if (NewNode == nullptr)
            {
                return;
            }

            if (NodeBefore == nullptr || NodeBefore == m_HeadNode)
            {
                AddHead(NewNode);
                return;
            }

            NewNode->m_PrevNode = NodeBefore->m_PrevNode;
            NewNode->m_NextNode = NodeBefore;

            if (NewNode->m_PrevNode != nullptr)
            {
                NewNode->m_PrevNode->m_NextNode = NewNode;
            }
            NodeBefore->m_PrevNode = NewNode;

            ++m_ListSize;
        }

        /**
         * @brief Remove a node from the list
         * @param NodeToRemove Node to remove
         * @param bDeleteNode If true, delete the node after removal
         */
        void RemoveNode(TDoubleLinkedListNode* NodeToRemove, bool bDeleteNode = true)
        {
            if (NodeToRemove == nullptr)
            {
                return;
            }

            if (NodeToRemove == m_HeadNode)
            {
                m_HeadNode = NodeToRemove->m_NextNode;
            }
            if (NodeToRemove == m_TailNode)
            {
                m_TailNode = NodeToRemove->m_PrevNode;
            }

            if (NodeToRemove->m_PrevNode != nullptr)
            {
                NodeToRemove->m_PrevNode->m_NextNode = NodeToRemove->m_NextNode;
            }
            if (NodeToRemove->m_NextNode != nullptr)
            {
                NodeToRemove->m_NextNode->m_PrevNode = NodeToRemove->m_PrevNode;
            }

            NodeToRemove->m_NextNode = nullptr;
            NodeToRemove->m_PrevNode = nullptr;

            --m_ListSize;

            if (bDeleteNode)
            {
                delete NodeToRemove;
            }
        }

        /**
         * @brief Remove all nodes
         */
        void Empty()
        {
            TDoubleLinkedListNode* Node = m_HeadNode;
            while (Node != nullptr)
            {
                TDoubleLinkedListNode* Next = Node->m_NextNode;
                delete Node;
                Node = Next;
            }
            m_HeadNode = nullptr;
            m_TailNode = nullptr;
            m_ListSize = 0;
        }

        // ====== Search ======

        /**
         * @brief Find a node with the given value
         * @param InValue Value to find
         * @return Node if found, nullptr otherwise
         */
        [[nodiscard]] TDoubleLinkedListNode* FindNode(const ElementType& InValue)
        {
            for (TDoubleLinkedListNode* Node = m_HeadNode; Node != nullptr; Node = Node->m_NextNode)
            {
                if (Node->m_Value == InValue)
                {
                    return Node;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const TDoubleLinkedListNode* FindNode(const ElementType& InValue) const
        {
            return const_cast<TDoubleLinkedList*>(this)->FindNode(InValue);
        }

        /**
         * @brief Check if list contains a value
         * @param InValue Value to check for
         * @return true if found
         */
        [[nodiscard]] bool Contains(const ElementType& InValue) const
        {
            return FindNode(InValue) != nullptr;
        }

        // ====== Iteration ======

        [[nodiscard]] friend TIterator begin(TDoubleLinkedList& List)
        {
            return TIterator(List.m_HeadNode);
        }
        [[nodiscard]] friend TIterator end(TDoubleLinkedList& /*List*/)
        {
            return TIterator(nullptr);
        }

      private:
        TDoubleLinkedListNode* m_HeadNode;
        TDoubleLinkedListNode* m_TailNode;
        i32 m_ListSize;
    };

    // ========================================================================
    // TList - Simple single linked list
    // ========================================================================

    /**
     * @class TList
     * @brief Simple single linked list
     *
     * A minimal single linked list implementation.
     * Simpler than TLinkedList but less feature-rich.
     */
    template<class ElementType>
    class TList
    {
      public:
        /**
         * @brief List node
         */
        struct TListNode
        {
            ElementType Element;
            TListNode* Next;

            TListNode(const ElementType& InElement, TListNode* InNext = nullptr)
                : Element(InElement), Next(InNext)
            {
            }

            TListNode(ElementType&& InElement, TListNode* InNext = nullptr)
                : Element(MoveTemp(InElement)), Next(InNext)
            {
            }
        };

        TList()
            : m_Head(nullptr)
        {
        }

        ~TList()
        {
            Clear();
        }

        // Non-copyable
        TList(const TList&) = delete;
        TList& operator=(const TList&) = delete;

        // Movable
        TList(TList&& Other) noexcept
            : m_Head(Other.m_Head)
        {
            Other.m_Head = nullptr;
        }

        TList& operator=(TList&& Other) noexcept
        {
            if (this != &Other)
            {
                Clear();
                m_Head = Other.m_Head;
                Other.m_Head = nullptr;
            }
            return *this;
        }

        /**
         * @brief Add element at head
         */
        void Add(const ElementType& Element)
        {
            m_Head = new TListNode(Element, m_Head);
        }

        void Add(ElementType&& Element)
        {
            m_Head = new TListNode(MoveTemp(Element), m_Head);
        }

        /**
         * @brief Get head node
         */
        [[nodiscard]] TListNode* GetHead() const
        {
            return m_Head;
        }

        /**
         * @brief Check if empty
         */
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Head == nullptr;
        }

        /**
         * @brief Clear all nodes
         */
        void Clear()
        {
            TListNode* Node = m_Head;
            while (Node != nullptr)
            {
                TListNode* Next = Node->Next;
                delete Node;
                Node = Next;
            }
            m_Head = nullptr;
        }

      private:
        TListNode* m_Head;
    };

} // namespace OloEngine
