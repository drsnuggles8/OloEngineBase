#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLBlockContract.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBlockContract.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glad/gl.h>

#include <array>
#include <cctype>
#include <string_view>
#include <unordered_map>

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
            FString Name;
        };
    } // namespace
} // namespace OloEngine::OpenGLBlockContract

namespace OloEngine
{
    // Member by member, so adding a member that does not survive a bitwise
    // move closes the gate again (engine-owned-containers.md).
    template<>
    struct TIsTriviallyRelocatable<OpenGLBlockContract::ProgramBlock>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(OpenGLBlockContract::ProgramBlock::Required)> &&
                                      TIsTriviallyRelocatable_V<decltype(OpenGLBlockContract::ProgramBlock::Name)>;
    };
} // namespace OloEngine

namespace OloEngine::OpenGLBlockContract
{
    namespace
    {
        struct ProgramRecord
        {
            FString Name;
            TArray<ProgramBlock> Blocks;
        };

        struct State
        {
            std::unordered_map<u32, BufferRecord> Buffers;
            // GL buffer name per uniform binding point, 0 for none.
            std::array<u32, ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS> Occupants{};
            // ...and the same occupant as an engine identity, resolved once
            // when it is bound so that asking costs one array read.
            std::array<RHI::ResourceHandle, ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS> OccupantHandles{};
            // The epoch in which each point was last bound through the engine.
            // A point is vouched for while that is the current epoch.
            std::array<u64, ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS> VouchedAt{};
            u64 VouchEpoch = 1;
            // Bumped whenever an occupant changes, so an unchanged frame state
            // is recognised without walking the blocks.
            u64 Generation = 1;

            std::unordered_map<u32, ProgramRecord> Programs;
            // What each program's SPIR-V says its blocks are called
            // (DeclareProgramBlocks). Not a cache: Shutdown keeps it, and it
            // goes when the program name is retired or declared again.
            std::unordered_map<u32, TArray<DeclaredBlock>> Declared;
            u32 BoundProgram = 0;

            u32 ValidatedProgram = 0;
            u64 ValidatedGeneration = 0;

