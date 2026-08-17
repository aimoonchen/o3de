/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/EngineViewport.h>
#include <Viewport/EngineViewportWindow.h>

#include <AzCore/PlatformDef.h>  // AZ_PUSH_DISABLE_WARNING / AZ_POP_DISABLE_WARNING

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QVBoxLayout>
#include <QWindow>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    EngineViewport::EngineViewport(QSurface::SurfaceType type, QWidget* parent)
        : QWidget(parent)
    {
        // Container attributes: Qt must not allocate a backing store for, or paint a
        // background over, the region covered by the native GPU surface (Plan §B2).
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAcceptDrops(true);

        // The native render window that owns the real GPU surface.
        m_window = new EngineViewportWindow(type);

        // Embed it. createWindowContainer produces a native-backed widget; do NOT also set
        // WA_NativeWindow on it (that would double-native-ise). We DO block Qt from turning
        // the whole ancestor chain native, which otherwise makes docking re-create HWNDs on
        // every drag and stutters (Plan §B2).
        m_container = QWidget::createWindowContainer(m_window, this);
        m_container->setFocusPolicy(Qt::StrongFocus);
        m_container->setMouseTracking(true);
        m_container->setAttribute(Qt::WA_DontCreateNativeAncestors);
        // Drops physically land on the native QWindow; let both container and window accept them
        // so the forwarded Drag/Drop events (EngineViewportWindow::event) are delivered (Plan §B2).
        m_container->setAcceptDrops(true);

        // Tab focus flows: EngineViewport -> container -> QWindow keyboard (Plan §B2).
        setFocusProxy(m_container);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_container);

        // Forward the native window's lifecycle/input signals up as this widget's own.
        // Same thread => default AutoConnection is a synchronous DirectConnection, which is
        // exactly what AboutToClose needs (swapchain released before the surface is gone).
        connect(m_window, &EngineViewportWindow::FirstExposed, this,
            [this]
            {
                Q_EMIT NativeReady(NativeHandle(), PhysicalSize());
            });
        connect(m_window, &EngineViewportWindow::PhysicalResized, this, &EngineViewport::Resized);
        connect(m_window, &EngineViewportWindow::AboutToClose, this, &EngineViewport::AboutToClose);
        connect(m_window, &EngineViewportWindow::InputEvent, this, &EngineViewport::InputEvent);
        connect(m_window, &EngineViewportWindow::VisibilityChanged, this, &EngineViewport::VisibilityChanged);
        connect(m_window, &EngineViewportWindow::FocusChanged, this, &EngineViewport::FocusChanged);
    }

    EngineViewport::~EngineViewport() = default;

    void* EngineViewport::NativeHandle() const noexcept
    {
        return m_window ? m_window->platformHandle() : nullptr;
    }

    QSize EngineViewport::PhysicalSize() const noexcept
    {
        return m_window ? m_window->physicalSize() : QSize{};
    }

    qreal EngineViewport::PixelRatio() const noexcept
    {
        return m_window ? m_window->devicePixelRatio() : 1.0;
    }

    QWindow* EngineViewport::SurfaceWindow() const noexcept
    {
        return m_window;
    }
} // namespace CrossEngineEditor
