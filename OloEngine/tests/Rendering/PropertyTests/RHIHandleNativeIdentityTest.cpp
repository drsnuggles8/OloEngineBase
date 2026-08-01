// =============================================================================
// RHIHandleNativeIdentityTest.cpp
//
// Issue #691 Phase 2 step 3. `RHIResourceRegistryTest` proves the registry's
// *bookkeeping* against synthetic values — it never touches a device, because
// the registry stores an opaque u64 and does not know what one is.
//
// THIS test is the other half: that the handle a real backend resource hands out
// actually names that resource's live GL object. Without it, every registry
// assertion could hold while `m_RHIHandle.Sync()` was missing from a creation
// path and the handle named nothing at all — which is exactly the shape of the
// bug an abandoned scripted sweep hit (see ADR 0011 amendment (16)).
//
// The reload case is the one that could not be tested before this file existed.
// `ResourceRegistry::UpdateNative` is what makes a handle survive an in-place
// hot-reload, and its whole justification is that GL may hand recreated storage
// a *different* name (issue #544 Part B; TextureInPlaceReloadTest's header
// states exactly this and concludes that consumers must therefore re-read the
// renderer ID every frame). That conclusion is what the handle is supposed to
// overturn, so it has to be checked against a driver rather than a mock.
//
// GL-gated: SKIPs cleanly when no GL 4.5+ context exists, same as its siblings.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <stb_image/stb_image_write.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

// OLO_TEST_LAYER: L3

namespace OloEngine::Tests
{
    namespace
    {
        bool WriteSolidPng(const std::filesystem::path& path, int w, int h, u8 r, u8 g, u8 b, u8 a)
        {
            std::vector<u8> pixels(static_cast<sizet>(w) * static_cast<sizet>(h) * 4u);
            for (sizet i = 0; i < pixels.size(); i += 4)
            {
                pixels[i + 0] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
                pixels[i + 3] = a;
            }
            return ::stbi_write_png(path.string().c_str(), w, h, 4, pixels.data(), w * 4) != 0;
        }

        [[nodiscard]] u64 NativeOf(RHI::ResourceHandle handle)
        {
            return RHI::GetNativeHandleForDebug(handle).Value;
        }
    } // namespace

    // A texture's handle must resolve to the GL name the texture is actually
    // using, and that name must be a live GL texture object.
    TEST(RHIHandleNativeIdentity, TextureHandleResolvesToItsLiveGLName)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TextureSpecification spec;
        spec.Width = 4;
        spec.Height = 4;
        spec.Format = ImageFormat::RGBA8;
        const auto texture = Texture2D::Create(spec);
        ASSERT_TRUE(texture);

