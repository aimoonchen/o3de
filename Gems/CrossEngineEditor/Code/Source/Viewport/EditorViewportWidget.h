/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#if !defined(Q_MOC_RUN)
#include <Viewport/EditorGrid.h>
#include <Viewport/EditorViewportCameraController.h>
#include <Viewport/GenericDebugDisplay.h>

#include <AzFramework/Viewport/CameraState.h>
#include <AzFramework/Viewport/ViewportId.h>

#include <AzToolsFramework/Viewport/ViewportMessages.h>

#include <QWidget>
#endif

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QResizeEvent;
class QDropEvent;
class QTimer;

namespace CrossEngineEditor
{
    class ISceneRenderer;
    class EngineViewport;
    class ViewportOverlayLabels;

    //! Engine-agnostic editor viewport controller (plan §2 阶段2, revised).
    //!
    //! Post-refactor this is a plain QWidget that HOSTS a native GPU surface rather than
    //! owning an OpenGL context: it embeds an EngineViewport (QWindow + createWindowContainer,
    //! see EngineViewport.h) into which the active backend's swapchain presents directly.
    //! No initializeGL/paintGL/resizeGL, no GL context - the reference design lists
    //! "QOpenGLWidget wrapping the engine" as an explicit anti-pattern (qt6_viewport_design §6).
    //!
    //! It still owns the editor-side logic that must run on the Qt main thread:
    //!   * the viewport id + the picking/camera contract (ViewportInteractionRequestBus),
    //!   * the reusable AzFramework camera controller (orbit/pan/dolly/fly),
    //!   * per-frame overlay generation: it drives ViewportDebugDisplayEventBus so
    //!     AzToolsFramework manipulators/selection emit into GenericDebugDisplay, whose line
    //!     and triangle batches are handed to the backend's ISceneRenderer (3 primitives).
    //! Input arrives from the native window via EngineViewport::InputEvent and is translated
    //! to AzToolsFramework mouse interactions exactly as before. With no backend attached the
    //! surface stays blank; the manipulator logic still runs against its camera.
    class EditorViewportWidget
        : public QWidget
        , public AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler
    {
        Q_OBJECT
    public:
        explicit EditorViewportWidget(AzFramework::ViewportId viewportId, QWidget* parent = nullptr);
        ~EditorViewportWidget() override;

        AzFramework::ViewportId GetViewportId() const { return m_viewportId; }

        //! Route overlay rendering through the given backend scene renderer (set in 阶段2/4).
        //! Passing nullptr detaches and the viewport renders nothing.
        void SetSceneRenderer(ISceneRenderer* renderer) { m_sceneRenderer = renderer; }

        // AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler...
        AzFramework::CameraState GetCameraState() override;
        AzFramework::ScreenPoint ViewportWorldToScreen(const AZ::Vector3& worldPosition) override;
        AZ::Vector3 ViewportScreenToWorld(const AzFramework::ScreenPoint& screenPosition) override;
        AzToolsFramework::ViewportInteraction::ProjectedViewportRay ViewportScreenToWorldRay(
            const AzFramework::ScreenPoint& screenPosition) override;
        float DeviceScalingFactor() override;

    protected:
        // QWidget - keep the 2D text-label overlay sized to the surface.
        void resizeEvent(QResizeEvent* event) override;

    private Q_SLOTS:
        //! Backend swapchain lifecycle, forwarded from the embedded EngineViewport.
        void OnNativeReady(void* nativeHandle, QSize physicalPx);
        void OnSurfaceResized(QSize physicalPx);
        void OnSurfaceAboutToClose();

        //! Idle tick: step the camera, generate overlays, drive one presented frame.
        void OnFrameTick();

    private:
        //! Raw input forwarded from the native render window; translated to editor interactions.
        void HandleNativeInput(QEvent* event);

        //! Handle an asset dropped onto the viewport surface (forwarded from the native window,
        //! §2.15). v1 resolves the drop point to a world position; actual spawning is a follow-up.
        void HandleAssetDrop(QDropEvent* dropEvent);

        //! Current viewport size as an engine-neutral screen size in physical pixels (min 1x1).
        AzFramework::ScreenSize ViewportSize() const;

        //! Advance the reusable camera controller and cache the resulting camera state.
        void UpdateCameraState();

        //! Refresh the 2D text-label overlay (drag readouts) from the last frame's labels.
        void UpdateTextLabels();

        //! Translate a Qt mouse event to an AzToolsFramework interaction and dispatch it to
        //! the editor selection/manipulator system. Returns true if the editor handled it.
        bool HandleMouseEvent(
            AzToolsFramework::ViewportInteraction::MouseEvent mouseEvent, const QPoint& position,
            Qt::MouseButton eventButton, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers, float wheelDelta);

        //! Map a logical-pixel Qt position from the native window to physical-pixel screen space.
        AzFramework::ScreenPoint ToPhysicalScreenPoint(const QPointF& logicalPos) const;

        AzFramework::ViewportId m_viewportId;
        AzFramework::CameraState m_cameraState;

        EditorViewportCameraController m_cameraController;
        EditorGrid m_grid;

        GenericDebugDisplay m_debugDisplay;
        ISceneRenderer* m_sceneRenderer = nullptr;

        //! Native GPU render surface embedded in this widget (owns the QWindow).
        EngineViewport* m_engineViewport = nullptr;
        //! Transparent 2D overlay on top of the surface for world-space text labels.
        ViewportOverlayLabels* m_labelOverlay = nullptr;
        //! Drives the main-thread idle tick (camera step + overlay generation + present).
        QTimer* m_frameTimer = nullptr;

        //! Cached physical size of the native surface (updated on ready/resize).
        QSize m_physicalSize{ 1, 1 };
        qreal m_pixelRatio = 1.0;
    };
} // namespace CrossEngineEditor
