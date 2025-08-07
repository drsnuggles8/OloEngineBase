#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/SkinnedMesh.h"

namespace OloEngine {

	/**
	 * @brief Mesh primitives utility class for common geometric shapes
	 * 
	 * This class provides factory methods for creating commonly used mesh primitives
	 * without cluttering the core Mesh and SkinnedMesh classes. These are essentially
	 * convenience generators that create standard geometric shapes with proper
	 * vertex data, normals, and texture coordinates.
	 */
	class MeshPrimitives
	{
	public:
		// =============================================================================
		// BASIC GEOMETRIC PRIMITIVES
		// =============================================================================
		
		/**
		 * @brief Create a unit cube mesh
		 * @return Mesh with vertices from -0.5 to 0.5 on all axes
		 */
		static Ref<Mesh> CreateCube();
		
		/**
		 * @brief Create a sphere mesh
		 * @param radius Sphere radius
		 * @param segments Number of horizontal and vertical segments
		 * @return Sphere mesh with proper UV mapping
		 */
		static Ref<Mesh> CreateSphere(f32 radius = 1.0f, u32 segments = 16);
		
		/**
		 * @brief Create a plane mesh
		 * @param width Plane width (X-axis)
		 * @param length Plane length (Z-axis)
		 * @return Plane mesh facing up (positive Y normal)
		 */
		static Ref<Mesh> CreatePlane(f32 width = 1.0f, f32 length = 1.0f);
		
		/**
		 * @brief Create a cylinder mesh
		 * @param radius Cylinder radius
		 * @param height Cylinder height
		 * @param segments Number of circular segments
		 * @return Cylinder mesh aligned with Y-axis
		 */
		static Ref<Mesh> CreateCylinder(f32 radius = 1.0f, f32 height = 2.0f, u32 segments = 16);
		
		/**
		 * @brief Create a cone mesh
		 * @param radius Base radius
		 * @param height Cone height
		 * @param segments Number of circular segments
		 * @return Cone mesh aligned with Y-axis, tip at top
		 */
		static Ref<Mesh> CreateCone(f32 radius = 1.0f, f32 height = 2.0f, u32 segments = 16);
		
		// =============================================================================
		// SPECIALIZED PRIMITIVES
		// =============================================================================
		
		/**
		 * @brief Create a skybox cube mesh
		 * @return Cube mesh optimized for skybox rendering (inward-facing normals)
		 */
		static Ref<Mesh> CreateSkyboxCube();
		
		/**
		 * @brief Create a full-screen quad mesh
		 * @return Quad mesh for post-processing effects (-1 to 1 on X and Y)
		 */
		static Ref<Mesh> CreateFullscreenQuad();
		
		/**
		 * @brief Create an icosphere mesh
		 * @param radius Sphere radius
		 * @param subdivisions Number of subdivision levels (higher = smoother)
		 * @return Icosphere mesh with more uniform triangle distribution than UV sphere
		 */
		static Ref<Mesh> CreateIcosphere(f32 radius = 1.0f, u32 subdivisions = 2);
		
		/**
		 * @brief Create a torus mesh
		 * @param majorRadius Distance from center to tube center
		 * @param minorRadius Tube radius
		 * @param majorSegments Segments around the major radius
		 * @param minorSegments Segments around the tube
		 * @return Torus mesh
		 */
		static Ref<Mesh> CreateTorus(f32 majorRadius = 1.0f, f32 minorRadius = 0.3f, u32 majorSegments = 24, u32 minorSegments = 12);
		
		// =============================================================================
		// UTILITY AND DEBUGGING PRIMITIVES
		// =============================================================================
		
		/**
		 * @brief Create a grid mesh
		 * @param size Grid size (total width/height)
		 * @param divisions Number of grid divisions
		 * @return Grid mesh for debugging and alignment
		 */
		static Ref<Mesh> CreateGrid(f32 size = 10.0f, u32 divisions = 10);
		
		/**
		 * @brief Create a wireframe cube mesh
		 * @return Cube mesh suitable for wireframe rendering
		 */
		static Ref<Mesh> CreateWireframeCube();
		
		/**
		 * @brief Create coordinate axes mesh
		 * @param length Length of each axis
		 * @return Mesh with X, Y, Z axes (red, green, blue)
		 */
		static Ref<Mesh> CreateCoordinateAxes(f32 length = 1.0f);
		
		// =============================================================================
		// SKINNED MESH PRIMITIVES
		// =============================================================================
		
		/**
		 * @brief Create a skinned cube mesh
		 * @return SkinnedMesh cube with single bone influence
		 */
		static Ref<SkinnedMesh> CreateSkinnedCube();
		
		/**
		 * @brief Create a multi-bone skinned cube mesh
		 * @return SkinnedMesh cube with multiple bone influences for testing
		 */
		static Ref<SkinnedMesh> CreateMultiBoneSkinnedCube();
	};

}
