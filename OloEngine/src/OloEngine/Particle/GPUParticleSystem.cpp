#include "OloEnginePCH.h"
#include "GPUParticleSystem.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"

#include <algorithm>

namespace OloEngine
{
	GPUParticleSystem::GPUParticleSystem(u32 maxParticles)
	{
		Init(maxParticles);
	}

	GPUParticleSystem::~GPUParticleSystem()
	{
		Shutdown();
	}

	GPUParticleSystem::GPUParticleSystem(GPUParticleSystem&& other) noexcept
		: m_MaxParticles(other.m_MaxParticles)
		, m_Initialized(other.m_Initialized)
		, m_ParticleSSBO(std::move(other.m_ParticleSSBO))
		, m_AliveIndexSSBO(std::move(other.m_AliveIndexSSBO))
		, m_CounterSSBO(std::move(other.m_CounterSSBO))
		, m_FreeListSSBO(std::move(other.m_FreeListSSBO))
		, m_IndirectDrawSSBO(std::move(other.m_IndirectDrawSSBO))
		, m_EmitStagingSSBO(std::move(other.m_EmitStagingSSBO))
		, m_EmitShader(std::move(other.m_EmitShader))
		, m_SimulateShader(std::move(other.m_SimulateShader))
		, m_CompactShader(std::move(other.m_CompactShader))
		, m_BuildIndirectShader(std::move(other.m_BuildIndirectShader))
	{
		other.m_Initialized = false;
		other.m_MaxParticles = 0;
	}

	GPUParticleSystem& GPUParticleSystem::operator=(GPUParticleSystem&& other) noexcept
	{
		if (this != &other)
		{
			Shutdown();
			m_MaxParticles = other.m_MaxParticles;
			m_Initialized = other.m_Initialized;
			m_ParticleSSBO = std::move(other.m_ParticleSSBO);
			m_AliveIndexSSBO = std::move(other.m_AliveIndexSSBO);
			m_CounterSSBO = std::move(other.m_CounterSSBO);
			m_FreeListSSBO = std::move(other.m_FreeListSSBO);
			m_IndirectDrawSSBO = std::move(other.m_IndirectDrawSSBO);
			m_EmitStagingSSBO = std::move(other.m_EmitStagingSSBO);
			m_EmitShader = std::move(other.m_EmitShader);
			m_SimulateShader = std::move(other.m_SimulateShader);
			m_CompactShader = std::move(other.m_CompactShader);
			m_BuildIndirectShader = std::move(other.m_BuildIndirectShader);
			other.m_Initialized = false;
			other.m_MaxParticles = 0;
		}
		return *this;
	}

	void GPUParticleSystem::Init(u32 maxParticles)
	{
		OLO_PROFILE_FUNCTION();

		if (m_Initialized)
		{
			Shutdown();
		}

		m_MaxParticles = maxParticles;

		// Allocate SSBOs
		m_ParticleSSBO = StorageBuffer::Create(
			maxParticles * GPUParticle::GetSize(),
			ShaderBindingLayout::SSBO_GPU_PARTICLES,
			StorageBufferUsage::DynamicCopy);

		m_AliveIndexSSBO = StorageBuffer::Create(
			maxParticles * sizeof(u32),
			ShaderBindingLayout::SSBO_ALIVE_INDICES,
			StorageBufferUsage::DynamicCopy);

		m_CounterSSBO = StorageBuffer::Create(
			sizeof(GPUParticleCounters),
			ShaderBindingLayout::SSBO_COUNTERS,
			StorageBufferUsage::DynamicCopy);

		m_FreeListSSBO = StorageBuffer::Create(
			maxParticles * sizeof(u32),
			ShaderBindingLayout::SSBO_FREE_LIST,
			StorageBufferUsage::DynamicCopy);

		m_IndirectDrawSSBO = StorageBuffer::Create(
			sizeof(DrawElementsIndirectCommand),
			ShaderBindingLayout::SSBO_INDIRECT_DRAW,
			StorageBufferUsage::DynamicCopy);

		m_EmitStagingSSBO = StorageBuffer::Create(
			MAX_EMIT_BATCH * GPUParticle::GetSize(),
			ShaderBindingLayout::SSBO_EMIT_STAGING,
			StorageBufferUsage::DynamicDraw);

		// Initialize free list: all slots are free [0, 1, 2, ..., maxParticles-1]
		std::vector<u32> freeListInit(maxParticles);
		for (u32 i = 0; i < maxParticles; ++i)
		{
			freeListInit[i] = i;
		}
		m_FreeListSSBO->SetData(freeListInit.data(), maxParticles * sizeof(u32));

		// Initialize counters: 0 alive, all dead
		GPUParticleCounters counters{};
		counters.AliveCount = 0;
		counters.DeadCount = maxParticles;
		counters.EmitCount = 0;
		counters.Pad = 0;
		m_CounterSSBO->SetData(&counters, sizeof(GPUParticleCounters));

		// Initialize indirect draw command (0 instances)
		DrawElementsIndirectCommand cmd{};
		cmd.Count = 6;           // Quad = 6 indices
		cmd.InstanceCount = 0;   // No alive particles yet
		cmd.FirstIndex = 0;
		cmd.BaseVertex = 0;
		cmd.BaseInstance = 0;
		m_IndirectDrawSSBO->SetData(&cmd, sizeof(DrawElementsIndirectCommand));

		// Load compute shaders
		m_EmitShader = ComputeShader::Create("assets/shaders/compute/Particle_Emit.comp");
		m_SimulateShader = ComputeShader::Create("assets/shaders/compute/Particle_Simulate.comp");
		m_CompactShader = ComputeShader::Create("assets/shaders/compute/Particle_Compact.comp");
		m_BuildIndirectShader = ComputeShader::Create("assets/shaders/compute/Particle_BuildIndirect.comp");

		// Validate all shaders loaded successfully
		if (!m_EmitShader || !m_EmitShader->IsValid())
		{
			OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_EmitShader");
			m_Initialized = false;
			return;
		}
		if (!m_SimulateShader || !m_SimulateShader->IsValid())
		{
			OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_SimulateShader");
			m_Initialized = false;
			return;
		}
		if (!m_CompactShader || !m_CompactShader->IsValid())
		{
			OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_CompactShader");
			m_Initialized = false;
			return;
		}
		if (!m_BuildIndirectShader || !m_BuildIndirectShader->IsValid())
		{
			OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_BuildIndirectShader");
			m_Initialized = false;
			return;
		}

		m_Initialized = true;
	}

