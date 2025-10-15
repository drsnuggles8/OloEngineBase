#include "OloEnginePCH.h"
#include "GlobalWorkQueue.h"

namespace OloEngine
{
    GlobalWorkQueue::GlobalWorkQueue(u32 maxNodes)
        : m_MaxNodes(maxNodes)
    {
        OLO_PROFILE_FUNCTION();

        // Allocate node pool
        m_NodePool = new Node[m_MaxNodes];
        
        // Allocate dummy node (not from pool)
        m_DummyNode = new Node();
        m_DummyNode->TaskPtr = nullptr;
        m_DummyNode->Next.store(nullptr, std::memory_order_relaxed);
        
        // Build free list
        for (u32 i = 0; i < m_MaxNodes - 1; ++i)
        {
            m_NodePool[i].Next.store(&m_NodePool[i + 1], std::memory_order_relaxed);
        }
        m_NodePool[m_MaxNodes - 1].Next.store(nullptr, std::memory_order_relaxed);
        
        m_FreeList.store(&m_NodePool[0], std::memory_order_release);
        
        // Queue starts with dummy node (head and tail point to dummy)
        m_Head.store(m_DummyNode, std::memory_order_release);
        m_Tail.store(m_DummyNode, std::memory_order_release);
        m_ApproximateCount.store(0, std::memory_order_relaxed);
    }

    GlobalWorkQueue::~GlobalWorkQueue()
    {
        OLO_PROFILE_FUNCTION();

        // Pop all remaining tasks to release their references
        while (Pop() != nullptr)
        {
            // Just discard them
        }

        // Free dummy node
        delete m_DummyNode;
        m_DummyNode = nullptr;

        // Free node pool
        delete[] m_NodePool;
        m_NodePool = nullptr;
    }

    GlobalWorkQueue::Node* GlobalWorkQueue::AllocateNode(Task* task)
    {
        // Try to pop from free list
        Node* node = m_FreeList.load(std::memory_order_acquire);
        
        while (node != nullptr)
        {
            Node* next = node->Next.load(std::memory_order_relaxed);
            
            // Try to CAS the head of the free list
            if (m_FreeList.compare_exchange_weak(
                node, next,
                std::memory_order_release,
                std::memory_order_acquire))
            {
                // Successfully allocated - initialize and return
                node->TaskPtr = task;
                node->Next.store(nullptr, std::memory_order_relaxed);
                return node;
            }
            
            // CAS failed, node was updated by another thread, retry
        }
        
        // Free list is empty - this shouldn't happen in normal operation
        // Log error and return null
        OLO_CORE_ERROR("GlobalWorkQueue: Node pool exhausted!");
        return nullptr;
    }

    void GlobalWorkQueue::FreeNode(Node* node)
    {
        if (!node) return;
        
        // Clear task pointer
        node->TaskPtr = nullptr;
        
        // Push onto free list
        Node* oldHead = m_FreeList.load(std::memory_order_relaxed);
        
        do
        {
            node->Next.store(oldHead, std::memory_order_relaxed);
        }
        while (!m_FreeList.compare_exchange_weak(
            oldHead, node,
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    bool GlobalWorkQueue::Push(Ref<Task> task)
    {
        OLO_PROFILE_FUNCTION();

        if (!task)
        {
            OLO_CORE_WARN("GlobalWorkQueue::Push called with null task");
            return false;
        }

        // Allocate a node
        Node* node = AllocateNode(task.Raw());
        if (!node)
        {
            return false;  // Pool exhausted
        }

        // Increment reference count - queue now holds a reference
        task->IncRefCount();

        // Michael-Scott queue push algorithm (with dummy node)
        while (true)
        {
            Node* tail = m_Tail.load(std::memory_order_acquire);
            Node* next = tail->Next.load(std::memory_order_acquire);
            
            // Check if tail is still the last node
            if (next != nullptr)
            {
                // Tail is not the last node - help advance it
                m_Tail.compare_exchange_weak(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_acquire);
                continue;
            }

            // Try to link our node to the tail
            Node* expectedNext = nullptr;
            if (tail->Next.compare_exchange_strong(
                expectedNext, node,
                std::memory_order_release,
                std::memory_order_acquire))
            {
                // Successfully linked - try to advance tail (not critical if this fails)
                m_Tail.compare_exchange_weak(
                    tail, node,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                
                m_ApproximateCount.fetch_add(1, std::memory_order_relaxed);
                return true;
            }

            // Failed to link - another thread modified tail->Next, retry
        }
    }

    Ref<Task> GlobalWorkQueue::Pop()
    {
        OLO_PROFILE_FUNCTION();

        // Michael-Scott queue pop algorithm (with dummy node)
        while (true)
        {
            Node* head = m_Head.load(std::memory_order_acquire);
            Node* tail = m_Tail.load(std::memory_order_acquire);
            Node* next = head->Next.load(std::memory_order_acquire);

            // Check if head is consistent
            Node* currentHead = m_Head.load(std::memory_order_acquire);
            if (head != currentHead)
            {
                continue;  // Head changed, retry
            }

            // Check if queue is empty (head == tail means only dummy node present)
            if (head == tail)
            {
                // Queue is empty (only dummy node)
                if (next == nullptr)
                {
                    return nullptr;
                }
                
                // Tail is falling behind - help advance it
                m_Tail.compare_exchange_weak(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_acquire);
                
                continue;  // Retry
            }

            // Queue has items - next points to the actual task node
            // (head is dummy or a previous node, next is the node to dequeue)
            if (next == nullptr)
            {
                // Inconsistent state, retry
                continue;
            }

            // Try to swing head to next node (makes next the new dummy)
            if (m_Head.compare_exchange_strong(
                head, next,
                std::memory_order_release,
                std::memory_order_acquire))
            {
                // Successfully dequeued - extract task from next (not head, which is the old dummy)
                Task* taskPtr = next->TaskPtr;
                
                // Clear next's task pointer since it's now the new dummy node
                next->TaskPtr = nullptr;
                
                // Free the old dummy node (head)
                FreeNode(head);
                
                m_ApproximateCount.fetch_sub(1, std::memory_order_relaxed);

                // Wrap in Ref and balance reference count
                // The queue incremented during Push, Ref constructor will increment,
                // so we decrement to compensate
                Ref<Task> result(taskPtr);
                if (taskPtr) taskPtr->DecRefCount();
                
                return result;
            }

            // CAS failed, another thread popped, retry
        }
    }

    bool GlobalWorkQueue::IsEmpty() const
    {
        Node* head = m_Head.load(std::memory_order_relaxed);
        Node* tail = m_Tail.load(std::memory_order_relaxed);
        
        // Empty if head == tail (only dummy node present)
        return head == tail;
    }

    u32 GlobalWorkQueue::ApproximateSize() const
    {
        return m_ApproximateCount.load(std::memory_order_relaxed);
    }

} // namespace OloEngine
