#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLBlockContract.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBlockContract.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glad/gl.h>

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::OpenGLBlockContract
{
    namespace
    {
        struct BufferRecord
        {
            const UniformBuffer* Buffer = nullptr;
            u32 Size = 0;
        };

        struct ProgramBlock
        {
            BindingContract::BlockRequirement Required;
            std::string Name;
        };

        struct ProgramRecord
        {
            std::string Name;
            std::vector<ProgramBlock> Blocks;
        };

        struct State
        {
            std::unordered_map<u32, BufferRecord> Buffers;
            // GL buffer name per uniform binding point, 0 for none.
            std::array<u32, ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS> Occupants{};
            // Bumped whenever an occupant changes, so an unchanged frame state
            // is recognised without walking the blocks.
            u64 Generation = 1;

            std::unordered_map<u32, ProgramRecord> Programs;
            u32 BoundProgram = 0;

            u32 ValidatedProgram = 0;
            u64 ValidatedGeneration = 0;

            u32 ZeroBlock = 0;
            u32 ZeroBlockSize = 0;
        };

        // Leaked on purpose: buffers unregister from their destructors, some of
        // which run during static destruction
        // (registries-must-outlive-their-registrants.md).
        State& Get()
        {
            static auto* s_State = new State;
            return *s_State;
        }

        // The GLSL block name behind the name the driver reports. The GLSL this
        // backend links is SPIRV-Cross output: a block that a later stage also
        // declares is renamed `Name_1` there, and a block with an instance is
        // reported as `Name.instance`. Neither is part of the name a producer
        // declares. No shader block name ends in `_<digits>`
        // (UniformBlockContractSourceTest pins that), so stripping it is exact.
        std::string CanonicalBlockName(std::string name)
        {
            if (const sizet dot = name.find('.'); dot != std::string::npos)
                name.erase(dot);
            sizet end = name.size();
            while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1])) != 0)
                --end;
            if (end < name.size() && end > 1 && name[end - 1] == '_')
                name.erase(end - 1);
            return name;
        }

        // The program's active uniform blocks, read from the linked program
        // once. Introspection rather than SPIR-V reflection because it is the
        // one source every GL program has: compute programs and the bindless
        // variants are built without SPIR-V.
        const ProgramRecord& FindOrBuildProgram(State& state, u32 program)
        {
            if (const auto it = state.Programs.find(program); it != state.Programs.end())
                return it->second;

            ProgramRecord record;
            {
                std::array<GLchar, 128> label{};
                GLsizei length = 0;
                glGetObjectLabel(GL_PROGRAM, program, static_cast<GLsizei>(label.size()), &length, label.data());
                record.Name = length > 0 ? std::string(label.data(), static_cast<sizet>(length))
                                         : "GL program " + std::to_string(program);
            }
            GLint blockCount = 0;
            glGetProgramInterfaceiv(program, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES, &blockCount);
            record.Blocks.reserve(static_cast<sizet>(std::max(blockCount, 0)));
            for (GLint index = 0; index < blockCount; ++index)
            {
                static constexpr std::array<GLenum, 2> kProperties{ GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE };
                std::array<GLint, 2> values{};
                glGetProgramResourceiv(program, GL_UNIFORM_BLOCK, static_cast<GLuint>(index),
                                       static_cast<GLsizei>(kProperties.size()), kProperties.data(),
                                       static_cast<GLsizei>(values.size()), nullptr, values.data());
                std::array<GLchar, 128> name{};
                GLsizei nameLength = 0;
                glGetProgramResourceName(program, GL_UNIFORM_BLOCK, static_cast<GLuint>(index),
                                         static_cast<GLsizei>(name.size()), &nameLength, name.data());
                if (values[0] < 0 || static_cast<sizet>(values[0]) >= state.Occupants.size())
                    continue;
                ProgramBlock block;
                block.Name = CanonicalBlockName(std::string(name.data(), static_cast<sizet>(std::max(nameLength, 0))));
                block.Required = { .Binding = static_cast<u32>(values[0]),
                                   .Size = static_cast<u32>(std::max(values[1], 0)),
                                   .NameHash = BindingContract::HashBlockName(block.Name) };
                record.Blocks.push_back(std::move(block));
            }
            return state.Programs.emplace(program, std::move(record)).first->second;
        }

        // A zero-filled buffer at least `size` bytes long, shared by every
        // refused read. Not registered as an engine buffer: it is recognised
        // by name and never judged.
        u32 EnsureZeroBlock(State& state, u32 size)
        {
            if (state.ZeroBlock != 0 && state.ZeroBlockSize >= size)
                return state.ZeroBlock;
            if (state.ZeroBlock != 0)
                glDeleteBuffers(1, &state.ZeroBlock);
            state.ZeroBlockSize = std::max<u32>(size, 64u * 1024u);
            const std::vector<u8> zeros(state.ZeroBlockSize, u8{ 0 });
            glCreateBuffers(1, &state.ZeroBlock);
            glNamedBufferData(state.ZeroBlock, state.ZeroBlockSize, zeros.data(), GL_STATIC_DRAW);
            return state.ZeroBlock;
        }

        void RetireProgram(u32 program)
        {
            auto& state = Get();
            state.Programs.erase(program);
            // GL reissues program names: the next program with this id is
            // another program and must be walked.
            if (state.ValidatedProgram == program)
                state.ValidatedProgram = 0;
        }
    } // namespace

    void RegisterBuffer(u32 glName, const UniformBuffer* buffer, u32 allocatedSize)
    {
        auto& state = Get();
        state.Buffers[glName] = BufferRecord{ .Buffer = buffer, .Size = allocatedSize };
        // The first buffer is as good a moment as any to hear about retired
        // programs; installing twice is harmless.
        BindingContract::SetProgramRetiredHandler(&RetireProgram);
    }

    void UnregisterBuffer(u32 glName)
    {
        auto& state = Get();
        state.Buffers.erase(glName);
        for (u32& occupant : state.Occupants)
        {
            if (occupant == glName)
            {
                occupant = 0;
                ++state.Generation;
            }
        }
    }

    void NoteBufferBound(u32 binding, u32 glName)
    {
        auto& state = Get();
        if (binding >= state.Occupants.size() || state.Occupants[binding] == glName)
            return;
        state.Occupants[binding] = glName;
        ++state.Generation;
    }

    void NoteProgramBound(u32 program)
    {
        Get().BoundProgram = program;
    }

    void ValidateBeforeDraw()
    {
        auto& state = Get();
        if (state.BoundProgram == state.ValidatedProgram && state.Generation == state.ValidatedGeneration) [[likely]]
            return;
        if (state.BoundProgram == 0 || !BindingContract::Enabled())
        {
            state.ValidatedProgram = state.BoundProgram;
            state.ValidatedGeneration = state.Generation;
            return;
        }

        const u32 program = state.BoundProgram;
        // Copy what the walk needs: a substitution below rebinds, and the map a
        // reference would point into is only ever appended to here, but the
        // intent should not depend on that.
        const ProgramRecord& record = FindOrBuildProgram(state, program);
        for (const ProgramBlock& block : record.Blocks)
        {
            const u32 binding = block.Required.Binding;
            const u32 occupantName = state.Occupants[binding];
            // Nothing recorded, or the zero block: nothing to judge. A point
            // nothing was ever bound to reads as zeros on GL already.
            if (occupantName == 0 || occupantName == state.ZeroBlock)
                continue;
            const auto found = state.Buffers.find(occupantName);
            if (found == state.Buffers.end())
                continue;
            const BufferRecord& occupant = found->second;
            const BindingContract::Verdict verdict =
                BindingContract::Check(block.Required, occupant.Size, occupant.Buffer->GetBlockDeclaration());
            if (verdict == BindingContract::Verdict::Satisfied)
                continue;

            // Ask the driver before acting. This record follows the engine's own
            // bind calls, and a raw GL bind (a test, a third-party SDK) can leave
            // it describing a program or an occupant that is no longer current.
            GLint currentProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
            GLint boundBuffer = 0;
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, binding, &boundBuffer);
            if (static_cast<u32>(currentProgram) != program || static_cast<u32>(boundBuffer) != occupantName)
            {
                state.BoundProgram = static_cast<u32>(currentProgram);
                NoteBufferBound(binding, static_cast<u32>(boundBuffer));
                // Start over against what is actually bound. The record now
                // agrees with the driver for this block, so this terminates.
                ValidateBeforeDraw();
                return;
            }

            BindingContract::ReportViolation(record.Name, block.Required, block.Name, occupant.Size,
                                             occupant.Buffer->GetBlockName(), verdict);
            const u32 zeroBlock = EnsureZeroBlock(state, block.Required.Size);
            glBindBufferBase(GL_UNIFORM_BUFFER, binding, zeroBlock);
            NoteBufferBound(binding, zeroBlock);
        }
        state.ValidatedProgram = program;
        state.ValidatedGeneration = state.Generation;
    }

    void Shutdown()
    {
        auto& state = Get();
        if (state.ZeroBlock != 0)
        {
            for (u32& occupant : state.Occupants)
            {
                if (occupant == state.ZeroBlock)
                    occupant = 0;
            }
            glDeleteBuffers(1, &state.ZeroBlock);
            state.ZeroBlock = 0;
            state.ZeroBlockSize = 0;
            ++state.Generation;
        }
        state.Programs.clear();
        state.BoundProgram = 0;
        state.ValidatedProgram = 0;
    }
} // namespace OloEngine::OpenGLBlockContract
