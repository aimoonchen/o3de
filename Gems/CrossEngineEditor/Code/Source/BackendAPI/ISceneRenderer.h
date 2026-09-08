/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Scene rendering surface and immediate-mode overlay primitives (Plan §A6 C4 rendering face / §B2).
//!
//! Rendering/compositing model (Plan §B2, revised for the QWindow native-surface
//! design): the backend owns a swapchain bound to the viewport's native window surface
//! (HWND / NSView+CAMetalLayer / xcb / wl_surface) and presents straight to it. In the
//! same frame it draws the overlay line/triangle batches produced by GenericDebugDisplay.
//! This keeps gizmo rendering fully engine-agnostic while the per-engine cost stays at
//! ~3 primitives instead of the full ~70 DebugDisplayRequests methods.
//!
//! Threading (see Plan §B8 B1 + O3DE AuxGeom double-buffer precedent):
//!   * Overlay geometry MUST be produced on the Qt main thread (AzToolsFramework EBus /
//!     manipulators are not thread-safe). The main thread pushes the geometry through the
//!     Submit* primitives inside a BeginOverlayFrame()/EndOverlayFrame() bracket.
//!   * A backend MAY present on its own render thread. The Submit* calls therefore write
//!     into a staging batch (owned by the backend) that is snapshot-swapped to the render
//!     thread at EndOverlayFrame(). A main-thread-present backend can consume it inline.
//!   * Surface lifecycle is driven by the native window signals and forwarded verbatim:
//!     OnSurfaceCreated (first expose) / OnSurfaceResized (uint64-packed size, render
//!     thread consumes) / OnSurfaceAboutToBeDestroyed (synchronous release).

#include <BackendAPI/BackendTypes.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/span.h>

#include <cstdint>

namespace CrossEngineEditor
{
    //! Pack a physical-pixel viewport size into a single 64-bit word: (w << 32) | h.
    //! The reference design uses this so a cross-thread resize needs only a lock-free
    //! std::atomic<uint64_t> (std::atomic<QSize> is not guaranteed lock-free). See Plan §B2.
    [[nodiscard]] constexpr uint64_t PackViewportSize(uint32_t width, uint32_t height) noexcept
    {
        return (static_cast<uint64_t>(width) << 32) | static_cast<uint64_t>(height);
    }

    constexpr uint32_t UnpackViewportWidth(uint64_t packed) noexcept
    {
        return static_cast<uint32_t>(packed >> 32);
    }

    constexpr uint32_t UnpackViewportHeight(uint64_t packed) noexcept
    {
        return static_cast<uint32_t>(packed & 0xFFFFFFFFull);
    }

    class ISceneRenderer
    {
    public:
        virtual ~ISceneRenderer() = default;

        // --- Native surface lifecycle (driven by EngineViewport signals, Plan §B2) ---

        //! The native window surface was first exposed (Plan §B2). The backend creates its
        //! swapchain bound to nativeWindowHandle at the given physical-pixel size and, if it
        //! renders on its own thread, starts that thread now.
        //!   nativeWindowHandle: HWND / NSView* / xcb_window_t / wl_surface* (see EngineViewport).
        virtual void OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height) = 0;

        //! The native client area's physical size changed (resize OR devicePixelRatio hop,
        //! Plan §B2). Cheap + non-blocking: a threaded backend stores the packed size and
        //! recreates the swapchain on its next frame; an inline backend may resize now.
        virtual void OnSurfaceResized(uint32_t width, uint32_t height) = 0;

        //! The native surface is about to be destroyed (Plan §B2). Called on the Qt main
        //! thread via a DirectConnection: the backend MUST synchronously stop its render
        //! thread, waitIdle and release the swapchain before returning.
        virtual void OnSurfaceAboutToBeDestroyed() = 0;

        //! True once a swapchain is live and RenderFrame will present. Lets the viewport
        //! controller skip overlay work before the surface exists / after it is gone.
        [[nodiscard]] virtual bool IsSurfaceReady() const = 0;

        // --- Per-frame scene + overlay (called from the Qt main-thread idle tick) ---

        //! Begin a new overlay frame for the given camera. Sets the world->view / view->clip
        //! transforms used to project the Submit* overlay geometry, and opens the staging
        //! batch the Submit* calls append to.
        virtual void BeginOverlayFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) = 0;

        //! Draw a batch of line-list vertices (pairs) in world space into the current frame.
        virtual void SubmitLines(AZStd::span<const DebugVertex> vertices) = 0;

        //! Draw a batch of triangle-list vertices (triples) in world space into the frame.
        virtual void SubmitTriangles(AZStd::span<const DebugVertex> vertices) = 0;

        //! Toggle depth testing for subsequent overlay submissions in this frame.
        virtual void SetDepthTest(bool enabled) = 0;

        //! Close the overlay frame. For a threaded backend this snapshot-swaps the staging
        //! batch to the render thread; for an inline backend it renders the scene + overlay
        //! and presents now. Either way one presented frame results.
        virtual void EndOverlayFrame() = 0;
    };
} // namespace CrossEngineEditor
