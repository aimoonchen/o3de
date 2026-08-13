/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Single-loop viewport render step.
//!
//! Industry editors (Unreal FEngineLoop::Tick, Godot Main::iteration, rbfx Engine::RunFrame)
//! drive rendering as ONE step of ONE main loop (pump-input -> tick -> render -> present),
//! presenting once per frame. We mirror that: the editor's sole loop (OnIdle) calls TickRender()
//! once per throttled frame instead of a second, independent QTimer whose timeout events piled up
//! inside the pump and flushed as several renders in one pass (the 233 ms multi-render spike).
//!
//! Registered as an AZ::Interface (a single 1:1 handler = a direct virtual call), not a bus:
//! rendering is a deterministic main-loop step with exactly one consumer.

#include <AzCore/RTTI/RTTI.h>

namespace CrossEngineEditor
{
    //! Implemented by the viewport widget; driven once per frame by the editor's single loop.
    class IViewportTick
    {
    public:
        AZ_RTTI(IViewportTick, "{6E1B7C2A-9F44-4E2D-8A1C-3B5D7A0E9C11}");
        virtual ~IViewportTick() = default;

        //! Advance the camera and render+present exactly one frame for this viewport.
        //! deltaSeconds is the wall-clock time since the previous TickRender.
        virtual void TickRender(float deltaSeconds) = 0;
    };
} // namespace CrossEngineEditor
