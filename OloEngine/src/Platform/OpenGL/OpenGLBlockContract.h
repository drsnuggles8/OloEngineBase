#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

namespace OloEngine
{
    class UniformBuffer;

    // The OpenGL half of the uniform block binding contract (ADR 0025 §5a).
    //
    // Vulkan already had a record of which buffer occupies each binding point
    // (VulkanBindingState) because its draw assembly needs one. OpenGL keeps
    // that state in the driver, so this is the same record for GL: which
    // engine uniform buffer the last glBindBufferBase put at each point. With
    // it, a draw or dispatch can be checked the way the Vulkan root-data writer
    // checks one: every uniform block the bound program reads against the size
    // and declared layout of the point's occupant.
    //
    // All of it runs on the thread that owns the GL context.
    namespace OpenGLBlockContract
    {
        // An engine uniform buffer's lifetime. Registered at construction,
        // before the buffer's block declaration is set, so the record keeps
        // the object and reads the declaration when it checks.
        void RegisterBuffer(u32 glName, const UniformBuffer* buffer, u32 allocatedSize);
        void UnregisterBuffer(u32 glName);

        // glBindBufferBase(GL_UNIFORM_BUFFER, binding, glName) just ran.
        void NoteBufferBound(u32 binding, u32 glName);

        // The engine buffer at a uniform binding point, or the null handle when
        // the point is empty or holds a buffer this record does not know.
        [[nodiscard]] RHI::ResourceHandle OccupantHandle(u32 binding);

        // Whether `buffer` is the occupant AND the point was bound through the
        // engine since the last DistrustOccupants. The record above follows
        // the engine's calls; the binding is driver state, and a raw
        // glBindBufferBase changes it without passing through here. A caller
        // that would SKIP a bind on the record's word asks this.
        [[nodiscard]] bool IsVouchedOccupant(u32 binding, RHI::ResourceHandle buffer);
        void DistrustOccupants();

        // glUseProgram(program) just ran.
        void NoteProgramBound(u32 program);

        // Called at the top of every draw and dispatch entry point. Two integer
        // compares when neither the program nor any binding point changed
        // since the last call; otherwise one pass over the program's active
        // uniform blocks. A refused block is reported (BindingContract::
        // ReportViolation) and its binding point is given a zero-filled buffer,
        // so the shader reads zeros instead of another block's fields. The
        // substitution lasts for that program's draws: the displaced occupant
        // is bound again before another program (or the same one with other
        // occupants) is validated, so one refusal does not zero the point for
        // every later reader.
        void ValidateBeforeDraw();

        // Release the zero block while the context is alive.
        void Shutdown();
    } // namespace OpenGLBlockContract
} // namespace OloEngine
