#pragma once

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <memory>

namespace OloEngine
{
    // The GL 4.6 SSBO namespace is full (ShaderBindingLayout.h, SSBO_GPU_STATS).
    // GPU Scene therefore aliases pass-local slots and is bound only
    // immediately before a consumer; it is never a global sticky binding.
    //
    //   Instances / Geometries / Materials: the GPU instance cull's trio
    //     (SSBO_INSTANCE_DATA / _CULL_INPUT / _DRAW_INDIRECT = 15 / 16 / 17).
    //     #1003 took the first two; the material record takes the third.
    //   Lights / Environments: the Forward+ per-type light buffers
    //     (SSBO_FPLUS_POINT_LIGHTS / _SPOT_LIGHTS = 9 / 10). The raster
    //     migration (#994) routes the clustered evaluator through the canonical
    //     light record and retires the per-type buffers, so the canonical
    //     buffers inherit their numbers rather than claiming new ones.
    //
    // Consequence, pinned by GPUSceneLayoutTest over every shader's include
    // closure: no storage binding may be declared twice in one shader's
    // closure. Since #994 the GLSL side declares one buffer per file
    // (include/GPUSceneMaterials.glsl and friends) and include/GPUScene.glsl
    // declares none, so a consumer takes only the kinds it reads and the two
    // families can coexist in one shader as long as they use different slots.
    // Buffer growth rebinds and unbinds the slot inside EndScene, so every
    // consumer binds per pass.
    struct GPUSceneBindingLayout
    {
        static constexpr u32 Instances = ShaderBindingLayout::SSBO_INSTANCE_DATA;
        static constexpr u32 Geometries = ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT;
        static constexpr u32 Materials = ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT;
        static constexpr u32 Lights = ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS;
        static constexpr u32 Environments = ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS;
        static_assert(Instances < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
        static_assert(Geometries < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
        static_assert(Materials < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
        static_assert(Lights < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
        static_assert(Environments < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
        static_assert(Instances != Geometries && Instances != Materials && Instances != Lights);
        static_assert(Instances != Environments && Geometries != Materials && Geometries != Lights);
        static_assert(Geometries != Environments && Materials != Lights && Materials != Environments);
        static_assert(Lights != Environments);
    };

    struct GPUSceneCapacities
    {
        u32 m_Instances = 64;
        u32 m_Geometries = 64;
        u32 m_Materials = 64;
        u32 m_Lights = 64;
        // A reset retires the global slot for two frames, so the replacement
        // appends; four slots keep a scene reload from growing (and rebinding)
        // the aliased buffer.
        u32 m_Environments = 4;
    };

    // CPU authority for stable GPU-scene identities. Extraction is staged and
    // committed in key order so registry iteration order cannot affect slots.
    // Identity and generation rules: GPUSceneTypes.h, top of file.
    class GPUScene
    {
      public:
        GPUScene();
        ~GPUScene();

        GPUScene(const GPUScene&) = delete;
        auto operator=(const GPUScene&) -> GPUScene& = delete;
        GPUScene(GPUScene&&) noexcept;
        auto operator=(GPUScene&&) noexcept -> GPUScene&;

        void BeginExtraction(u64 ownerToken, const glm::vec3& renderOrigin);
        void ExtractGeometry(const GPUSceneGeometryKey& key, const GPUSceneGeometryInput& input);
        void ExtractInstance(const GPUSceneInstanceKey& key, const GPUSceneInstanceInput& input);
        // Materials commit before instances, so an instance staged with a
        // material key resolves the canonical slot in the same frame.
        void ExtractMaterial(const GPUSceneMaterialKey& key, const GPUSceneMaterialInput& input);
        void ExtractLight(const GPUSceneLightKey& key, const GPUSceneLightInput& input);
        void ExtractEnvironment(const GPUSceneEnvironmentKey& key, const GPUSceneEnvironmentInput& input);
        // True once a material key was staged this frame. The renderer uses it
        // to visit each material once per frame although every submesh that
        // uses it is submitted separately.
        [[nodiscard]] bool IsMaterialStaged(const GPUSceneMaterialKey& key) const;
        void ReportUnsupported(GPUSceneUnsupportedCategory category, u32 count = 1);
        [[nodiscard]] GPUSceneFrameUpdate EndExtraction();

        // GPU resources are explicit so CPU-only tools/tests can use the
        // registry without a renderer context. Resize preserves the RHI
        // object identity and the backend retires old storage frame-safely.
        void InitializeGPU(const GPUSceneCapacities& capacities = {});
        void Upload();
        // Pass-local aliases: call immediately before the consuming dispatch or
        // draw. Allocation/growth releases the aliases before pass setup.
        //
        // Bind() takes all five slots at once and is therefore only for a
        // consumer that reads all five. A raster pass takes the kinds it
        // actually reads: the classic mesh path (#994) takes materials alone,
        // because the per-draw InstanceData stream still owns slot 15 and
        // binding the canonical instances there would replace it mid-pass.
        void Bind() const;
        // Only the material table has a consumer today (#994). The other four
        // get their own binder when something reads them; adding them now would
        // be four untested entry points that look supported and are not.
        [[nodiscard]] bool BindMaterials() const;
        void Shutdown();
        [[nodiscard]] bool HasGPUResources() const;
        [[nodiscard]] RHI::ResourceHandle GetInstanceBufferHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetGeometryBufferHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetMaterialBufferHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetLightBufferHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetEnvironmentBufferHandle() const;

        [[nodiscard]] GPUSceneHandle FindGeometry(const GPUSceneGeometryKey& key) const;
        [[nodiscard]] GPUSceneHandle FindInstance(const GPUSceneInstanceKey& key) const;
        [[nodiscard]] GPUSceneHandle FindMaterial(const GPUSceneMaterialKey& key) const;
        [[nodiscard]] GPUSceneHandle FindLight(const GPUSceneLightKey& key) const;
        [[nodiscard]] GPUSceneHandle FindEnvironment(const GPUSceneEnvironmentKey& key) const;
        [[nodiscard]] bool IsGeometryHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] bool IsInstanceHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] bool IsMaterialHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] bool IsLightHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] bool IsEnvironmentHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneGeometry* GetGeometryRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneInstance* GetInstanceRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneMaterial* GetMaterialRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneLight* GetLightRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneEnvironment* GetEnvironmentRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneFrameUpdate& GetLastFrameUpdate() const;

        // --- Slot-indexed read seam (issue #978) -------------------------
        //
        // The handle-based accessors above answer "give me the record for a
        // key I already hold". A consumer that must walk the WHOLE table —
        // the ray-tracing scene builds one BLAS per live geometry and one TLAS
        // instance per live instance — has no such key, and there was no way
        // to enumerate. These four close that gap and nothing more: they are
        // read-only, they hand back the same const pointers, and they apply
        // the same liveness rule.
        //
        // THE LIVENESS RULE IS THE POINT. Each Get*BySlot returns nullptr
        // unless the slot is live AND its record carries the Active flag.
        // Testing the generation alone is not enough: AdvanceGeneration
        // saturates at u32 max and then leaves the generation UNCHANGED, so a
        // tombstoned slot can keep the generation its last live handle had.
        // That is exactly the stale-record hole #978's acceptance criterion is
        // about, and answering it here means every consumer gets it right.
        [[nodiscard]] u32 GetInstanceSlotCount() const;
        [[nodiscard]] const GPUSceneInstance* GetLiveInstanceRecordBySlot(u32 slot) const;
        // The geometry/material variants additionally take the generation the
        // referring instance recorded, so a record that died and had its slot
        // reused between frames is refused rather than silently substituted.
        [[nodiscard]] const GPUSceneGeometry* GetLiveGeometryRecordBySlot(u32 slot, u32 generation) const;
        [[nodiscard]] const GPUSceneMaterial* GetLiveMaterialRecordBySlot(u32 slot, u32 generation) const;

        // The camera-relative origin this frame's transforms were ENCODED
        // against (GPUScene.cpp's MakeModelRelative). A consumer that decodes
        // or re-keys those transforms must use this one rather than reading
        // the camera again — the two are the same number only until a rebase
        // lands mid-frame.
        [[nodiscard]] const glm::vec3& GetRenderOrigin() const;

        // Invalidates every live handle while retaining slot generations. This
        // is safe for scene reloads and renderer restarts: stale handles cannot
        // become valid merely because allocation starts again at slot zero.
        // Every record is tombstoned, which also drops every resolved texture
        // heap offset; the next extraction re-resolves them.
        void Reset();

      private:
        class Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace OloEngine
