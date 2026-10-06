#pragma once

#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>

namespace OloEngine::MCP
{
    // A framebuffer target names its first colour attachment, or depth for a
    // depth-only target. Querying colour index zero on a depth-only framebuffer
    // is out of bounds; an invalid-handle fallback cannot make that query safe.
    inline RHI::ResourceHandle ResolveFramebufferTargetHandle(const Framebuffer& framebuffer, bool& outDepth)
    {
        outDepth = false;
        const auto& attachments = framebuffer.GetSpecification().Attachments.Attachments;
        const bool hasColour = std::any_of(attachments.begin(), attachments.end(), [](const auto& attachment)
                                           { const auto format = attachment.TextureFormat;
            return format != FramebufferTextureFormat::None &&
                   format != FramebufferTextureFormat::DEPTH24STENCIL8 &&
                   format != FramebufferTextureFormat::DEPTH_COMPONENT32F; });
        if (hasColour)
        {
            const auto colour = framebuffer.GetColorAttachmentHandle(0);
            if (colour.IsValid())
                return colour;
        }
        const auto depth = framebuffer.GetDepthAttachmentHandle();
        outDepth = depth.IsValid();
        return depth;
    }
} // namespace OloEngine::MCP
