/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Single include point for Tracy instrumentation across CrossEngineEditor.
//!
//! Usage (official Tracy markup, thin project-namespaced wrappers):
//!   - CEE_PROFILE_FUNCTION();            // scope zone named after the enclosing function
//!   - CEE_PROFILE_SCOPE("MyLabel");      // scope zone with an explicit static name
//!   - CEE_PROFILE_SCOPE_C("Foo", 0xFF0000); // named zone with a fixed colour
//!   - CEE_PROFILE_FRAME();               // mark the end of a frame (call once per frame)
//!   - CEE_PROFILE_FRAME_NAMED("Render"); // secondary/discontinuous frame set
//!   - CEE_PROFILE_TAG("scene", str, len);// attach text to the current zone
//!   - CEE_PROFILE_VALUE("bounds", n);    // plot a numeric value over time
//!   - CEE_PROFILE_MSG("clicked", 7);     // one-off message on the timeline
//!
//! When Tracy is disabled (CEE_HAVE_TRACY undefined, or TRACY_ENABLE not set) every macro
//! compiles to nothing, so instrumented code stays in place at zero cost in release builds.

#if defined(CEE_HAVE_TRACY) && defined(TRACY_ENABLE)

// Tracy's client headers legitimately trip several MSVC warnings (C4366 unaligned '&', C4267
// size_t narrowing, C4244 conversion, ...). CrossEngineEditor compiles first-party code with
// /W4 /WX, which would promote those to errors in every TU that includes the profiler. Silence
// them for the Tracy include only; our own code below the include keeps the strict flags.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4366 4267 4244 4324 4245)
#endif

#include <tracy/Tracy.hpp>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#define CEE_PROFILE_FUNCTION()          ZoneScoped
#define CEE_PROFILE_SCOPE(name)         ZoneScopedN(name)
#define CEE_PROFILE_SCOPE_C(name, rgb)  ZoneScopedNC(name, rgb)
#define CEE_PROFILE_FRAME()             FrameMark
#define CEE_PROFILE_FRAME_NAMED(name)   FrameMarkNamed(name)
#define CEE_PROFILE_TAG(txt, len)       ZoneText(txt, len)
#define CEE_PROFILE_ZONE_NAME(txt, len) ZoneName(txt, len)
#define CEE_PROFILE_VALUE(name, val)    TracyPlot(name, val)
#define CEE_PROFILE_MSG(txt, len)       TracyMessage(txt, len)
#define CEE_PROFILE_MSG_L(txt)          TracyMessageL(txt)

#else

#define CEE_PROFILE_FUNCTION()
#define CEE_PROFILE_SCOPE(name)
#define CEE_PROFILE_SCOPE_C(name, rgb)
#define CEE_PROFILE_FRAME()
#define CEE_PROFILE_FRAME_NAMED(name)
#define CEE_PROFILE_TAG(txt, len)
#define CEE_PROFILE_ZONE_NAME(txt, len)
#define CEE_PROFILE_VALUE(name, val)
#define CEE_PROFILE_MSG(txt, len)
#define CEE_PROFILE_MSG_L(txt)

#endif
