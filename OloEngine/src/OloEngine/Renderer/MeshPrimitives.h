#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Mesh.h"

namespace OloEngine
{

    // @brief Mesh primitives utility class for common geometric shapes
    //
    // This class provides factory methods for creating commonly used mesh primitives
    // without cluttering the core Mesh classes. These are essentially
    // convenience generators that create standard geometric shapes with proper
    // vertex data, normals, and texture coordinates.
    class MeshPrimitives
    {
      public:
        // Delete constructors and operators to prevent instantiation
        MeshPrimitives() = delete;
        MeshPrimitives(const MeshPrimitives&) = delete;
        MeshPrimitives& operator=(const MeshPrimitives&) = delete;
        MeshPrimitives(MeshPrimitives&&) = delete;
        MeshPrimitives& operator=(MeshPrimitives&&) = delete;

        // =============================================================================
        // BASIC GEOMETRIC PRIMITIVES
        // =============================================================================

        // @brief Create a unit cube mesh
        // @return Mesh with vertices from -0.5 to 0.5 on all axes
        [[nodiscard]] static Ref<Mesh> CreateCube();

        // @brief Create a sphere mesh
        // @param radius Sphere radius
        // @param segments Number of horizontal and vertical segments
        // @return Sphere mesh with proper UV mapping
        [[nodiscard]] static Ref<Mesh> CreateSphere(f32 radius = 1.0f, u32 segments = 16);

        // @brief Create a plane mesh
        // @param width Plane width (X-axis)
        // @param length Plane length (Z-axis)
        // @return Plane mesh facing up (positive Y normal)
        [[nodiscard]] static Ref<Mesh> CreatePlane(f32 width = 1.0f, f32 length = 1.0f);

        // @brief Create a cylinder mesh
        // @param radius Cylinder radius (must be > 0.0f)
        // @param height Cylinder height (must be > 0.0f)
        // @param segments Number of circular segments (must be >= 3, recommended 8-32 for good quality)
        // @return Cylinder mesh aligned with Y-axis
        // @throws std::invalid_argument if parameters are out of valid range
        //
        // Valid parameter ranges:
        // - radius: (0.0f, +inf) - Must be positive
        // - height: (0.0f, +inf) - Must be positive
        // - segments: [3, UINT32_MAX] - Minimum 3 for valid geometry, recommended 8-32
        [[nodiscard]] static Ref<Mesh> CreateCylinder(f32 radius = 1.0f, f32 height = 2.0f, u32 segments = 16);

        // @brief Create a cone mesh
        // @param radius Base radius (must be > 0.0f)
        // @param height Cone height (must be > 0.0f)
        // @param segments Number of circular segments (must be >= 3, recommended 8-32 for good quality)
        // @return Cone mesh aligned with Y-axis, tip at top
        // @throws std::invalid_argument if parameters are out of valid range
        //
        // Valid parameter ranges:
        // - radius: (0.0f, +inf) - Must be positive
        // - height: (0.0f, +inf) - Must be positive
        // - segments: [3, UINT32_MAX] - Minimum 3 for valid geometry, recommended 8-32
        [[nodiscard]] static Ref<Mesh> CreateCone(f32 radius = 1.0f, f32 height = 2.0f, u32 segments = 16);

        // =============================================================================
        // SPECIALIZED PRIMITIVES
        // =============================================================================

        // @brief Create a skybox cube mesh
        // @return Cube mesh optimized for skybox rendering (inward-facing normals)
        [[nodiscard]] static Ref<Mesh> CreateSkyboxCube();

        // @brief Create a full-screen quad mesh
        // @return Quad mesh for post-processing effects (-1 to 1 on X and Y)
        [[nodiscard]] static Ref<Mesh> CreateFullscreenQuad();

