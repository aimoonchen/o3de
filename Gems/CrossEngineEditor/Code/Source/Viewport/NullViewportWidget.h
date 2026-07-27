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

#include <QOpenGLWidget>
#endif

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;

namespace CrossEngineEditor
{
    class ISceneRenderer;

    //! Engine-agnostic editor viewport (plan §6 阶段2 / §3.2).
    //!
    //! A QOpenGLWidget surface that owns a viewport id and answers the picking/camera
    //! contract (ViewportInteractionRequestBus) using AzFramework's engine-neutral camera
    //! math. Each frame it drives the ViewportDebugDisplayEventBus so AzToolsFramework's
    //! manipulators/selection emit their draw calls into GenericDebugDisplay, whose line
    //! and triangle batches are then rendered into this GL surface through the active
    //! backend's ISceneRenderer (3 primitives). With no backend attached the surface just
    //! clears to the editor grey; the manipulator logic still runs against its camera.
    class NullViewportWidget
        : public QOpenGLWidget
        , public AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler
    {
        Q_OBJECT
    public:
        explicit NullViewportWidget(AzFramework::ViewportId viewportId, QWidget* parent = nullptr);
        ~NullViewportWidget() override;

        AzFramework::ViewportId GetViewportId() const { return m_viewportId; }

        //! Route overlay rendering through the given backend scene renderer (set in 阶段2/4).
        //! Passing nullptr detaches and the viewport renders only the clear color.
        void SetSceneRenderer(ISceneRenderer* renderer) { m_sceneRenderer = renderer; }

        // AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler...
        AzFramework::CameraState GetCameraState() override;
        AzFramework::ScreenPoint ViewportWorldToScreen(const AZ::Vector3& worldPosition) override;
        AZ::Vector3 ViewportScreenToWorld(const AzFramework::ScreenPoint& screenPosition) override;
        AzToolsFramework::ViewportInteraction::ProjectedViewportRay ViewportScreenToWorldRay(
            const AzFramework::ScreenPoint& screenPosition) override;
        float DeviceScalingFactor() override;

    protected:
        // QOpenGLWidget...
        void initializeGL() override;
        void resizeGL(int width, int height) override;
        void paintGL() override;

        // QWidget input - translated to AzToolsFramework mouse interactions so the reused
        // manipulators/selection respond exactly as in the original editor...
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void keyReleaseEvent(QKeyEvent* event) override;

    private:
        //! Current viewport size as an engine-neutral screen size (min 1x1).
        AzFramework::ScreenSize ViewportSize() const;

        //! Advance the reusable camera controller and cache the resulting camera state.
        void UpdateCameraState();

        //! Paint the debug-display world-space text labels as a 2D QPainter overlay (drag readouts).
        void PaintTextLabels();

        //! Translate a Qt mouse event to an AzToolsFramework interaction and dispatch it to
        //! the editor selection/manipulator system. Returns true if the editor handled it.
        bool HandleMouseEvent(
            AzToolsFramework::ViewportInteraction::MouseEvent mouseEvent, const QPoint& position,
            Qt::MouseButton eventButton, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers, float wheelDelta);

        AzFramework::ViewportId m_viewportId;
        AzFramework::CameraState m_cameraState;

        EditorViewportCameraController m_cameraController;
        EditorGrid m_grid;

        GenericDebugDisplay m_debugDisplay;
        ISceneRenderer* m_sceneRenderer = nullptr;
    };
} // namespace CrossEngineEditor
