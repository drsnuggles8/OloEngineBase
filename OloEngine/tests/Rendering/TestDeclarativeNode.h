#pragma once

#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RGBuilder.h"

#include <algorithm>
#include <vector>

namespace OloEngine
{
    // @brief Test-only convenience base for `RenderGraphNode` subclasses
    // that need to declare reads/writes from test bodies without spelling
    // out the import + declaration boilerplate every time. Subclasses
    // store declared resource names via `DeclareTestRead(name, kind)` /
    // `DeclareTestWrite(name, kind)`; the base class flushes them during
    // `Setup()` by calling `builder.CreateTexture/Framebuffer/Buffer` and
    // `builder.Read/Write`.
    class TestDeclarativeNode : public RenderGraphNode
    {
      public:
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
        {
            RenderGraphNode::Setup(builder, blackboard);
            for (const auto& read : m_TestReads)
                MirrorRead(builder, read);
            for (const auto& write : m_TestWrites)
            {
                const bool sameResourceRead = std::ranges::any_of(m_TestReads,
                                                                  [&write](const ResourceHandle& read)
                                                                  {
                                                                      return read.Name == write.Name;
                                                                  });
                MirrorWrite(builder, write, sameResourceRead);
            }
        }

        [[nodiscard]] const std::vector<ResourceHandle>& GetReads() const
        {
            return m_TestReads;
        }

        [[nodiscard]] const std::vector<ResourceHandle>& GetWrites() const
        {
            return m_TestWrites;
        }

      protected:
        void DeclareTestRead(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
        {
            AddUniqueHandle(m_TestReads, name, kind);
        }

        void DeclareTestWrite(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
        {
            AddUniqueHandle(m_TestWrites, name, kind);
        }

      private:
        static void AddUniqueHandle(std::vector<ResourceHandle>& resources,
                                    std::string_view name,
                                    ResourceHandle::Kind kind)
        {
            if (const auto it = std::ranges::find_if(resources,
                                                     [name](const ResourceHandle& resource)
                                                     {
                                                         return resource.Name == name;
                                                     });
                it != resources.end())
            {
                return;
            }

            resources.emplace_back(name, kind);
        }

        static ResourceHandle::Kind NormalizeKind(ResourceHandle::Kind kind)
        {
            return kind == ResourceHandle::Kind::Unknown ? ResourceHandle::Kind::Texture2D : kind;
        }

        static RGResourceDesc MakeMirrorDesc(ResourceHandle::Kind kind, std::string_view name)
        {
            // Mark mirror-imported resources as transient (Imported=false) so the
            // imported-resource lifetime validator (which expects a valid backing
            // object for genuinely imported resources) does not flag test
            // passes that never attach real GPU resources.
            auto desc = RGResourceDesc::FromHandleKind(kind, name);
            desc.Imported = false;
            return desc;
        }

        static void MirrorRead(RGBuilder& builder, const ResourceHandle& resource)
        {
            const auto kind = NormalizeKind(resource.Type);
            const auto desc = MakeMirrorDesc(kind, resource.Name);
            switch (kind)
            {
                case ResourceHandle::Kind::Framebuffer:
                {
                    auto handle = builder.CreateFramebuffer(resource.Name, desc);
                    [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::RenderTargetRead);
                    break;
                }
                case ResourceHandle::Kind::UniformBuffer:
                case ResourceHandle::Kind::StorageBuffer:
                {
                    auto handle = builder.CreateBuffer(resource.Name, desc);
                    [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderStorage);
                    break;
                }
                default:
                {
                    auto handle = builder.CreateTexture(resource.Name, desc);
                    [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderSample);
                    break;
                }
            }
        }

        static void MirrorWrite(RGBuilder& builder, const ResourceHandle& resource, bool allowFeedback = false)
        {
            const auto kind = NormalizeKind(resource.Type);
            const auto desc = MakeMirrorDesc(kind, resource.Name);
            switch (kind)
            {
                case ResourceHandle::Kind::Framebuffer:
                {
                    auto handle = builder.CreateFramebuffer(resource.Name, desc);
                    if (allowFeedback)
                        builder.AllowSamePassReadWrite(handle);
                    builder.Write(handle, RGWriteUsage::RenderTarget);
                    break;
                }
                case ResourceHandle::Kind::UniformBuffer:
                case ResourceHandle::Kind::StorageBuffer:
                {
                    auto handle = builder.CreateBuffer(resource.Name, desc);
                    if (allowFeedback)
                        builder.AllowSamePassReadWrite(handle);
                    builder.Write(handle, RGWriteUsage::ShaderStorage);
                    break;
                }
                default:
                {
                    auto handle = builder.CreateTexture(resource.Name, desc);
                    if (allowFeedback)
                        builder.AllowSamePassReadWrite(handle);
                    builder.Write(handle, RGWriteUsage::RenderTarget);
                    break;
                }
            }
        }

      private:
        std::vector<ResourceHandle> m_TestReads;
        std::vector<ResourceHandle> m_TestWrites;
    };
} // namespace OloEngine
