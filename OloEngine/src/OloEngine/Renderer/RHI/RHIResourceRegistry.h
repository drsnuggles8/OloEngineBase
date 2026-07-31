#pragma once

// =============================================================================
// RHIResourceRegistry.h — the generation-checked producer of RHI::ResourceHandle.
//
// Issue #691 Phase 2 step 3, ADR 0011 §1.1 / §1.3 / §1.4.
//
// Phase 1 declared `RHI::ResourceHandle` and specified what it means; nothing
// minted one. This is the mint. Every backend-native object name — a GL texture
// name, a framebuffer, a buffer, a VAO, a program, a query — is registered here
// at creation and unregistered at destruction, and the engine above
// `Platform/<Backend>/` only ever sees the resulting handle.
//
// WHY A GENERATION, NOT JUST AN OPAQUE u32 (ADR 0011 §1.1): GL recycles object
// names. Today `Texture::operator==` compares `GetRendererID()`, so a deleted
// texture and a later-created unrelated one can compare EQUAL. The generation
// distinguishes them, and `TransientPool`'s alias reporting depends on exactly
// that distinction — "did these two plan entries get the same object, or a
// recycled name?".
//
// TWO ESCAPE HATCHES, BOTH NAMED TO BE CONSPICUOUS. Turning a handle back into
// a native name is spelled `ResolveNativeForBackend` (for `Platform/<Backend>/`)
// or `GetNativeHandleForDebug` (RHIResources.h, for `Renderer/Debug/` and the
// MCP capture endpoints). Neither reads as ordinary, which is the whole
// mechanism — `GetRendererID()` read as ordinary and is how GL reached 42 files.
// RHIBoundaryRatchetTest baselines both at zero outside their sanctioned homes.
//
// THREAD SAFETY. Reads (`ResolveNativeForBackend`, `IsLive`, `KindOf`) are
// lock-free and safe from any thread. Writes (`Register`, `Unregister`) take an
// internal mutex, so they are safe from any thread too. What the registry does
// NOT do is paper over a caller-level lifetime bug: destroying a resource on one
// thread while another is mid-bind with its handle is racy at the *caller*, and
// the registry's guarantee is only that such a race resolves to "stale" rather
// than to a torn native value.
// =============================================================================

#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string_view>
#include <vector>

namespace OloEngine::RHI
{
    // What kind of GPU object a handle names. Diagnostic rather than
    // load-bearing — the registry never dispatches on it — but it is what makes
    // a stale-handle report legible ("stale Texture handle", not "stale u32"),
    // and `olo_render_transient_plan` already groups by exactly these words.
    enum class ResourceKind : u8
    {
        Unknown = 0,
        Texture,
        Framebuffer,
        Buffer,
        VertexArray,
        ShaderProgram,
        Query,
    };

    [[nodiscard]] auto ToString(ResourceKind kind) -> std::string_view;

    class ResourceRegistry
    {
      public:
        // One registry per process. Deliberately leaked (never destroyed) so
        // that a `Ref<Texture>` released during static destruction — which does
        // happen, asset caches are file-scope statics in a couple of tests —
        // still finds a live registry to unregister from.
        [[nodiscard]] static auto Get() -> ResourceRegistry&;

        // Mint a handle for a freshly created native object. `nativeHandle` is
        // whatever the backend needs to get back to the object (a GLuint widened
        // to u64 today; a VkImage tomorrow). Returns an invalid handle if the
        // registry is full, which is a hard error the caller should log.
        [[nodiscard]] auto Register(ResourceKind kind, u64 nativeHandle, Backend owner) -> ResourceHandle;

        // Retire a handle. The slot's generation advances, so every outstanding
        // copy of `handle` becomes permanently stale — that is the point. A
        // double-unregister is counted and ignored rather than corrupting the
        // freelist.
        void Unregister(ResourceHandle handle);

        // Repoint a live handle at a newly created native object WITHOUT
        // changing its identity.
        //
        // This is what makes the handle better than the raw name it replaces,
        // not merely different. A texture hot-reload recreates the GL storage on
        // the SAME C++ object (issue #544 Part B, TextureInPlaceReloadTest), and
        // GL is free to hand the recreated storage a different name — which is
        // why that test's header tells consumers they "must read the RendererID
        // off the object each frame rather than caching it". A handle survives
        // the reload and resolves to the new name, so caching it is safe.
        //
        // Identity is the C++ resource object, not the native name. Two distinct
        // objects therefore always differ (different slot, or same slot with a
        // bumped generation), which is the comparison `Texture::operator==` got
        // wrong; one object across a reload stays equal to itself, which is what
        // materials holding `Ref<Texture2D>` already assume.
        //
        // No-op (and counted as a stale unregister) if the handle is already dead.
        void UpdateNative(ResourceHandle handle, u64 nativeHandle);

