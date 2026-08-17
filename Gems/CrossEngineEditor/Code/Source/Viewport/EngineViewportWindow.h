/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! High-performance native render surface for the cross-engine editor viewport.
//!
//! Design: Plan §B2. A QWindow subclass owns a real GPU render
//! surface (Vulkan/Direct3D/Metal). Because setSurfaceType() declares a GPU surface,
//! Qt allocates NO QBackingStore and creates NO OpenGL context - the backend RHI
//! presents its swapchain straight to this window's native handle. It is embedded in
//! the widget tree via QWidget::createWindowContainer (see EngineViewport).
//!
//! IMPROVEMENT over the reference design (annotated inline):
//!   * Handle caching: winId() is resolved once and cached; the reference re-derives it
//!     on every nativeHandle() call.
//!   * DPR-change signal: emits physicalResized on devicePixelRatio changes (monitor
//!     move between HiDPI/SDR screens), which the reference omits - it only watched size.
//!   * A single-shot re-expose guard so a hide/show cycle re-arms firstExposed correctly.

#if !defined(Q_MOC_RUN)
#include <QWindow>
#include <QSurface>
#endif

class QExposeEvent;
class QResizeEvent;
class QFocusEvent;

namespace CrossEngineEditor
{
    //! Owns the native surface. Not used directly by callers - EngineViewport wraps it.
    class EngineViewportWindow final : public QWindow
    {
        Q_OBJECT
    public:
        explicit EngineViewportWindow(QSurface::SurfaceType type, QWindow* parent = nullptr);
        ~EngineViewportWindow() override;

        EngineViewportWindow(const EngineViewportWindow&) = delete;
        EngineViewportWindow& operator=(const EngineViewportWindow&) = delete;

        //! Platform surface handle for the backend RHI:
        //!   Windows: HWND, macOS: NSView*, X11: xcb_window_t, Wayland: wl_surface*.
        //! Resolved on first call and cached (IMPROVEMENT: reference re-derived each time).
        [[nodiscard]] void* platformHandle();

        //! Client-area size in PHYSICAL pixels (logical size * devicePixelRatio, rounded).
        //! The swapchain MUST use this, not size(), or HiDPI displays render blurry (Plan §B2).
        [[nodiscard]] QSize physicalSize() const noexcept;

    Q_SIGNALS:
        //! First expose with a valid surface: the backend creates its swapchain now (Plan §B2).
        void FirstExposed();

        //! Physical client size changed (resize OR devicePixelRatio change): the render
        //! thread recreates its swapchain on the next frame (Plan §B2).
        void PhysicalResized(QSize physicalPx);

        //! The native surface is about to be destroyed: the backend must waitIdle and
        //! release its swapchain synchronously before we return (Plan §B2).
        void AboutToClose();

        //! Raw input (mouse/keyboard/wheel/enter/leave) forwarded to the engine dispatcher.
        void InputEvent(QEvent* event);

        //! Surface visibility changed (dock tab switch / auto-hide). The controller pauses the
        //! frame loop while hidden so it stops presenting to an invisible surface (Plan §B3).
        void VisibilityChanged(bool visible);

        //! Viewport focus gained/lost. The engine starts/stops camera input on this (Plan §B2);
        //! the O3DE ActionManager integration also keys off it (see Plan §B8 B2).
        void FocusChanged(bool hasFocus);

    protected:
        void exposeEvent(QExposeEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void focusInEvent(QFocusEvent* event) override;
        void focusOutEvent(QFocusEvent* event) override;
        bool event(QEvent* event) override;

    private:
        //! Emit PhysicalResized only when the physical size actually changed, so a pure
        //! devicePixelRatio change (monitor hop) still triggers a swapchain recreate but a
        //! no-op resize does not spam the render thread. IMPROVEMENT over the reference.
        void MaybeEmitPhysicalResize();

        void* m_cachedHandle = nullptr;
        QSize m_lastPhysicalSize;
        qreal m_lastPixelRatio = 0.0;
        bool m_firstExpose = true;
    };
} // namespace CrossEngineEditor
