// Single translation unit that compiles the pl_mpeg implementation.
//
// pl_mpeg (https://github.com/phoboslab/pl_mpeg) is a single-file, public-domain
// MPEG-1 video + MP2 audio decoder. It is the decode backend for the OloEngine
// video subsystem. The rest of the engine only ever #includes the declarations
// (via <pl_mpeg.h> without the IMPLEMENTATION macro) through VideoDecoder.cpp, so
// the heavy implementation lives in this one isolated TU.
//
// The warning-level is dialled down to 0 around the include because pl_mpeg is
// third-party C code we do not want to fix; the engine's /W4 would otherwise spam
// the build log (it is not /WX, so this is purely about keeping output readable).
//
// CMake force-includes OloEnginePCH.h at the top of every TU, so it is already
// active before this point — nothing here depends on it.

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
