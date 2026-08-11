// Throwaway consumer for the vcpkg spike (issue #774). Proves the three
// deps configure, build, and LINK into a real consumer TU under the
// static-md CRT choice: glm (header-only), spdlog (static lib + fmt
// feature), and Jolt (overlay port, CROSS_PLATFORM_DETERMINISTIC=ON +
// rtti feature exercised via dynamic_cast below). Nothing here ships.

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/string_cast.hpp>
#include <spdlog/spdlog.h>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

int main()
{
    // --- glm ---
    glm::vec3 v(1.0f, 2.0f, 3.0f);
    spdlog::info("olo-vcpkg-spike: glm vec3 = {}", glm::to_string(v));

    // --- Jolt ---
    JPH::RegisterDefaultAllocator();

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    JPH::BoxShapeSettings boxShapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f));
    JPH::ShapeSettings::ShapeResult boxShapeResult = boxShapeSettings.Create();
    if (boxShapeResult.HasError())
    {
        spdlog::error("Jolt box shape creation failed: {}", boxShapeResult.GetError().c_str());
        return 1;
    }

    JPH::ShapeRefC boxShape = boxShapeResult.Get();
    JPH::BodyCreationSettings bodySettings(
        boxShape.GetPtr(),
        JPH::RVec3(0.0, 10.0, 0.0),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic,
        0 // object layer — unused outside a PhysicsSystem, any value is fine here
    );

    // Exercise RTTI (the `rtti` feature / CPP_RTTI_ENABLED) via dynamic_cast,
    // the same idiom the engine uses on Jolt shape types.
    const JPH::BoxShape* castShape = dynamic_cast<const JPH::BoxShape*>(boxShape.GetPtr());
    if (castShape == nullptr)
    {
        spdlog::error("dynamic_cast<const JPH::BoxShape*> failed — RTTI not enabled?");
        return 1;
    }

    const JPH::Vec3 halfExtent = castShape->GetHalfExtent();
    spdlog::info(
        "olo-vcpkg-spike: constructed Jolt BodyCreationSettings + BoxShape (half-extent {}), RTTI cast OK",
        glm::to_string(glm::vec3(halfExtent.GetX(), halfExtent.GetY(), halfExtent.GetZ())));

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;

    spdlog::info("olo-vcpkg-spike: PASS");
    return 0;
}