	void GPUParticleSystem::Shutdown()
	{
		OLO_PROFILE_FUNCTION();

		m_ParticleSSBO = nullptr;
		m_AliveIndexSSBO = nullptr;
		m_CounterSSBO = nullptr;
		m_FreeListSSBO = nullptr;
		m_IndirectDrawSSBO = nullptr;
		m_EmitStagingSSBO = nullptr;
		m_EmitShader = nullptr;
		m_SimulateShader = nullptr;
		m_CompactShader = nullptr;
		m_BuildIndirectShader = nullptr;
		m_Initialized = false;
	}

	void GPUParticleSystem::EmitParticles(std::span<const GPUParticle> newParticles)
	{
		OLO_PROFILE_FUNCTION();

		if (newParticles.empty() || !m_Initialized || !m_EmitShader || !m_EmitShader->IsValid())
		{
			return;
		}

		u32 emitCount = static_cast<u32>(std::min(newParticles.size(), static_cast<sizet>(MAX_EMIT_BATCH)));

		// Upload new particles to staging SSBO
		m_EmitStagingSSBO->Bind();
		m_EmitStagingSSBO->SetData(newParticles.data(), emitCount * GPUParticle::GetSize());

		// Bind all SSBOs
		m_ParticleSSBO->Bind();
		m_CounterSSBO->Bind();
		m_FreeListSSBO->Bind();

		// Dispatch emission compute
		m_EmitShader->Bind();
		m_EmitShader->SetInt("u_EmitCount", static_cast<int>(emitCount));
		m_EmitShader->SetUint("u_MaxParticles", m_MaxParticles);

		u32 groups = (emitCount + EMIT_WORKGROUP_SIZE - 1) / EMIT_WORKGROUP_SIZE;
		RenderCommand::DispatchCompute(groups, 1, 1);
		RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
	}

	void GPUParticleSystem::Simulate(f32 dt, const GPUSimParams& params)
	{
		OLO_PROFILE_FUNCTION();

		if (!m_Initialized || !m_SimulateShader || !m_SimulateShader->IsValid())
		{
			return;
		}

		// Bind SSBOs
		m_ParticleSSBO->Bind();

		// Set simulation uniforms
		m_SimulateShader->Bind();
		m_SimulateShader->SetFloat("u_DeltaTime", dt);
		m_SimulateShader->SetFloat3("u_Gravity", params.Gravity);
		m_SimulateShader->SetFloat("u_DragCoefficient", params.DragCoefficient);
		m_SimulateShader->SetUint("u_MaxParticles", m_MaxParticles);
		m_SimulateShader->SetInt("u_EnableGravity", params.EnableGravity);
		m_SimulateShader->SetInt("u_EnableDrag", params.EnableDrag);

		u32 groups = (m_MaxParticles + SIM_WORKGROUP_SIZE - 1) / SIM_WORKGROUP_SIZE;
		RenderCommand::DispatchCompute(groups, 1, 1);
		RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
	}

	void GPUParticleSystem::Compact()
	{
		OLO_PROFILE_FUNCTION();

		if (!m_Initialized || !m_CompactShader || !m_CompactShader->IsValid())
		{
			return;
		}

		// Reset counters before compaction
		GPUParticleCounters counters{};
		counters.AliveCount = 0;
		counters.DeadCount = 0;
		counters.EmitCount = 0;
		counters.Pad = 0;
		m_CounterSSBO->SetData(&counters, sizeof(GPUParticleCounters));

		// Bind SSBOs
		m_ParticleSSBO->Bind();
		m_AliveIndexSSBO->Bind();
		m_CounterSSBO->Bind();
		m_FreeListSSBO->Bind();

		m_CompactShader->Bind();
		m_CompactShader->SetUint("u_MaxParticles", m_MaxParticles);

		u32 groups = (m_MaxParticles + COMPACT_WORKGROUP_SIZE - 1) / COMPACT_WORKGROUP_SIZE;
		RenderCommand::DispatchCompute(groups, 1, 1);
		RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
	}

	void GPUParticleSystem::PrepareIndirectDraw()
	{
		OLO_PROFILE_FUNCTION();

		if (!m_Initialized || !m_BuildIndirectShader || !m_BuildIndirectShader->IsValid())
		{
			return;
		}

		// Bind SSBOs
		m_CounterSSBO->Bind();
		m_IndirectDrawSSBO->Bind();

		m_BuildIndirectShader->Bind();

		RenderCommand::DispatchCompute(1, 1, 1);
		RenderCommand::MemoryBarrier(MemoryBarrierFlags::Command | MemoryBarrierFlags::ShaderStorage);
	}

	u32 GPUParticleSystem::GetAliveCount() const
	{
		if (!m_Initialized)
		{
			return 0;
		}

		auto counters = m_CounterSSBO->GetData<GPUParticleCounters>();
		return counters.AliveCount;
	}

} // namespace OloEngine