            u32 ZeroBlock = 0;
            u32 ZeroBlockSize = 0;
            // Per binding point, the buffer the zero block is standing in for
            // (0 for none). A refusal is one program's: the occupant goes back
            // as soon as another program, or another set of occupants, is
            // validated, so a later reader of the same point finds the buffer
            // that was really bound there and not zeros.
            std::array<u32, ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS> Displaced{};
            u32 DisplacedCount = 0;
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
        std::string_view CanonicalBlockName(std::string_view name)
        {
            if (const sizet dot = name.find('.'); dot != std::string_view::npos)
                name = name.substr(0, dot);
            sizet end = name.size();
            while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1])) != 0)
                --end;
            if (end < name.size() && end > 1 && name[end - 1] == '_')
                name = name.substr(0, end - 1);
            return name;
        }

        // One requirement of `record`, once: two stages that declare the same
        // block are one reader.
        void AddBlock(ProgramRecord& record, u32 binding, u32 size, std::string_view name)
        {
            const u32 nameHash = BindingContract::HashBlockName(name);
            const bool known = record.Blocks.ContainsByPredicate(
                [&](const ProgramBlock& block)
                { return block.Required.Binding == binding && block.Required.Size == size && block.Required.NameHash == nameHash; });
            if (known)
                return;
            ProgramBlock block;
            block.Name = FString(name);
            block.Required = { .Binding = binding, .Size = size, .NameHash = nameHash };
            record.Blocks.Add(std::move(block));
        }

        // The names the program's SPIR-V gives the block the driver reports at
        // `binding`, each added as a requirement. Returns how many.
        //
        // Two stages may declare one binding under two names (`Camera` and
        // `CameraMatrices`). The size tells such blocks apart where it
        // differs. Where it does not, nothing tells which of the driver's
        // blocks is which, and nothing needs to: both names read the point, so
        // the occupant is held to EVERY one of them. Picking one would make
        // the verdict follow the order the stages were reflected in.
        u32 AddDeclaredBlocks(const State& state, ProgramRecord& record, u32 program, u32 binding, u32 size)
        {
            const auto found = state.Declared.find(program);
            if (found == state.Declared.end())
                return 0;
            u32 added = 0;
            for (const DeclaredBlock& block : found->second)
            {
                if (block.Binding == binding && block.Size == size)
                {
                    AddBlock(record, binding, size, block.Name.ToView());
                    ++added;
                }
            }
            if (added != 0)
                return added;
            // No declaration of this size: the driver measures the block
            // otherwise than the reflection does. The binding still says whose
            // it is.
            for (const DeclaredBlock& block : found->second)
            {
                if (block.Binding == binding)
                {
                    AddBlock(record, binding, size, block.Name.ToView());
                    ++added;
                }
            }
            return added;
        }

        // The program's active uniform blocks, read once. Which blocks are
        // active, where and how large comes from the linked program, the one
        // source every GL program has. A block's name comes from the program's
        // SPIR-V where it was reflected from SPIR-V, because a program linked
        // from SPIR-V has no names for a driver to report; compute programs
        // and the bindless variants are built from GLSL text with no
        // reflection, declare nothing, and the driver names their blocks.
        const ProgramRecord& FindOrBuildProgram(State& state, u32 program)
        {
            if (const auto it = state.Programs.find(program); it != state.Programs.end())
                return it->second;

            ProgramRecord record;
            {
                std::array<GLchar, 128> label{};
                GLsizei length = 0;
                glGetObjectLabel(GL_PROGRAM, program, static_cast<GLsizei>(label.size()), &length, label.data());
                record.Name = length > 0 ? FString(std::string_view(label.data(), static_cast<sizet>(length)))
                                         : FString(std::string_view(fmt::format("GL program {}", program)));
            }
            GLint blockCount = 0;
            glGetProgramInterfaceiv(program, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES, &blockCount);
            record.Blocks.Reserve(std::max(blockCount, 0));
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
                const auto binding = static_cast<u32>(values[0]);
                const auto size = static_cast<u32>(std::max(values[1], 0));
                if (AddDeclaredBlocks(state, record, program, binding, size) != 0)
                    continue;
                const std::string_view blockName =
                    CanonicalBlockName(std::string_view(name.data(), static_cast<sizet>(std::max(nameLength, 0))));
                AddBlock(record, binding, size, blockName);
                if (blockName.empty())
                {
                    // Neither source names it, so its layout cannot be
                    // compared with the occupant's (BindingContract::Check
                    // judges it on its byte range). Said once per program.
                    OLO_CORE_ERROR("[BindingContract] '{}' reads a uniform block at binding {} ({} B) that neither its SPIR-V nor "
                                   "the driver names. Its reads are checked for size only, not for layout.",
                                   record.Name.ToView(), binding, size);
                }
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
            glCreateBuffers(1, &state.ZeroBlock);
            glNamedBufferData(state.ZeroBlock, state.ZeroBlockSize, nullptr, GL_STATIC_DRAW);
            // A null data pointer fills the store with zeros.
            glClearNamedBufferData(state.ZeroBlock, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
            return state.ZeroBlock;
        }

        void RetireProgram(u32 program)
        {
            auto& state = Get();
            state.Programs.erase(program);
            state.Declared.erase(program);
            // GL reissues program names: the next program with this id is
            // another program and must be walked.
            if (state.ValidatedProgram == program)
                state.ValidatedProgram = 0;
        }
    } // namespace

    void DeclareProgramBlocks(u32 program, std::span<const DeclaredBlock> blocks)
    {
        auto& state = Get();
        // Copied before the old declaration goes: `blocks` may be the span
        // DeclaredBlocks returned for this very program.
        TArray<DeclaredBlock> declared;
        declared.Reserve(static_cast<i32>(blocks.size()));
        for (const DeclaredBlock& block : blocks)
            declared.Add(block);
        if (declared.IsEmpty())
            state.Declared.erase(program);
        else
            state.Declared.insert_or_assign(program, std::move(declared));
        // Whatever was read from this program name before was read without
        // these names, or from the program that had the name earlier.
        state.Programs.erase(program);
        if (state.ValidatedProgram == program)
            state.ValidatedProgram = 0;
        // Programs are declared before buffers in some start-up orders;
        // installing twice is harmless.
        BindingContract::SetProgramRetiredHandler(&RetireProgram);
    }

    std::span<const DeclaredBlock> DeclaredBlocks(u32 program)
    {
        const auto& state = Get();
        const auto found = state.Declared.find(program);
        if (found == state.Declared.end())
            return {};
        return { found->second.GetData(), static_cast<sizet>(found->second.Num()) };
    }

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
        for (sizet binding = 0; binding < state.Occupants.size(); ++binding)
        {
            if (state.Occupants[binding] == glName)
            {
                state.Occupants[binding] = 0;
                state.OccupantHandles[binding] = {};
                ++state.Generation;
            }
            if (state.Displaced[binding] == glName)
            {
                state.Displaced[binding] = 0;
                --state.DisplacedCount;
            }
        }
    }

    void NoteBufferBound(u32 binding, u32 glName)
    {
        auto& state = Get();
        if (binding >= state.Occupants.size())
            return;
        // The bind just happened, whatever the record said before it.
        state.VouchedAt[binding] = state.VouchEpoch;
        if (state.Occupants[binding] == glName)
            return;
        state.Occupants[binding] = glName;
        const auto found = state.Buffers.find(glName);
        state.OccupantHandles[binding] = found != state.Buffers.end() ? found->second.Buffer->GetRHIHandle() : RHI::ResourceHandle{};
        ++state.Generation;
    }

    RHI::ResourceHandle OccupantHandle(u32 binding)
    {
        const auto& state = Get();
        return binding < state.OccupantHandles.size() ? state.OccupantHandles[binding] : RHI::ResourceHandle{};
    }

    bool IsVouchedOccupant(u32 binding, RHI::ResourceHandle buffer)
    {
        const auto& state = Get();
        return buffer.IsValid() && binding < state.OccupantHandles.size() && state.VouchedAt[binding] == state.VouchEpoch &&
               state.OccupantHandles[binding] == buffer;
    }

    void DistrustOccupants()
    {
        ++Get().VouchEpoch;
    }

    void NoteProgramBound(u32 program)
    {
        Get().BoundProgram = program;
    }

    namespace
    {
        // Put back every occupant the zero block is standing in for. A point
        // somebody bound since keeps what they bound, and a displaced buffer
        // that was destroyed meanwhile is already forgotten (UnregisterBuffer).
        void RestoreDisplaced(State& state)
        {
            if (state.DisplacedCount == 0) [[likely]]
                return;
            for (u32 binding = 0; binding < state.Displaced.size(); ++binding)
            {
                const u32 displaced = state.Displaced[binding];
                if (displaced == 0)
                    continue;
                state.Displaced[binding] = 0;
                if (state.Occupants[binding] != state.ZeroBlock)
                    continue;
                glBindBufferBase(GL_UNIFORM_BUFFER, binding, displaced);
                NoteBufferBound(binding, displaced);
            }
            state.DisplacedCount = 0;
        }
    } // namespace

    void ValidateBeforeDraw()
    {
        auto& state = Get();
        if (state.BoundProgram == state.ValidatedProgram && state.Generation == state.ValidatedGeneration) [[likely]]
            return;
        // Another program, or other occupants: what the last refused program
        // was given instead of its occupant is not this draw's business.
        RestoreDisplaced(state);
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

            BindingContract::ReportViolation(record.Name.ToView(), block.Required, block.Name.ToView(), occupant.Size,
                                             occupant.Buffer->GetBlockName(), verdict);
            const u32 zeroBlock = EnsureZeroBlock(state, block.Required.Size);
            glBindBufferBase(GL_UNIFORM_BUFFER, binding, zeroBlock);
            NoteBufferBound(binding, zeroBlock);
            // For this program's draws only: RestoreDisplaced puts the
            // occupant back before anything else is validated, which is what
            // the Vulkan root writer does by withholding one draw's address.
            if (state.Displaced[binding] == 0)
                ++state.DisplacedCount;
            state.Displaced[binding] = occupantName;
        }
        state.ValidatedProgram = program;
        state.ValidatedGeneration = state.Generation;
    }

    void Shutdown()
    {
        auto& state = Get();
        if (state.ZeroBlock != 0)
        {
            for (sizet binding = 0; binding < state.Occupants.size(); ++binding)
            {
                if (state.Occupants[binding] == state.ZeroBlock)
                {
                    state.Occupants[binding] = 0;
                    state.OccupantHandles[binding] = {};
                }
            }
            state.Displaced.fill(0);
            state.DisplacedCount = 0;
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
