#pragma once

#include "OloEngine/Core/Base.h"

#include <concepts>
#include <limits>
#include <utility>
#include <vector>

namespace OloEngine
{
    // @brief Pluggable eviction for GPUPagedCache (issue #704, ported from the
    // VoxelEngine reference's gpu_cache_policy.h).
    //
    // Divergence from the reference: SelectVictim() (assert-on-empty) became
    // TrySelectVictim(exclude, out) -> bool. The cache calls it in a loop under
    // memory pressure, and two situations the reference could not express are
    // routine there: the policy has no evictable entry left (allocation must
    // FAIL, not hang), and the only candidate is the requesting object itself
    // (evicting it would free pages the allocation is about to chain).
    template<typename Policy, typename ObjectID>
    concept EvictionPolicy =
        requires(Policy policy, const ObjectID& objectID, ObjectID& outVictim, u32 capacity) {
            typename Policy::Handle;

            Policy(capacity);

            { policy.OnAccess(std::declval<typename Policy::Handle&>()) } noexcept -> std::same_as<void>;
            { policy.OnInsert(objectID) } noexcept -> std::same_as<typename Policy::Handle>;
            { policy.OnRemove(std::declval<typename Policy::Handle&>()) } noexcept -> std::same_as<void>;
            { policy.TrySelectVictim(objectID, outVictim) } noexcept -> std::same_as<bool>;
        };

    // @brief Strict least-recently-used: intrusive doubly-linked list, OnAccess
    // moves to front, victim is the tail.
    template<typename ObjectID>
    class LRUPolicy
    {
      public:
        using Index = u32;
        static constexpr Index kNullIndex = std::numeric_limits<Index>::max();

        struct Handle
        {
            Index m_Index = kNullIndex;
        };

        explicit LRUPolicy(u32 capacity)
        {
            m_Nodes.reserve(capacity);
            m_FreeList.reserve(capacity);
        }

        void OnAccess(Handle& handle) noexcept
        {
            OLO_CORE_ASSERT(IsValid(handle.m_Index), "LRUPolicy::OnAccess on an invalid handle");
            MoveToFront(handle.m_Index);
        }

        [[nodiscard]] Handle OnInsert(const ObjectID& id) noexcept
        {
            const Index idx = AllocateNode();
            Node& node = m_Nodes[idx];
            node.m_Id = id;
            node.m_Prev = kNullIndex;
            node.m_Next = kNullIndex;
            InsertFront(idx);
            return Handle{ idx };
        }

        void OnRemove(Handle& handle) noexcept
        {
            if (!IsValid(handle.m_Index))
            {
                return;
            }
            RemoveNode(handle.m_Index);
            m_FreeList.push_back(handle.m_Index);
            handle.m_Index = kNullIndex;
        }

        [[nodiscard]] bool TrySelectVictim(const ObjectID& exclude, ObjectID& outVictim) noexcept
        {
            for (Index idx = m_Tail; idx != kNullIndex; idx = m_Nodes[idx].m_Prev)
            {
                if (!(m_Nodes[idx].m_Id == exclude))
                {
                    outVictim = m_Nodes[idx].m_Id;
                    return true;
                }
            }
            return false;
        }

      private:
        struct Node
        {
            ObjectID m_Id{};
            Index m_Prev = kNullIndex;
            Index m_Next = kNullIndex;
        };

        [[nodiscard]] bool IsValid(Index i) const noexcept
        {
            return i != kNullIndex && i < m_Nodes.size();
        }

        [[nodiscard]] Index AllocateNode() noexcept
        {
            if (!m_FreeList.empty())
            {
                const Index idx = m_FreeList.back();
                m_FreeList.pop_back();
                return idx;
            }
            m_Nodes.emplace_back();
            return static_cast<Index>(m_Nodes.size() - 1);
        }

        void InsertFront(Index idx) noexcept
        {
            Node& node = m_Nodes[idx];
            node.m_Prev = kNullIndex;
            node.m_Next = m_Head;
            if (m_Head != kNullIndex)
            {
                m_Nodes[m_Head].m_Prev = idx;
            }
            m_Head = idx;
            if (m_Tail == kNullIndex)
            {
                m_Tail = idx;
            }
        }

        void RemoveNode(Index idx) noexcept
        {
            Node& node = m_Nodes[idx];
            if (node.m_Prev != kNullIndex)
            {
                m_Nodes[node.m_Prev].m_Next = node.m_Next;
            }
            else
            {
                m_Head = node.m_Next;
            }
            if (node.m_Next != kNullIndex)
            {
                m_Nodes[node.m_Next].m_Prev = node.m_Prev;
            }
            else
            {
                m_Tail = node.m_Prev;
            }
            node.m_Prev = kNullIndex;
            node.m_Next = kNullIndex;
        }

