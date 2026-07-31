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
#include "OloEngine/Renderer/RHI/RHIResources.h"
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