        // Structural validity (`handle.IsValid()`) says the handle is not the
        // null handle. This says the object it names is still alive.
        [[nodiscard]] auto IsLive(ResourceHandle handle) const -> bool;

        // The kind this handle was registered as, or ResourceKind::Unknown when
        // the handle is stale. GL names are per-object-type, so a texture and a
        // buffer can both be name 1 — the generation cannot tell those apart
        // because both handles are live. Backends use this to refuse a handle
        // handed to the wrong family.
        [[nodiscard]] auto GetKind(ResourceHandle handle) const -> ResourceKind;

        [[nodiscard]] auto KindOf(ResourceHandle handle) const -> ResourceKind;

        // ---------------------------------------------------------------------
        // ESCAPE HATCH — `Platform/<Backend>/` only.
        //
        // Returns 0 for a stale, null or foreign-backend handle, and counts the
        // rejection. Zero is the right failure value on both backends: a GL name
        // of 0 unbinds rather than binding garbage, so a stale handle degrades
        // to "nothing bound" instead of "whatever object inherited that name" —
        // which is precisely the failure `GetRendererID()` could not detect.
        // ---------------------------------------------------------------------
        [[nodiscard]] auto ResolveNativeForBackend(ResourceHandle handle) const -> u64;

        // As above, but reports the owning backend too. Backs
        // RHI::GetNativeHandleForDebug (RHIResources.h).
        [[nodiscard]] auto ResolveTaggedForBackend(ResourceHandle handle) const -> NativeHandle;

        struct Stats
        {
            u32 LiveCount = 0;        ///< handles currently registered
            u32 SlotCount = 0;        ///< slots ever allocated (live + free)
            u32 FreeCount = 0;        ///< retired slots awaiting reuse
            u64 TotalRegistered = 0;  ///< lifetime Register() count
            u64 StaleRejections = 0;  ///< resolves that hit a dead generation
            u64 StaleUnregisters = 0; ///< Unregister() of an already-dead handle
        };
        [[nodiscard]] auto GetStats() const -> Stats;

        // Test/diagnostic affordance. Not called by the engine.
        void ResetCounters();

        // Context loss / shutdown. Advances every live slot's generation, so
        // handles held across the reset are correctly reported stale rather than
        // silently resolving into a new device's objects.
        void Clear();

      private:
        ResourceRegistry() = default;

        // 1024 x 1024 = 1,048,576 concurrently-live GPU objects. Chunks are
        // allocated on demand and never freed, so a slot's address is stable for
        // the process lifetime — which is what lets readers run lock-free.
        static constexpr u32 kChunkSize = 1024u;
        static constexpr u32 kMaxChunks = 1024u;
        static constexpr u32 kMaxSlots = kChunkSize * kMaxChunks;

        struct Slot
        {
            // 0 means "never handed out". Register makes it odd-or-even without
            // meaning; all that matters is that it CHANGES on every transition,
            // so no handle from a previous tenant can match.
            std::atomic<u32> Generation{ 0u };
            std::atomic<u8> Kind{ static_cast<u8>(ResourceKind::Unknown) };
            std::atomic<u8> Owner{ static_cast<u8>(Backend::None) };
            std::atomic<u64> Native{ 0u };
        };

        [[nodiscard]] auto SlotAt(u32 index) const -> Slot*;
        // Caller must hold m_WriteMutex.
        [[nodiscard]] auto EnsureChunk(u32 chunkIndex) -> Slot*;

        mutable std::mutex m_WriteMutex;
        std::array<std::atomic<Slot*>, kMaxChunks> m_Chunks{};
        std::atomic<u32> m_SlotCount{ 0u };
        std::vector<u32> m_FreeList;
        u64 m_TotalRegistered = 0u;
        mutable std::atomic<u64> m_StaleRejections{ 0u };
        u64 m_StaleUnregisters = 0u;
    };