        void MoveToFront(Index idx) noexcept
        {
            if (idx == m_Head)
            {
                return;
            }
            RemoveNode(idx);
            InsertFront(idx);
        }

        std::vector<Node> m_Nodes;
        std::vector<Index> m_FreeList;
        Index m_Head = kNullIndex;
        Index m_Tail = kNullIndex;
    };

    // @brief First-in-first-out: exactly the LRU list with accesses ignored,
    // so the victim is always the least-recently-INSERTED entry.
    //
    // The reference implemented FIFO as a lock-free Vyukov MPMC ring, which
    // cannot remove a mid-queue entry — every DeallocateObject left a stale
    // entry that only drained under eviction pressure, so deallocate-heavy
    // churn without pressure grew the queue without bound. The policies run
    // under GPUPagedCache's single-writer mutation contract, so the ring's
    // lock-freedom bought nothing; the intrusive list gives O(1) removal and
    // no stale entries at all.
    template<typename ObjectID>
    class FIFOPolicy : private LRUPolicy<ObjectID>
    {
        using Base = LRUPolicy<ObjectID>;

      public:
        using Handle = typename Base::Handle;
        using Base::Base;
        using Base::OnInsert;
        using Base::OnRemove;
        using Base::TrySelectVictim;

        void OnAccess(Handle&) noexcept
        {
            // FIFO ignores accesses — insertion order is eviction order.
        }
    };

    // @brief Second-chance ("clock") eviction: entries sit on a ring with a
    // referenced bit that OnAccess sets; the hand clears bits as it sweeps and
    // evicts the first entry found unreferenced. LRU-approximating at O(1)
    // bookkeeping per access — the classic page-cache compromise, named by the
    // issue alongside FIFO and LRU.
    template<typename ObjectID>
    class ClockPolicy
    {
      public:
        using Index = u32;
        static constexpr Index kNullIndex = std::numeric_limits<Index>::max();

        struct Handle
        {
            Index m_Index = kNullIndex;
        };

        explicit ClockPolicy(u32 capacity)
        {
            m_Entries.reserve(capacity);
            m_FreeList.reserve(capacity);
        }

        void OnAccess(Handle& handle) noexcept
        {
            OLO_CORE_ASSERT(handle.m_Index != kNullIndex && handle.m_Index < m_Entries.size() &&
                                m_Entries[handle.m_Index].m_Live,
                            "ClockPolicy::OnAccess on an invalid handle");
            m_Entries[handle.m_Index].m_Referenced = true;
        }

        [[nodiscard]] Handle OnInsert(const ObjectID& id) noexcept
        {
            Index idx;
            if (!m_FreeList.empty())
            {
                idx = m_FreeList.back();
                m_FreeList.pop_back();
            }
            else
            {
                m_Entries.emplace_back();
                idx = static_cast<Index>(m_Entries.size() - 1);
            }
            Entry& entry = m_Entries[idx];
            entry.m_Id = id;
            entry.m_Referenced = true; // a fresh insert gets its second chance
            entry.m_Live = true;
            ++m_LiveCount;
            return Handle{ idx };
        }

        void OnRemove(Handle& handle) noexcept
        {
            if (handle.m_Index == kNullIndex || handle.m_Index >= m_Entries.size() ||
                !m_Entries[handle.m_Index].m_Live)
            {
                return;
            }
            m_Entries[handle.m_Index].m_Live = false;
            m_FreeList.push_back(handle.m_Index);
            --m_LiveCount;
            handle.m_Index = kNullIndex;
        }

        [[nodiscard]] bool TrySelectVictim(const ObjectID& exclude, ObjectID& outVictim) noexcept
        {
            if (m_LiveCount == 0 || m_Entries.empty())
            {
                return false;
            }
            // Two full sweeps suffice: the first clears every referenced bit the
            // hand passes, so the second must find an unreferenced entry —
            // unless every live entry is the excluded object.
            const sizet entryCount = m_Entries.size();
            for (sizet step = 0; step < entryCount * 2; ++step)
            {
                Entry& entry = m_Entries[m_Hand];
                m_Hand = (m_Hand + 1) % entryCount;
                if (!entry.m_Live || entry.m_Id == exclude)
                {
                    continue;
                }
                if (entry.m_Referenced)
                {
                    entry.m_Referenced = false;
                    continue;
                }
                outVictim = entry.m_Id;
                return true;
            }
            return false;
        }

      private:
        struct Entry
        {
            ObjectID m_Id{};
            bool m_Referenced = false;
            bool m_Live = false;
        };

        std::vector<Entry> m_Entries;
        std::vector<Index> m_FreeList;
        sizet m_Hand = 0;
        sizet m_LiveCount = 0;
    };
} // namespace OloEngine