        const auto handle = texture->GetRHIHandle();
        ASSERT_TRUE(handle.IsValid()) << "Texture2D minted no identity — a Sync() is missing from its "
                                         "creation path, which no registry-only test can detect";
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(handle));
        EXPECT_EQ(RHI::ResourceRegistry::Get().KindOf(handle), RHI::ResourceKind::Texture);

        // The handle names the same object the legacy accessor does. This
        // equality is what the migration rests on: while both currencies exist,
        // a caller may hold either and they must agree.
        EXPECT_EQ(NativeOf(handle), static_cast<u64>(texture->GetRendererID()));

        // ...and that name is a real, live GL texture.
        EXPECT_TRUE(glIsTexture(static_cast<GLuint>(NativeOf(handle))) == GL_TRUE);
    }

    // Two distinct textures never share an identity. The bare renderer ID could
    // not promise this across a delete/create pair, which is the defect
    // ADR 0011 §1.1 names in Texture::operator==.
    TEST(RHIHandleNativeIdentity, DistinctTexturesHaveDistinctHandles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TextureSpecification spec;
        spec.Width = 2;
        spec.Height = 2;
        spec.Format = ImageFormat::RGBA8;

        const auto a = Texture2D::Create(spec);
        const auto b = Texture2D::Create(spec);
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);

        EXPECT_NE(a->GetRHIHandle(), b->GetRHIHandle());
        EXPECT_NE(NativeOf(a->GetRHIHandle()), NativeOf(b->GetRHIHandle()));
    }

    // Buffers mint too — the sweep converts these alongside textures, and a
    // missing Sync() on a buffer path would be just as invisible.
    TEST(RHIHandleNativeIdentity, BufferHandlesResolveToLiveGLNames)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto storage = StorageBuffer::Create(256u, 0u);
        ASSERT_TRUE(storage);
        const auto storageHandle = storage->GetRHIHandle();
        ASSERT_TRUE(storageHandle.IsValid());
        EXPECT_EQ(RHI::ResourceRegistry::Get().KindOf(storageHandle), RHI::ResourceKind::Buffer);
        EXPECT_EQ(NativeOf(storageHandle), static_cast<u64>(storage->GetRendererID()));
        EXPECT_TRUE(glIsBuffer(static_cast<GLuint>(NativeOf(storageHandle))) == GL_TRUE);

        const auto uniform = UniformBuffer::Create(256u, 0u);
        ASSERT_TRUE(uniform);
        const auto uniformHandle = uniform->GetRHIHandle();
        ASSERT_TRUE(uniformHandle.IsValid());
        EXPECT_NE(storageHandle, uniformHandle);
        EXPECT_EQ(NativeOf(uniformHandle), static_cast<u64>(uniform->GetRendererID()));
    }

    // ==========================================================================
    // The one this file exists for.
    //
    // An in-place reload recreates the GL storage on the SAME C++ object. GL may
    // reuse the freed name or hand out a new one — TextureInPlaceReloadTest
    // deliberately does not assert which, and that uncertainty is precisely why
    // it tells consumers not to cache the renderer ID.
    //
    // The handle must survive regardless and follow the object to whatever name
    // the driver chose. Nothing checked that against a real driver until now:
    // `UpdateNative` was covered only by a unit test feeding it synthetic
    // integers.
    // ==========================================================================
    TEST(RHIHandleNativeIdentity, HandleSurvivesInPlaceReloadAndTracksTheNewGLName)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto path = std::filesystem::temp_directory_path() / "olo_rhi_handle_reload_691.png";
        ASSERT_TRUE(WriteSolidPng(path, 2, 2, 255, 0, 0, 255));

        // Non-const: Reload() mutates the object in place, which is the whole
        // point — the C++ object survives and only its GL storage is recreated.
        auto texture = Texture2D::Create(path.string(), /*srgb=*/false);
        ASSERT_TRUE(texture);
        ASSERT_TRUE(texture->IsLoaded());

        const auto handleBefore = texture->GetRHIHandle();
        ASSERT_TRUE(handleBefore.IsValid());
        const u64 nativeBefore = NativeOf(handleBefore);
        ASSERT_NE(nativeBefore, 0u);

        // Replace the file and reload in place.
        ASSERT_TRUE(WriteSolidPng(path, 4, 4, 0, 0, 255, 255));
        ASSERT_TRUE(texture->Reload());
        ASSERT_TRUE(texture->IsLoaded());

        const auto handleAfter = texture->GetRHIHandle();

        // (1) IDENTITY IS PRESERVED. This is the property that makes caching a
        //     handle safe where caching a renderer ID was not.
        EXPECT_EQ(handleBefore, handleAfter)
            << "An in-place reload must not change the object's identity — materials hold "
               "Ref<Texture2D> and cache the handle alongside it (ADR 0011 §1.2).";
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(handleBefore));

        // (2) THE HANDLE FOLLOWED THE STORAGE. Whether GL recycled the name or
        //     issued a new one, resolving the ORIGINAL handle must yield whatever
        //     the object is using now.
        const u64 nativeAfter = NativeOf(handleAfter);
        EXPECT_NE(nativeAfter, 0u);
        EXPECT_EQ(nativeAfter, static_cast<u64>(texture->GetRendererID()));
        EXPECT_EQ(NativeOf(handleBefore), nativeAfter)
            << "The pre-reload handle must resolve to the post-reload GL name. If this fails, "
               "UpdateNative was not called on the reload path and a cached handle now names "
               "a deleted object.";
        EXPECT_TRUE(glIsTexture(static_cast<GLuint>(nativeAfter)) == GL_TRUE);

        // Recording which case the driver took makes a future failure legible:
        // the assertions above hold either way, but knowing whether the name was
        // recycled tells you whether this run exercised the interesting path.
        if (nativeAfter == nativeBefore)
        {
            GTEST_LOG_(INFO) << "Driver recycled the GL name across the reload (" << nativeBefore
                             << ") — the identity-preservation half was exercised.";
        }
        else
        {
            GTEST_LOG_(INFO) << "Driver issued a NEW GL name across the reload (" << nativeBefore
                             << " -> " << nativeAfter << ") — the UpdateNative half was exercised, "
                             << "and a cached renderer ID would have been stale here.";
        }

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // ==========================================================================
    // Framebuffer ATTACHMENT identity — a migration root, because
    // RenderGraph::ResolveTexture returns attachment ids for its
    // framebuffer-view resources and cannot hand out handles until these do.
    //
    // The resize case is the interesting one, and its semantics are the OPPOSITE
    // of a texture hot-reload: a reload preserves identity (the C++ object
    // survives, only its storage is recreated), whereas a resize genuinely
    // destroys the attachment textures. Anything holding an old attachment
    // handle must therefore see it go STALE rather than silently follow the
    // resize onto a different texture.
    // ==========================================================================
    TEST(RHIHandleNativeIdentity, FramebufferAttachmentsHaveTheirOwnLiveIdentities)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FramebufferSpecification spec;
        spec.Width = 32;
        spec.Height = 32;
        spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
        const auto fb = Framebuffer::Create(spec);
        ASSERT_TRUE(fb);

        const auto colour = fb->GetColorAttachmentHandle(0);
        const auto depth = fb->GetDepthAttachmentHandle();
        ASSERT_TRUE(colour.IsValid());
        ASSERT_TRUE(depth.IsValid());

        // Each attachment names its own GL texture, not the framebuffer.
        EXPECT_EQ(NativeOf(colour), static_cast<u64>(fb->GetColorAttachmentRendererID(0)));
        EXPECT_EQ(NativeOf(depth), static_cast<u64>(fb->GetDepthAttachmentRendererID()));
        EXPECT_TRUE(glIsTexture(static_cast<GLuint>(NativeOf(colour))) == GL_TRUE);
        EXPECT_TRUE(glIsTexture(static_cast<GLuint>(NativeOf(depth))) == GL_TRUE);

        // ...and none of the three identities collide. "The framebuffer's
        // handle" is the wrong answer to "which texture is this?".
        const auto self = fb->GetRHIHandle();
        EXPECT_NE(colour, depth);
        EXPECT_NE(colour, self);
        EXPECT_NE(depth, self);
        EXPECT_EQ(RHI::ResourceRegistry::Get().KindOf(self), RHI::ResourceKind::Framebuffer);
        EXPECT_EQ(RHI::ResourceRegistry::Get().KindOf(colour), RHI::ResourceKind::Texture);
    }

    TEST(RHIHandleNativeIdentity, FramebufferResizeRetiresTheOldAttachmentIdentities)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FramebufferSpecification spec;
        spec.Width = 16;
        spec.Height = 16;
        spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
        // Non-const: Resize() mutates, recreating the attachments — the very
        // thing under test.
        auto fb = Framebuffer::Create(spec);
        ASSERT_TRUE(fb);

        const auto colourBefore = fb->GetColorAttachmentHandle(0);
        const auto depthBefore = fb->GetDepthAttachmentHandle();
        ASSERT_TRUE(colourBefore.IsValid());

        fb->Resize(48u, 48u);

        const auto colourAfter = fb->GetColorAttachmentHandle(0);
        ASSERT_TRUE(colourAfter.IsValid());

        // NEW objects, so NEW identities — the inverse of the hot-reload rule.
        EXPECT_NE(colourBefore, colourAfter)
            << "A resize destroys and recreates the attachment textures, so their identities must "
               "not carry over. Preserving them here would be the reload rule misapplied: a holder "
               "of the old handle would silently follow onto a different texture.";
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(colourBefore));
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(depthBefore));
        EXPECT_EQ(NativeOf(colourBefore), 0u);

        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(colourAfter));
        EXPECT_EQ(NativeOf(colourAfter), static_cast<u64>(fb->GetColorAttachmentRendererID(0)));
    }

    // ==========================================================================
    // The facade's handle overloads (slice 2) have no callers yet — the call
    // sites migrate in later slices. Untested dead code is how a "purely
    // mechanical" delegation ships with the arguments transposed, so exercise
    // them here against real GL state rather than waiting for a caller.
    // ==========================================================================
    TEST(RHIHandleNativeIdentity, FacadeHandleOverloadBindsTheSameObjectAsTheLegacyForm)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TextureSpecification spec;
        spec.Width = 2;
        spec.Height = 2;
        spec.Format = ImageFormat::RGBA8;
        const auto texture = Texture2D::Create(spec);
        ASSERT_TRUE(texture);

        constexpr u32 kSlot = 6u;
        const auto expected = static_cast<GLint>(texture->GetRendererID());

        // Bind through the handle overload...
        RenderCommand::BindTexture(kSlot, texture->GetRHIHandle());
        glActiveTexture(GL_TEXTURE0 + kSlot);
        GLint boundViaHandle = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundViaHandle);
        EXPECT_EQ(boundViaHandle, expected)
            << "The handle overload must reach GL with the same object the u32 form does — "
               "it resolves and delegates, so a transposed argument would land here.";

        // ...and confirm the legacy form still agrees, since both must coexist
        // until the final slice deletes the u32 spelling.
        RenderCommand::BindTexture(kSlot, 0u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundViaHandle);
        ASSERT_EQ(boundViaHandle, 0) << "precondition: slot cleared";

        RenderCommand::BindTexture(kSlot, texture->GetRendererID());
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundViaHandle);
        EXPECT_EQ(boundViaHandle, expected);

        RenderCommand::BindTexture(kSlot, 0u);
        glActiveTexture(GL_TEXTURE0);
    }

    // A stale handle must unbind rather than bind whatever inherited its name.
    // This is the degradation the whole layer promises, and it is only
    // observable through the facade — the registry alone cannot show it.
    TEST(RHIHandleNativeIdentity, FacadeHandleOverloadUnbindsForAStaleHandle)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TextureSpecification spec;
        spec.Width = 2;
        spec.Height = 2;
        spec.Format = ImageFormat::RGBA8;

        RHI::ResourceHandle dead;
        {
            const auto doomed = Texture2D::Create(spec);
            ASSERT_TRUE(doomed);
            dead = doomed->GetRHIHandle();
        }
        ASSERT_FALSE(RHI::ResourceRegistry::Get().IsLive(dead));

        constexpr u32 kSlot = 7u;
        const auto live = Texture2D::Create(spec);
        ASSERT_TRUE(live);

        glActiveTexture(GL_TEXTURE0 + kSlot);
        RenderCommand::BindTexture(kSlot, live->GetRHIHandle());
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        ASSERT_EQ(bound, static_cast<GLint>(live->GetRendererID())) << "precondition";

        RenderCommand::BindTexture(kSlot, dead);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        EXPECT_EQ(bound, 0) << "A stale handle must resolve to 0 and unbind. Binding anything else "
                               "would mean a use-after-free samples whatever object inherited the "
                               "recycled GL name — the failure a bare u32 could not detect.";

        glActiveTexture(GL_TEXTURE0);
    }

    // ==========================================================================
    // Raw facade creators (slice 4) — the last migration root, and like slice
    // 2's overloads they have no callers yet. The Delete* half is what needs
    // proving: destroying the object and RETIRING the identity are two separate
    // acts, and a sibling that does only the first leaves a handle resolving to
    // a name the driver may reissue. That is the exact omission the abandoned
    // scripted sweep made.
    // ==========================================================================
    TEST(RHIHandleNativeIdentity, RawCreatorMintsALiveIdentityAndDeleteRetiresIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto texture = RenderCommand::CreateTexture2DHandle(8u, 8u, RHI::Format::RGBA8UNorm);
        ASSERT_TRUE(texture.IsValid());
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(texture));
        EXPECT_EQ(RHI::ResourceRegistry::Get().KindOf(texture), RHI::ResourceKind::Texture);

        const u64 native = NativeOf(texture);
        ASSERT_NE(native, 0u);
        EXPECT_TRUE(glIsTexture(static_cast<GLuint>(native)) == GL_TRUE);

        RenderCommand::DeleteTexture(texture);

        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(texture))
            << "Delete must retire the identity as well as the object. Without the Unregister, the "
               "slot keeps its generation and this handle goes on resolving to a GL name the driver "
               "is free to hand to something else.";
        EXPECT_EQ(NativeOf(texture), 0u);
    }

    // A delete handed a handle of the WRONG family must be a complete no-op.
    //
    // Regression test for a defect introduced by the kind-checking change
    // itself: ResolveNativeAs correctly refused to hand back the wrong family's
    // GL name, but the Unregister that follows is not kind-aware, so the delete
    // retired the other resource's registry entry while leaving its GL object
    // alive. That is worse than the unchecked form it replaced — the two halves
    // disagreed. Both halves must skip together.
    TEST(RHIHandleNativeIdentity, DeletingWithAWrongKindHandleTouchesNeitherTheObjectNorTheRegistry)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto buffer = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(buffer.IsValid());

        auto& registry = RHI::ResourceRegistry::Get();
        ASSERT_EQ(registry.KindOf(buffer), RHI::ResourceKind::Buffer);
        const u32 nativeBefore = NativeOf(buffer);
        ASSERT_NE(nativeBefore, 0u);

        // Hand a buffer to the TEXTURE delete. GL names are per-type, so this
        // buffer's name is very likely also a live texture's name — the exact
        // confusion the kind check exists to stop.
        RenderCommand::DeleteTexture(buffer);

        EXPECT_TRUE(registry.IsLive(buffer))
            << "The buffer's registry entry was retired by a texture delete. A wrong-kind handle "
               "names someone else's resource; refusing to resolve it is only half the job.";
        EXPECT_EQ(registry.KindOf(buffer), RHI::ResourceKind::Buffer);
        EXPECT_EQ(NativeOf(buffer), nativeBefore)
            << "The buffer's native name changed, so the delete did not leave it alone.";

        RenderCommand::DeleteBuffer(buffer);
        EXPECT_FALSE(registry.IsLive(buffer)) << "The correctly-typed delete must still work.";
    }

    TEST(RHIHandleNativeIdentity, RawBufferAndVertexArrayCreatorsMintDistinctKinds)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto buffer = RenderCommand::CreateBufferHandle();
        const auto vertexArray = RenderCommand::CreateVertexArrayHandle();
        const auto framebuffer = RenderCommand::CreateFramebufferHandle();
        ASSERT_TRUE(buffer.IsValid());
        ASSERT_TRUE(vertexArray.IsValid());
        ASSERT_TRUE(framebuffer.IsValid());

        auto& registry = RHI::ResourceRegistry::Get();
        EXPECT_EQ(registry.KindOf(buffer), RHI::ResourceKind::Buffer);
        EXPECT_EQ(registry.KindOf(vertexArray), RHI::ResourceKind::VertexArray);
        EXPECT_EQ(registry.KindOf(framebuffer), RHI::ResourceKind::Framebuffer);

        // Three different object types created back to back: GL may well hand
        // them the same numeric name, since names are per-type. The identities
        // must still differ — which a bare renderer ID could not promise.
        EXPECT_NE(buffer, vertexArray);
        EXPECT_NE(buffer, framebuffer);
        EXPECT_NE(vertexArray, framebuffer);

        EXPECT_TRUE(glIsBuffer(static_cast<GLuint>(NativeOf(buffer))) == GL_TRUE);
        EXPECT_TRUE(glIsVertexArray(static_cast<GLuint>(NativeOf(vertexArray))) == GL_TRUE);

        RenderCommand::DeleteBuffer(buffer);
        RenderCommand::DeleteVertexArray(vertexArray);
        RenderCommand::DeleteFramebuffer(framebuffer);
        EXPECT_FALSE(registry.IsLive(buffer));
        EXPECT_FALSE(registry.IsLive(vertexArray));
        EXPECT_FALSE(registry.IsLive(framebuffer));
    }

    // Destroying a resource must retire its identity, so a handle held across
    // the destruction reports "gone" rather than resolving into whatever object
    // inherits the recycled GL name.
    TEST(RHIHandleNativeIdentity, HandleGoesStaleWhenTheResourceIsDestroyed)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RHI::ResourceHandle handle;
        {
            TextureSpecification spec;
            spec.Width = 2;
            spec.Height = 2;
            spec.Format = ImageFormat::RGBA8;
            const auto texture = Texture2D::Create(spec);
            ASSERT_TRUE(texture);
            handle = texture->GetRHIHandle();
            ASSERT_TRUE(RHI::ResourceRegistry::Get().IsLive(handle));
        }

        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(handle));
        EXPECT_EQ(NativeOf(handle), 0u)
            << "A handle outliving its resource must resolve to nothing. Resolving to a "
               "non-zero name would mean the registry entry was never retired — the "
               "recycled-name failure this layer exists to prevent.";
    }
} // namespace OloEngine::Tests
