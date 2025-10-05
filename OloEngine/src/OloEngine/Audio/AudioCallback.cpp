#include "OloEnginePCH.h"
#include "AudioCallback.h"

namespace OloEngine::Audio
{
    void processing_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
    {
        // Assert that the node is valid
        OLO_CORE_ASSERT(pNode != nullptr);
        
        // Safe cast: ProcessingNode has ma_node_base as first member, making this layout-compatible
        AudioCallback::ProcessingNode* node = static_cast<AudioCallback::ProcessingNode*>(static_cast<void*>(pNode));
        
        // Runtime type validation: verify this is actually a ProcessingNode instance
        OLO_CORE_ASSERT(node->m_TypeId == AudioCallback::ProcessingNode::s_MagicTypeId, 
            "Invalid node type! Expected ProcessingNode but got different type. This indicates memory corruption or incorrect node usage.");
        
        AudioCallback* callback = node->m_Callback;
        
        // Early return if callback is null to avoid dereferencing
        if (!callback)
            return;

        // Silence all configured output buses with their actual channel counts
        // This runs exactly once regardless of callback state
        for (sizet i = 0; i < callback->m_BusConfig.m_OutputBuses.size(); ++i)
        {
            if (ppFramesOut[i] != nullptr)
            {
                u32 channelCount = callback->m_BusConfig.m_OutputBuses[i];
                ma_silence_pcm_frames(ppFramesOut[i], *pFrameCountOut, ma_format_f32, channelCount);
            }
        }

        // Only process audio if callback is not suspended
        if (!callback->IsSuspended())
        {
            callback->ProcessBlockBase(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
        }
    }

    bool AudioCallback::Initialize(ma_engine* engine, const BusConfig& busConfig)
    {
        OLO_PROFILE_FUNCTION();
        
        m_Engine = engine;
        m_BusConfig = busConfig;
        
        if (!engine || !engine->pDevice)
        {
            OLO_CORE_ERROR("Engine or device is null");
            return false;
        }

        u32 sampleRate = engine->pDevice->sampleRate;
        u32 blockSize = engine->pDevice->playback.internalPeriodSizeInFrames;

        // Store max block size for runtime validation
        m_MaxBlockSize = blockSize;

        OLO_CORE_ASSERT(!busConfig.m_InputBuses.empty() && !busConfig.m_OutputBuses.empty(), "Bus config must have input and output buses");
        for (const auto& bus : busConfig.m_InputBuses)  
            OLO_CORE_ASSERT(bus > 0, "Input bus channel count must be > 0");
        for (const auto& bus : busConfig.m_OutputBuses) 
            OLO_CORE_ASSERT(bus > 0, "Output bus channel count must be > 0");

        if (m_Node.m_Initialized)
        {
            ma_node_set_state(&m_Node, ma_node_state_stopped);
            ma_node_uninit(&m_Node, nullptr);
            m_Node.m_Initialized = false;
        }

        m_Node.m_Callback = this;
        m_Node.m_Engine = m_Engine;
        m_Node.m_TypeId = ProcessingNode::s_MagicTypeId; // Set magic ID for runtime type validation

        // Validate bus counts before casting to ma_uint8 (max 255)
        // This prevents overflow when casting sizet to ma_uint8
        if (busConfig.m_InputBuses.size() > UINT8_MAX)
        {
            OLO_CORE_ERROR("Too many input buses: {} (maximum is {})", busConfig.m_InputBuses.size(), UINT8_MAX);
            return false;
        }
        if (busConfig.m_OutputBuses.size() > UINT8_MAX)
        {
            OLO_CORE_ERROR("Too many output buses: {} (maximum is {})", busConfig.m_OutputBuses.size(), UINT8_MAX);
            return false;
        }

        // Initialize node with required layout
        m_VTable.onProcess = processing_node_process_pcm_frames;
        m_VTable.onGetRequiredInputFrameCount = nullptr;
        m_VTable.inputBusCount  = static_cast<ma_uint8>(busConfig.m_InputBuses.size());
        m_VTable.outputBusCount = static_cast<ma_uint8>(busConfig.m_OutputBuses.size());
        m_VTable.flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT;
        
        ma_result result;

        ma_node_config nodeConfig = ma_node_config_init();
        nodeConfig.initialState = ma_node_state_stopped;

        nodeConfig.pInputChannels = busConfig.m_InputBuses.data();
        nodeConfig.pOutputChannels = busConfig.m_OutputBuses.data();
        nodeConfig.vtable = &m_VTable;
        result = ma_node_init(&engine->nodeGraph, &nodeConfig, nullptr, &m_Node);

        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("Failed to initialize miniaudio node: {}", result);
            return false;
        }

        result = ma_node_attach_output_bus(&m_Node, 0u, &engine->nodeGraph.endpoint, 0u);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("Failed to attach output bus: {}", result);
            ma_node_uninit(&m_Node, nullptr);
            return false;
        }

        // Call InitBase first and only set flags on success
        if (!InitBase(sampleRate, blockSize, busConfig))
        {
            // InitBase failed - cleanup and ensure flags remain false
            ma_node_uninit(&m_Node, nullptr);
            m_Node.m_Initialized = false;
            m_IsInitialized = false;
            return false;
        }

        // InitBase succeeded - set flags and return success
        m_Node.m_Initialized = true;
        m_IsInitialized = true;
        return true;
    }

    void AudioCallback::Uninitialize()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_Node.m_Initialized)
        {
            ma_node_set_state(&m_Node, ma_node_state_stopped);
            ma_node_uninit(&m_Node, nullptr);
            m_Node.m_Initialized = false;
            m_Node.m_Engine = nullptr;
            m_Node.m_Callback = nullptr;
        }

        m_IsInitialized = false;
        ReleaseResources();
    }

    bool AudioCallback::StartNode()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Node.m_Initialized)
            ma_node_set_state(&m_Node, ma_node_state_started);

        return m_Node.m_Initialized;
    }

} // namespace OloEngine::Audio