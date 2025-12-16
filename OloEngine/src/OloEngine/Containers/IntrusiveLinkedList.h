// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /**
     * Base linked list iterator class
     */
    template <class ContainerType>
    class TLinkedListIteratorBase
    {
    public:
        [[nodiscard]] explicit TLinkedListIteratorBase(ContainerType* FirstLink)
            : m_CurrentLink(FirstLink)
        {
        }

        /**
         * Advances the iterator to the next element.
         */
        OLO_FINLINE void Next()
        {
            OLO_CORE_ASSERT(m_CurrentLink, "Invalid linked list iterator");
            m_CurrentLink = static_cast<ContainerType*>(m_CurrentLink->GetNextLink());
        }

        OLO_FINLINE TLinkedListIteratorBase& operator++()
        {
            Next();
            return *this;
        }

        OLO_FINLINE TLinkedListIteratorBase operator++(int)
        {
            auto Tmp = *this;
            Next();
            return Tmp;
        }

        /** Conversion to "bool" returning true if the iterator is valid. */
        [[nodiscard]] OLO_FINLINE explicit operator bool() const
        { 
            return m_CurrentLink != nullptr;
        }

        [[nodiscard]] OLO_FINLINE bool operator==(const TLinkedListIteratorBase& Rhs) const { return m_CurrentLink == Rhs.m_CurrentLink; }
        [[nodiscard]] OLO_FINLINE bool operator!=(const TLinkedListIteratorBase& Rhs) const { return m_CurrentLink != Rhs.m_CurrentLink; }

    protected:
        ContainerType* m_CurrentLink;
    };

    /**
     * Iterator for intrusive linked lists
     */
    template <class ContainerType, class ElementType>
    class TIntrusiveLinkedListIterator : public TLinkedListIteratorBase<ElementType>
    {
        using Super = TLinkedListIteratorBase<ElementType>;

    public:
        [[nodiscard]] explicit TIntrusiveLinkedListIterator(ElementType* FirstLink)
            : Super(FirstLink)
        {
        }

        // Accessors.
        OLO_FINLINE ElementType& operator->() const
        {
            OLO_CORE_ASSERT(this->m_CurrentLink, "Invalid linked list iterator");
            return *(this->m_CurrentLink);
        }

        OLO_FINLINE ElementType& operator*() const
        {
            OLO_CORE_ASSERT(this->m_CurrentLink, "Invalid linked list iterator");
            return *(this->m_CurrentLink);
        }
    };

    /**
     * Base linked list class, used to implement methods shared by intrusive/non-intrusive linked lists
     */
    template <class ContainerType, class ElementType, template<class, class> class IteratorType>
    class TLinkedListBase
    {
    public:
        /**
         * Used to iterate over the elements of a linked list.
         */
        using TIterator = IteratorType<ContainerType, ElementType>;
        using TConstIterator = IteratorType<ContainerType, const ElementType>;

        /**
         * Default constructor (empty list)
         */
        [[nodiscard]] TLinkedListBase()
            : m_NextLink(nullptr)
            , m_PrevLink(nullptr)
        {
        }

        /**
         * Removes this element from the list in constant time.
         * This function is safe to call even if the element is not linked.
         */
        OLO_FINLINE void Unlink()
        {
            if (m_NextLink)
            {
                m_NextLink->m_PrevLink = m_PrevLink;
            }
            if (m_PrevLink)
            {
                *m_PrevLink = m_NextLink;
            }
            // Make it safe to call Unlink again.
            m_NextLink = nullptr;
            m_PrevLink = nullptr;
        }

        /**
         * Adds this element to a list, before the given element.
         * @param Before The link to insert this element before.
         */
        OLO_FINLINE void LinkBefore(ContainerType* Before)
        {
            OLO_CORE_ASSERT(Before != nullptr, "Before cannot be null");

            m_PrevLink = Before->m_PrevLink;
            Before->m_PrevLink = &m_NextLink;

            m_NextLink = Before;

            if (m_PrevLink != nullptr)
            {
                *m_PrevLink = static_cast<ContainerType*>(this);
            }
        }

        /**
         * Adds this element to the linked list, after the specified element
         * @param After The link to insert this element after.
         */
        OLO_FINLINE void LinkAfter(ContainerType* After)
        {
            OLO_CORE_ASSERT(After != nullptr, "After cannot be null");

            m_PrevLink = &After->m_NextLink;
            m_NextLink = *m_PrevLink;
            *m_PrevLink = static_cast<ContainerType*>(this);

            if (m_NextLink != nullptr)
            {
                m_NextLink->m_PrevLink = &m_NextLink;
            }
        }

        /**
         * Adds this element to the linked list, replacing the specified element.
         * This is equivalent to calling LinkBefore(Replace); Replace->Unlink();
         * @param Replace Pointer to the element to be replaced
         */
        OLO_FINLINE void LinkReplace(ContainerType* Replace)
        {
            OLO_CORE_ASSERT(Replace != nullptr, "Replace cannot be null");

            ContainerType**& ReplacePrev = Replace->m_PrevLink;
            ContainerType*& ReplaceNext = Replace->m_NextLink;

            m_PrevLink = ReplacePrev;
            m_NextLink = ReplaceNext;

            if (m_PrevLink != nullptr)
            {
                *m_PrevLink = static_cast<ContainerType*>(this);
            }

            if (m_NextLink != nullptr)
            {
                m_NextLink->m_PrevLink = &m_NextLink;
            }

            ReplacePrev = nullptr;
            ReplaceNext = nullptr;
        }

        /**
         * Adds this element as the head of the linked list, linking the input Head pointer to this element,
         * so that when the element is linked/unlinked, the Head linked list pointer will be correctly updated.
         *
         * If Head already has an element, this functions like LinkBefore.
         *
         * @param Head Pointer to the head of the linked list - this pointer should be the main reference point for the linked list
         */
        OLO_FINLINE void LinkHead(ContainerType*& Head)
        {
            if (Head != nullptr)
            {
                Head->m_PrevLink = &m_NextLink;
            }

            m_NextLink = Head;
            m_PrevLink = &Head;
            Head = static_cast<ContainerType*>(this);
        }

        /**
         * Returns whether element is currently linked.
         * @return true if currently linked, false otherwise
         */
        [[nodiscard]] OLO_FINLINE bool IsLinked() const
        {
            return m_PrevLink != nullptr;
        }

        [[nodiscard]] OLO_FINLINE ContainerType** GetPrevLink() const
        {
            return m_PrevLink;
        }

        [[nodiscard]] OLO_FINLINE ContainerType* GetNextLink() const
        {
            return m_NextLink;
        }

        [[nodiscard]] OLO_FINLINE ContainerType* Next()
        {
            return m_NextLink;
        }

    private:
        /** The next link in the linked list */
        ContainerType* m_NextLink;

        /** Pointer to 'NextLink', within the previous link in the linked list */
        ContainerType** m_PrevLink;

        [[nodiscard]] OLO_FINLINE friend TIterator begin(ContainerType& List) { return TIterator(&List); }
        [[nodiscard]] OLO_FINLINE friend TConstIterator begin(const ContainerType& List) { return TConstIterator(const_cast<ContainerType*>(&List)); }
        [[nodiscard]] OLO_FINLINE friend TIterator end(ContainerType& /*List*/) { return TIterator(nullptr); }
        [[nodiscard]] OLO_FINLINE friend TConstIterator end(const ContainerType& /*List*/) { return TConstIterator(nullptr); }
    };

    /**
     * Encapsulates a link in a single linked list with constant access time.
     * Structs/classes must inherit this, to use it, e.g: struct FMyStruct : public TIntrusiveLinkedList<FMyStruct>
     *
     * This linked list is intrusive, i.e. the element is a subclass of this link, so that each link IS the element.
     *
     * Never reference TIntrusiveLinkedList outside of the above class/struct inheritance, only ever refer to the struct, e.g:
     *     FMyStruct* MyLinkedList = nullptr;
     *
     *     FMyStruct* StructLink = new FMyStruct();
     *     StructLink->LinkHead(MyLinkedList);
     *
     *     for (FMyStruct::TIterator It(MyLinkedList); It; It.Next())
     *     {
     *         ...
     *     }
     */
    template <class ElementType>
    class TIntrusiveLinkedList : public TLinkedListBase<ElementType, ElementType, TIntrusiveLinkedListIterator>
    {
        using Super = TLinkedListBase<ElementType, ElementType, TIntrusiveLinkedListIterator>;

    public:
        /** Default constructor (empty list). */
        [[nodiscard]] TIntrusiveLinkedList()
            : Super()
        {
        }
    };
} // namespace OloEngine