        // @brief Create an icosphere mesh
        // @param radius Sphere radius (must be > 0.0f)
        // @param subdivisions Number of subdivision levels (must be 0-6, higher = smoother but exponentially more triangles)
        // @return Icosphere mesh with more uniform triangle distribution than UV sphere
        // @throws std::invalid_argument if parameters are out of valid range
        //
        // Valid parameter ranges:
        // - radius: (0.0f, +inf) - Must be positive
        // - subdivisions: [0, 6] - Limited range to prevent exponential triangle growth
        //
        // Triangle count by subdivision level:
        // - Level 0: 20 triangles (icosahedron)
        // - Level 1: 80 triangles
        // - Level 2: 320 triangles (default)
        // - Level 3: 1,280 triangles
        // - Level 4: 5,120 triangles
        // - Level 5: 20,480 triangles
        // - Level 6: 81,920 triangles (maximum allowed)
        [[nodiscard]] static Ref<Mesh> CreateIcosphere(f32 radius = 1.0f, u32 subdivisions = 2);

        // @brief Create a torus mesh
        // @param majorRadius Distance from center to tube center (must be > 0.0f)
        // @param minorRadius Tube radius (must be > 0.0f and < majorRadius)
        // @param majorSegments Segments around the major radius (must be >= 3, recommended 16-48)
        // @param minorSegments Segments around the tube (must be >= 3, recommended 8-24)
        // @return Torus mesh
        // @throws std::invalid_argument if parameters are out of valid range
        //
        // Valid parameter ranges:
        // - majorRadius: (0.0f, +inf) - Must be positive
        // - minorRadius: (0.0f, majorRadius) - Must be positive and smaller than major radius
        // - majorSegments: [3, UINT32_MAX] - Minimum 3 for valid geometry, recommended 16-48
        // - minorSegments: [3, UINT32_MAX] - Minimum 3 for valid geometry, recommended 8-24
        //
        // Geometric constraints:
        // - minorRadius < majorRadius (prevents self-intersecting torus)
        // - Higher segment counts produce smoother curves but increase triangle count
        // - Triangle count ≈ 2 × majorSegments × minorSegments
        [[nodiscard]] static Ref<Mesh> CreateTorus(f32 majorRadius = 1.0f, f32 minorRadius = 0.3f, u32 majorSegments = 24, u32 minorSegments = 12);

        // =============================================================================
        // UTILITY AND DEBUGGING PRIMITIVES
        // =============================================================================

        // @brief Create a grid mesh
        // @param size Grid size (total width/height, must be > 0.0f)
        // @param divisions Number of grid divisions (must be > 0, recommended 1-100)
        // @return Grid mesh for debugging and alignment
        // @throws std::invalid_argument if parameters are out of valid range
        //
        // Valid parameter ranges:
        // - size: (0.0f, +inf) - Must be positive for valid grid dimensions
        // - divisions: [1, UINT32_MAX] - Must be positive, recommended 1-100 for performance
        //
        // Performance considerations:
        // - Vertex count = (divisions + 1)²
        // - Line count = 2 × divisions × (divisions + 1)
        // - High division counts can impact performance for debugging grids
        [[nodiscard]] static Ref<Mesh> CreateGrid(f32 size = 10.0f, u32 divisions = 10);

        // @brief Create a wireframe cube mesh
        // @return Cube mesh suitable for wireframe rendering
        [[nodiscard]] static Ref<Mesh> CreateWireframeCube();

        // @brief Create coordinate axes mesh
        // @param length Length of each axis
        // @return Mesh with X, Y, Z axes (red, green, blue)
        [[nodiscard]] static Ref<Mesh> CreateCoordinateAxes(f32 length = 1.0f);

        // =============================================================================
        // ANIMATED MESH PRIMITIVES (TODO: Update for new MeshSource system)
        // =============================================================================

        // TODO: Update these methods to work with new MeshSource bone influence system
        // static Ref<Mesh> CreateAnimatedCube();
        // static Ref<Mesh> CreateMultiBoneAnimatedCube();
    };

} // namespace OloEngine
