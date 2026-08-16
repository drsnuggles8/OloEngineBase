// =============================================================================
// OpenGLStateGuard.h
//
// OpenGL half of the GLStateGuard seam (#691 Phase 9, ADR 0011 §1.6). The
// function DECLARATIONS deliberately live in the neutral seam header under
// Renderer/Debug/ (StateGuardBackend.h) — GLStateGuard.cpp must be able to
// call them without including a Platform/ header — so this header only
// re-exports them for the defining TU, OpenGLStateGuard.cpp.
// =============================================================================

#pragma once

#include "OloEngine/Renderer/Debug/StateGuardBackend.h"
