#pragma once

#ifdef OLO_PLATFORM_WINDOWS
    #ifdef OLO_BUILD_DLL
        #define OLO_API __declspec(dllexport)
    #else
        #define OLO_API __declspec(dllimport)
    #endif
#else
    #error OloEngine only supports Windows!
#endif
