/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/EngineViewportWindow.h>

#include <Profiling/CrossEngineProfiler.h>

#include <AzCore/PlatformDef.h>  // AZ_PUSH_DISABLE_WARNING / AZ_POP_DISABLE_WARNING

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QExposeEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QtGui/QPlatformSurfaceEvent>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    EngineViewportWindow::EngineViewportWindow(QSurface::SurfaceType type, QWindow* parent)
        : QWindow(parent)
    {
        // Declaring the surface type is the crux of the design: Qt does NOT allocate a
        // QBackingStore and does NOT create an OpenGL context for a GPU surface (§2.1).
        setSurfaceType(type);

        // IMPROVEMENT: a devicePixelRatio change (dragging the window between a HiDPI and an SDR
        // monitor) does not arrive as a resize - the logical size is unchanged, only the physical
        // size follows the new DPR. QWindow reports this via the screenChanged signal (there is no
        // QEvent::ScreenChanged type). Re-evaluate the physical size so the swapchain follows the
        // DPR; the reference design missed this and left the swapchain at the old physical size.
        connect(this, &QWindow::screenChanged, this,
            [this](QScreen*)
            {
                if (!m_firstExpose)
                {
                    MaybeEmitPhysicalResize();
                }
            });

        // Pause the frame loop while the surface is hidden (dock tab switch / auto-hide) so we
        // stop presenting 60Hz to an invisible surface and wasting GPU (§2.14).
        connect(this, &QWindow::visibleChanged, this,
            [this](bool visible)
            {
                Q_EMIT VisibilityChanged(visible);
            });
    }

    EngineViewportWindow::~EngineViewportWindow() = default;

    void* EngineViewportWindow::platformHandle()
    {
        // IMPROVEMENT: cache the resolved handle. For raster/GPU surfaces the native handle
        // is stable for the window's lifetime, so re-deriving it per frame is wasteful.
        if (m_cachedHandle != nullptr)
        {
            return m_cachedHandle;
        }

#if defined(Q_OS_WIN)
        // Qt6 on Windows: winId() is the HWND.
        m_cachedHandle = reinterpret_cast<void*>(winId());

#elif defined(Q_OS_MAC)
        // Public native interface (Qt 6.0+) returns the CAMetalLayer-backed NSView*.
        if (auto* iface = nativeInterface<QNativeInterface::QCocoaWindow>())
        {
            m_cachedHandle = iface->view();
        }
        else
        {
            m_cachedHandle = reinterpret_cast<void*>(winId());
        }

#elif defined(Q_OS_LINUX)
        if (QGuiApplication::platformName() == QLatin1String("wayland"))
        {
            // Wayland: the backend needs the wl_surface*, not winId(). Public API (Qt 6.7+).
            if (auto* iface = nativeInterface<QNativeInterface::QWaylandWindow>())
            {
                m_cachedHandle = iface->surface();
            }
        }
        else
        {
            // X11: winId() is the xcb_window_t.
            m_cachedHandle = reinterpret_cast<void*>(winId());
        }
#else
        m_cachedHandle = reinterpret_cast<void*>(winId());
#endif
        return m_cachedHandle;
    }

    QSize EngineViewportWindow::physicalSize() const noexcept
    {
        const qreal dpr = devicePixelRatio();
        return QSize(
            static_cast<int>(width() * dpr + 0.5),
            static_cast<int>(height() * dpr + 0.5));
    }

    void EngineViewportWindow::exposeEvent(QExposeEvent* event)
    {
        QWindow::exposeEvent(event);
        if (isExposed() && m_firstExpose)
        {
            m_firstExpose = false;
            m_lastPhysicalSize = physicalSize();
            Q_EMIT FirstExposed();
        }
    }

    void EngineViewportWindow::resizeEvent(QResizeEvent* event)
    {
        QWindow::resizeEvent(event);
        if (!m_firstExpose)
        {
            MaybeEmitPhysicalResize();
        }
    }

    void EngineViewportWindow::MaybeEmitPhysicalResize()
    {
        const QSize physical = physicalSize();
        if (physical != m_lastPhysicalSize)
        {
            m_lastPhysicalSize = physical;
            Q_EMIT PhysicalResized(physical);
        }
    }

    void EngineViewportWindow::focusInEvent(QFocusEvent* event)
    {
        QWindow::focusInEvent(event);
        Q_EMIT FocusChanged(true); // engine: resume camera input (§2.12).
    }

    void EngineViewportWindow::focusOutEvent(QFocusEvent* event)
    {
        QWindow::focusOutEvent(event);
        Q_EMIT FocusChanged(false); // engine: stop camera input so it does not run while unfocused.
    }

    bool EngineViewportWindow::event(QEvent* event)
    {
        CEE_PROFILE_SCOPE("EngineViewportWindow::event");
        switch (event->type())
        {
        case QEvent::PlatformSurface:
            {
                const auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
                if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
                {
                    // Fires for reparent/close/hide - more reliable than closeEvent (§2.8).
                    // Handlers connected DirectConnection release the swapchain before the
                    // native surface is gone. Also drop the cached handle - the next expose
                    // re-derives a fresh one.
                    Q_EMIT AboutToClose();
                    m_cachedHandle = nullptr;
                    m_firstExpose = true; // re-arm so a later re-expose recreates the swapchain.
                }
                break;
            }

        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::Enter:
        case QEvent::Leave:
            Q_EMIT InputEvent(event);
            break;

        // Drag & drop lands on the top-most native QWindow, not the container underneath, so the
        // container's setAcceptDrops is shadowed. Forward these so assets dropped into the viewport
        // reach the controller (§2.15). The controller accepts the QDragMoveEvent to allow the drop.
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::DragLeave:
        case QEvent::Drop:
            Q_EMIT InputEvent(event);
            break;

        default:
            break;
        }
        return QWindow::event(event);
    }
} // namespace CrossEngineEditor
