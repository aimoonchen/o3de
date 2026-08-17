/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! QWidget container that embeds the native EngineViewportWindow into the dock tree.
//!
//! Design: Plan §B2. This is the widget the docking system sees; it
//! wraps EngineViewportWindow with QWidget::createWindowContainer and forwards the
//! window's lifecycle/input signals. The container carries the attributes that stop Qt
//! from allocating a backing store or painting a background over the GPU surface, and
//! sets WA_DontCreateNativeAncestors so docking does not recursively native-ise the
//! ancestor chain (Plan §B2).

#if !defined(Q_MOC_RUN)
#include <QWidget>
#include <QSurface>
#endif

class QWindow;

namespace CrossEngineEditor
{
    class EngineViewportWindow;

    class EngineViewport final : public QWidget
    {
        Q_OBJECT
    public:
        explicit EngineViewport(QSurface::SurfaceType type, QWidget* parent = nullptr);
        ~EngineViewport() override;

        EngineViewport(const EngineViewport&) = delete;
        EngineViewport& operator=(const EngineViewport&) = delete;

        //! Native surface handle for the backend RHI (see EngineViewportWindow::platformHandle).
        [[nodiscard]] void* NativeHandle() const noexcept;

        //! Client-area size in physical pixels - the swapchain size (Plan §B2).
        [[nodiscard]] QSize PhysicalSize() const noexcept;

        [[nodiscard]] qreal PixelRatio() const noexcept;

        //! The underlying native QWindow (for advanced callers that need the QSurface).
        [[nodiscard]] QWindow* SurfaceWindow() const noexcept;

    Q_SIGNALS:
        //! Native window first exposed - the backend creates its swapchain now.
        void NativeReady(void* handle, QSize physicalPx);

        //! Physical client size changed - the render thread recreates the swapchain.
        void Resized(QSize physicalPx);

        //! Native surface about to be destroyed - backend waitIdle + destroy swapchain.
        void AboutToClose();

        //! Raw input forwarded from the native window to the engine input dispatcher.
        void InputEvent(QEvent* event);

        //! Surface visibility changed - the controller pauses/resumes its frame loop (Plan §B3).
        void VisibilityChanged(bool visible);

        //! Viewport focus gained/lost - engine camera input start/stop (Plan §B2).
        void FocusChanged(bool hasFocus);

    private:
        EngineViewportWindow* m_window = nullptr;
        QWidget* m_container = nullptr;
    };
} // namespace CrossEngineEditor
