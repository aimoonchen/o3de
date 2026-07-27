/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! C5 + C4-device: scene rendering surface and immediate-mode overlay primitives.
//!
//! Rendering/compositing model (plan §3.2): the backend renders the engine scene
//! directly into the viewport's native window surface, and in the same frame draws
//! the overlay line/triangle batches produced by GenericDebugDisplay. This keeps
//! gizmo rendering fully engine-agnostic while the per-engine cost stays at ~3
//! primitives instead of the full ~70 DebugDisplayRequests methods.

#include <BackendAPI/BackendTypes.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/span.h>

#include <cstdint>

namespace CrossEngineEditor
{
    class ISceneRenderer
    {
    public:
        virtual ~ISceneRenderer() = default;

        //! Bind the renderer to the editor viewport's native window surface.
        virtual void AttachToWindow(void* nativeWindowHandle, uint32_t width, uint32_t height) = 0;

        //! React to viewport resize.
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        //! Render one scene frame for the given camera. Called from the editor idle tick.
        virtual void RenderFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) = 0;

        // --- IDebugRenderDevice: the only 3 primitives a backend must implement ---

        //! Draw a batch of line-list vertices (pairs) in world space.
        virtual void SubmitLines(AZStd::span<const DebugVertex> vertices) = 0;

        //! Draw a batch of triangle-list vertices (triples) in world space.
        virtual void SubmitTriangles(AZStd::span<const DebugVertex> vertices) = 0;

        //! Toggle depth testing for subsequent overlay submissions in this frame.
        virtual void SetDepthTest(bool enabled) = 0;

        //! Release any GPU resources. Called by the viewport with its GL context current,
        //! before the surface is destroyed. Backends without GL state may ignore this.
        virtual void ReleaseGraphics() {}

        //! Notify the renderer that the owning GL context was rebuilt (e.g. the viewport was
        //! reparented by the docking system). The renderer must drop stale GL handles without
        //! issuing GL calls; the next AttachToWindow/RenderFrame recreates them. No-op by default.
        virtual void InvalidateGraphics() {}
    };
} // namespace CrossEngineEditor
