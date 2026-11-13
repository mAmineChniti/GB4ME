#pragma once

// Optional Tracy profiling (https://github.com/wolfpld/tracy).
// Disabled by default: all macros are no-ops unless GB4ME_TRACY is enabled
// in CMake AND the Tracy client header is found. No new dependency when off.

#ifdef GB4ME_TRACY
#if __has_include(<tracy/Tracy.hpp>)
#include <tracy/Tracy.hpp>
#define GB4ME_ZONE ZoneScoped
#define GB4ME_ZONE_N(name) ZoneScopedN(name)
#define GB4ME_FRAME FrameMark
#else
// Requested but unavailable: degrade to no-ops rather than break the build.
#define GB4ME_ZONE ((void) 0)
#define GB4ME_ZONE_N(name) ((void) 0)
#define GB4ME_FRAME ((void) 0)
#endif
#else
#define GB4ME_ZONE ((void) 0)
#define GB4ME_ZONE_N(name) ((void) 0)
#define GB4ME_FRAME ((void) 0)
#endif
