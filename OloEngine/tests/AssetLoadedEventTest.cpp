#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Asset/AssetTypes.h"

#include <algorithm>
#include <filesystem>

using namespace OloEngine;

TEST(AssetLoadedEventTest, ExposesPayload)
{
    const AssetHandle handle = 1234567ULL;
    const AssetType type = AssetType::Texture2D;
    const std::filesystem::path path = "textures/foo.png";

    AssetLoadedEvent evt(handle, type, path);

    EXPECT_EQ(evt.GetHandle(), handle);
    EXPECT_EQ(evt.GetAssetType(), type);
    EXPECT_EQ(evt.GetPath(), path);
}

TEST(AssetLoadedEventTest, IdentifiesItselfAsAssetLoaded)
{
    AssetLoadedEvent evt(1ULL, AssetType::Scene, "scenes/main.olo");

    EXPECT_EQ(AssetLoadedEvent::GetStaticType(), EventType::AssetLoaded);
    EXPECT_EQ(evt.GetEventType(), EventType::AssetLoaded);
    EXPECT_STREQ(evt.GetName(), "AssetLoaded");
    EXPECT_TRUE(evt.IsInCategory(EventCategory::Application));
}

TEST(AssetLoadedEventTest, IsDistinctFromAssetReloaded)
{
    // The two events must not collide on the EventDispatcher.Dispatch<T> path —
    // listeners wired up for one should never fire for the other.
    EXPECT_NE(AssetLoadedEvent::GetStaticType(), AssetReloadedEvent::GetStaticType());
}

TEST(AssetLoadedEventTest, ToStringContainsPayload)
{
    const AssetHandle handle = 0xDEADBEEFULL;
    AssetLoadedEvent evt(handle, AssetType::Mesh, "meshes/character.olomesh");

    const std::string s = evt.ToString();

    // Normalize path separators: std::filesystem::path::string() returns the
    // native format, which on Windows can yield backslashes depending on how
    // the path was constructed. The handle/name assertions don't care, but
    // the path substring check would be brittle without normalization.
    std::string sNormalized = s;
    std::replace(sNormalized.begin(), sNormalized.end(), '\\', '/');

    EXPECT_NE(s.find("AssetLoadedEvent"), std::string::npos);
    EXPECT_NE(s.find(std::to_string(handle)), std::string::npos);
    EXPECT_NE(sNormalized.find("meshes/character.olomesh"), std::string::npos);
}

TEST(AssetLoadedEventTest, DispatcherInvokesMatchingHandler)
{
    AssetLoadedEvent evt(42ULL, AssetType::Material, "materials/m.olo");

    bool handlerRan = false;
    AssetHandle observedHandle = 0;

    EventDispatcher dispatcher(evt);
    const bool dispatched = dispatcher.Dispatch<AssetLoadedEvent>(
        [&](AssetLoadedEvent& e)
        {
            handlerRan = true;
            observedHandle = e.GetHandle();
            return true; // mark handled
        });

    EXPECT_TRUE(dispatched);
    EXPECT_TRUE(handlerRan);
    EXPECT_EQ(observedHandle, 42ULL);
    EXPECT_TRUE(evt.Handled);
}

TEST(AssetLoadedEventTest, DispatcherSkipsMismatchedHandler)
{
    AssetLoadedEvent evt(7ULL, AssetType::Texture2D, "t.png");

    bool reloadedHandlerRan = false;
    EventDispatcher dispatcher(evt);
    const bool dispatched = dispatcher.Dispatch<AssetReloadedEvent>(
        [&](AssetReloadedEvent&)
        {
            reloadedHandlerRan = true;
            return true;
        });

    EXPECT_FALSE(dispatched);
    EXPECT_FALSE(reloadedHandlerRan);
    EXPECT_FALSE(evt.Handled);
}