    // -------------------------------------------------------------------------
    // RAII ownership of one registry entry.
    //
    // Backend resource classes hold one of these instead of calling
    // Register/Unregister by hand. The reason is not tidiness: several of them
    // have more than one destruction path (an explicit Release(), a failed
    // construction, a resize that recreates storage), and a missed unregister
    // leaks a slot whose generation never advances — so a handle to a destroyed
    // object would keep resolving to a live native name, which is precisely the
    // bug this whole layer exists to make impossible.
    //
    // Move-only: a copied handle would unregister twice.
    // -------------------------------------------------------------------------
    class ScopedResourceHandle
    {
      public:
        ScopedResourceHandle() = default;

        ScopedResourceHandle(ResourceKind kind, u64 nativeHandle, Backend owner)
            : m_Handle(ResourceRegistry::Get().Register(kind, nativeHandle, owner))
        {
        }

        ~ScopedResourceHandle()
        {
            Reset();
        }

        ScopedResourceHandle(const ScopedResourceHandle&) = delete;
        auto operator=(const ScopedResourceHandle&) -> ScopedResourceHandle& = delete;

        ScopedResourceHandle(ScopedResourceHandle&& other) noexcept
            : m_Handle(other.m_Handle)
        {
            other.m_Handle = {};
        }

        auto operator=(ScopedResourceHandle&& other) noexcept -> ScopedResourceHandle&
        {
            if (this != &other)
            {
                Reset();
                m_Handle = other.m_Handle;
                other.m_Handle = {};
            }
            return *this;
        }

        // Retire whatever was held and mint a handle for a new native object.
        void Adopt(ResourceKind kind, u64 nativeHandle, Backend owner)
        {
            Reset();
            m_Handle = ResourceRegistry::Get().Register(kind, nativeHandle, owner);
        }

        // Same object, new storage behind it — see ResourceRegistry::UpdateNative.
        void Rebind(u64 nativeHandle)
        {
            ResourceRegistry::Get().UpdateNative(m_Handle, nativeHandle);
        }

        // The form backend classes call, right after assigning their native
        // object name. Says only "this object's native name is now X":
        //
        //   no identity yet, name != 0 -> registers, minting the identity
        //   already has one            -> repoints, PRESERVING the identity
        //   no identity yet, name == 0 -> nothing to name; stays null
        //
        // SYNC NEVER RETIRES, and that is a correction rather than a
        // simplification. It used to treat `nativeHandle == 0` as "released" and
        // Reset(), which looked reasonable and was wrong: a recreate path zeroes
        // the native name transiently between destroying the old object and
        // creating the new one (OpenGLTexture2D::InvalidateImpl does exactly
        // this), so an in-place hot-reload retired the identity and minted a
        // fresh one. Materials caching the handle alongside their Ref<Texture2D>
        // — the practice §1.2 sanctions — would have been left holding a dead
        // handle after every reload.
        //
        // `Sync` cannot tell a transient zero from a final one; only the caller
        // knows, and the two look identical at the call site. So the destructive
        // act is spelled separately: RAII retires at destruction, and `Reset()`
        // is there for a deliberate early release. Caught by
        // RHIHandleNativeIdentityTest's reload case, which is the whole reason
        // that test exists.
        //
        // Called at object-lifetime frequency, not per frame, so taking the
        // registry's write lock here is not a hot-path cost.
        void Sync(ResourceKind kind, u64 nativeHandle, Backend owner)
        {
            // IsValid() is a STRUCTURAL test on the handle's own bits — it says
            // nothing about whether the registry still holds the entry. After a
            // ResourceRegistry::Clear() every handle still reads as valid while
            // its slot is gone, so rebinding on IsValid() alone would write the
            // native id into a slot that may since have been handed to someone
            // else. Ask the registry instead, and re-adopt when the entry is no
            // longer ours.
            if (!ResourceRegistry::Get().IsLive(m_Handle))
            {
                m_Handle = {};
                if (nativeHandle != 0u)
                {
                    Adopt(kind, nativeHandle, owner);
                }
                return;
            }

            Rebind(nativeHandle);
        }

        void Reset()
        {
            if (m_Handle.IsValid())
            {
                ResourceRegistry::Get().Unregister(m_Handle);
                m_Handle = {};
            }
        }

        [[nodiscard]] auto Get() const -> ResourceHandle
        {
            return m_Handle;
        }

      private:
        ResourceHandle m_Handle;
    };
} // namespace OloEngine::RHI
