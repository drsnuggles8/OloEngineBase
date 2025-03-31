#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderCommands/RenderCommandBase.h"
#include <unordered_map>

namespace OloEngine
{
    // Command dispatcher - responsible for executing commands through registered dispatch functions
    class CommandDispatcher
    {
    public:
        static void Init();
        static void Shutdown();

        // Register a dispatch function for a specific command type
        static void RegisterDispatchFunction(CommandType type, DispatchFn dispatchFn);

        // Get a dispatch function for a command type
        static DispatchFn GetDispatchFunction(CommandType type);

        // Execute a command directly
        static void Execute(const CommandPacket* packet);

    private:
        // Core dispatch functions
        static void DispatchDrawIndexed(const void* commandData);
        static void DispatchDrawIndexedInstanced(const void* commandData);
        static void DispatchSetBlendState(const void* commandData);
        static void DispatchSetDepthState(const void* commandData);
        static void DispatchSetStencilState(const void* commandData);
        static void DispatchSetCullingState(const void* commandData);
        static void DispatchSetLineWidth(const void* commandData);
        static void DispatchSetPolygonMode(const void* commandData);
        static void DispatchSetScissorState(const void* commandData);
        static void DispatchSetColorMask(const void* commandData);
        static void DispatchSetPolygonOffset(const void* commandData);
        static void DispatchSetMultisampling(const void* commandData);
        static void DispatchSetTexture(const void* commandData);

        // Dispatch function map
        static std::unordered_map<CommandType, DispatchFn> s_DispatchFunctions;
    };
}