/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/NullViewportWidget.h>
#include <BackendAPI/ISceneRenderer.h>

#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzFramework/Viewport/ScreenGeometry.h>
#include <AzFramework/Viewport/ViewportScreen.h>

#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/Viewport/ViewportTypes.h>
#include <AzToolsFramework/ViewportSelection/EditorInteractionSystemViewportSelectionRequestBus.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWheelEvent>
AZ_POP_DISABLE_WARNING

#include <cstdio>

namespace
{
    void ViewportBootLog(const char* stage)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, "cee_boot.log", "a") == 0 && f != nullptr)
        {
            fprintf(f, "%s\n", stage);
            fclose(f);
        }
    }
} // namespace

namespace CrossEngineEditor
{
    namespace
    {
        //! Blender-inspired viewport clear color (neutral warm grey).
        constexpr float k_clearColor[4] = { 0.231f, 0.231f, 0.231f, 1.0f };

        //! Multisample count for the anti-aliased Blender-grade gizmo look.
        constexpr int k_msaaSamples = 4;
    } // namespace

    NullViewportWidget::NullViewportWidget(AzFramework::ViewportId viewportId, QWidget* parent)
        : QOpenGLWidget(parent)
        , m_viewportId(viewportId)
    {
        setObjectName(QStringLiteral("NullViewport"));
        // Keyboard focus so viewport shortcuts/interactions can target this surface.
        setFocusPolicy(Qt::StrongFocus);
        // Track mouse moves without a button held so manipulators get hover feedback.
        setMouseTracking(true);

        QSurfaceFormat format;
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setVersion(3, 3);
        format.setDepthBufferSize(24);
        format.setSamples(k_msaaSamples);
        setFormat(format);

        // Drive continuous repaint so hover/animation feedback stays live (v1 idle tick).
        auto* repaintTimer = new QTimer(this);
        connect(repaintTimer, &QTimer::timeout, this, QOverload<>::of(&NullViewportWidget::update));
        repaintTimer->start(16);

        UpdateCameraState();
        AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler::BusConnect(m_viewportId);
    }

    NullViewportWidget::~NullViewportWidget()
    {
        AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler::BusDisconnect();

        // Release the backend's GL resources while this widget's context is still current,
        // otherwise Qt destroys the GL objects with no context and warns.
        if (m_sceneRenderer)
        {
            makeCurrent();
            m_sceneRenderer->ReleaseGraphics();
            doneCurrent();
        }
    }

    void NullViewportWidget::initializeGL()
    {
        ViewportBootLog("viewport:initializeGL:enter");
        QOpenGLFunctions* functions = context()->functions();
        functions->glClearColor(k_clearColor[0], k_clearColor[1], k_clearColor[2], k_clearColor[3]);
        functions->glEnable(GL_DEPTH_TEST);
        functions->glEnable(GL_LINE_SMOOTH);
        functions->glEnable(GL_BLEND);
        functions->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        if (m_sceneRenderer)
        {
            // This runs once per GL context. If the widget was reparented (docking) the old
            // context and its GL objects are gone, so drop stale handles before rebuilding.
            m_sceneRenderer->InvalidateGraphics();
            m_sceneRenderer->AttachToWindow(reinterpret_cast<void*>(winId()), width(), height());
        }
        ViewportBootLog("viewport:initializeGL:done");
    }

    void NullViewportWidget::resizeGL(int width, int height)
    {
        ViewportBootLog("viewport:resizeGL:enter");
        UpdateCameraState();
        if (m_sceneRenderer)
        {
            m_sceneRenderer->Resize(static_cast<uint32_t>(AZStd::max(1, width)), static_cast<uint32_t>(AZStd::max(1, height)));
        }
        ViewportBootLog("viewport:resizeGL:done");
    }

    void NullViewportWidget::paintGL()
    {
        ViewportBootLog("viewport:paintGL:enter");
        QOpenGLFunctions* functions = context()->functions();
        functions->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Step the camera each frame so smoothing/inertia and held-key movement advance.
        UpdateCameraState();
        ViewportBootLog("viewport:paintGL:camera-stepped");

        if (!m_sceneRenderer)
        {
            return;
        }

        // Hand the current camera to the backend so it can set up its world->clip transform,
        // then let AzToolsFramework emit gizmo/selection geometry into GenericDebugDisplay.
        const AZ::Matrix4x4 worldToView = AZ::Matrix4x4::CreateFromMatrix3x4(AzFramework::CameraView(m_cameraState));
        const AZ::Matrix4x4 viewToClip = AzFramework::CameraProjection(m_cameraState);
        m_sceneRenderer->RenderFrame(worldToView, viewToClip);
        ViewportBootLog("viewport:paintGL:renderframe-done");

        m_debugDisplay.ClearFrame();

        // Editor overlays owned by the shell (grid) draw first, then engine/manipulator overlays.
        m_grid.Draw(m_debugDisplay, m_cameraState);
        ViewportBootLog("viewport:paintGL:grid-done");

        const AzFramework::ViewportInfo viewportInfo{ static_cast<int>(m_viewportId) };
        AzFramework::ViewportDebugDisplayEventBus::Event(
            AzToolsFramework::GetEntityContextId(), &AzFramework::ViewportDebugDisplayEvents::DisplayViewport, viewportInfo,
            m_debugDisplay);
        ViewportBootLog("viewport:paintGL:display-done");
        m_debugDisplay.Flush(*m_sceneRenderer);
        ViewportBootLog("viewport:paintGL:flush-done");

        PaintTextLabels();
    }

    void NullViewportWidget::PaintTextLabels()
    {
        const auto& labels = m_debugDisplay.TextLabels();
        if (labels.empty())
        {
            return;
        }

        // Cull labels behind the camera before projecting: WorldToScreen has no depth sign, so a
        // point behind the camera would otherwise fold to a bogus on-screen position.
        const AZ::Vector3 camPos = m_cameraState.m_position;
        const AZ::Vector3 camForward = m_cameraState.m_forward;
        const float dpr = aznumeric_cast<float>(devicePixelRatioF());

        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        QFont font = painter.font();
        font.setPointSizeF(9.0);
        painter.setFont(font);

        for (const DebugTextLabel& label : labels)
        {
            if ((label.m_worldPosition - camPos).Dot(camForward) <= 0.0f)
            {
                continue;
            }
            const AzFramework::ScreenPoint sp = AzFramework::WorldToScreen(label.m_worldPosition, m_cameraState);
            const QPointF pos(sp.m_x / dpr, sp.m_y / dpr);

            const QColor color = QColor::fromRgbF(
                label.m_color.GetR(), label.m_color.GetG(), label.m_color.GetB(), label.m_color.GetA());
            const QString text = QString::fromUtf8(label.m_text.c_str());

            QPointF drawPos = pos;
            if (label.m_center)
            {
                const QRectF bounds = painter.fontMetrics().boundingRect(text);
                drawPos.rx() -= bounds.width() * 0.5;
                drawPos.ry() += bounds.height() * 0.5;
            }

            // 1px dark shadow for legibility over both light and dark geometry.
            painter.setPen(QColor(0, 0, 0, 180));
            painter.drawText(drawPos + QPointF(1.0, 1.0), text);
            painter.setPen(color);
            painter.drawText(drawPos, text);
        }
    }

    AzFramework::ScreenSize NullViewportWidget::ViewportSize() const
    {
        return AzFramework::ScreenSize(AZStd::max(width(), 1), AZStd::max(height(), 1));
    }

    void NullViewportWidget::UpdateCameraState()
    {
        // Advance the reusable AzFramework camera controller (orbit/pan/dolly/fly) and take
        // the resulting engine-neutral camera state for rendering and picking.
        m_cameraState = m_cameraController.StepCamera(ViewportSize(), 1.0f / 60.0f);
    }

    AzFramework::CameraState NullViewportWidget::GetCameraState()
    {
        return m_cameraState;
    }

    AzFramework::ScreenPoint NullViewportWidget::ViewportWorldToScreen(const AZ::Vector3& worldPosition)
    {
        return AzFramework::WorldToScreen(worldPosition, m_cameraState);
    }

    AZ::Vector3 NullViewportWidget::ViewportScreenToWorld(const AzFramework::ScreenPoint& screenPosition)
    {
        return AzFramework::ScreenToWorld(screenPosition, m_cameraState);
    }

    AzToolsFramework::ViewportInteraction::ProjectedViewportRay NullViewportWidget::ViewportScreenToWorldRay(
        const AzFramework::ScreenPoint& screenPosition)
    {
        return AzToolsFramework::ViewportInteraction::ViewportScreenToWorldRay(m_cameraState, screenPosition);
    }

    float NullViewportWidget::DeviceScalingFactor()
    {
        return aznumeric_cast<float>(devicePixelRatioF());
    }

    void NullViewportWidget::mousePressEvent(QMouseEvent* event)
    {
        using AzToolsFramework::ViewportInteraction::MouseEvent;
        // Camera navigation gets first refusal; if it starts navigating, don't also select.
        if (m_cameraController.HandleMousePress(*event, ViewportSize()))
        {
            update();
            return;
        }
        if (!HandleMouseEvent(
                MouseEvent::Down, event->pos(), event->button(), event->buttons(), event->modifiers(), 0.0f))
        {
            QOpenGLWidget::mousePressEvent(event);
        }
    }

    void NullViewportWidget::mouseReleaseEvent(QMouseEvent* event)
    {
        using AzToolsFramework::ViewportInteraction::MouseEvent;
        const bool cameraHandled = m_cameraController.HandleMouseRelease(*event, ViewportSize());
        if (!HandleMouseEvent(
                MouseEvent::Up, event->pos(), event->button(), event->buttons(), event->modifiers(), 0.0f) &&
            !cameraHandled)
        {
            QOpenGLWidget::mouseReleaseEvent(event);
        }
        update();
    }

    void NullViewportWidget::mouseMoveEvent(QMouseEvent* event)
    {
        using AzToolsFramework::ViewportInteraction::MouseEvent;
        // Feed the move to the camera (updates its cursor delta) and to manipulators for hover.
        m_cameraController.HandleMouseMove(*event, ViewportSize());
        HandleMouseEvent(MouseEvent::Move, event->pos(), Qt::NoButton, event->buttons(), event->modifiers(), 0.0f);
        QOpenGLWidget::mouseMoveEvent(event);
    }

    void NullViewportWidget::mouseDoubleClickEvent(QMouseEvent* event)
    {
        using AzToolsFramework::ViewportInteraction::MouseEvent;
        if (!HandleMouseEvent(
                MouseEvent::DoubleClick, event->pos(), event->button(), event->buttons(), event->modifiers(), 0.0f))
        {
            QOpenGLWidget::mouseDoubleClickEvent(event);
        }
    }

    void NullViewportWidget::wheelEvent(QWheelEvent* event)
    {
        using AzToolsFramework::ViewportInteraction::MouseEvent;
        // Wheel is dolly/zoom for the camera; only fall back to the editor if it declines.
        if (m_cameraController.HandleWheel(*event, ViewportSize()))
        {
            update();
            return;
        }
        const float delta = aznumeric_cast<float>(event->angleDelta().y());
        if (!HandleMouseEvent(
                MouseEvent::Wheel, event->position().toPoint(), Qt::NoButton, event->buttons(), event->modifiers(), delta))
        {
            QOpenGLWidget::wheelEvent(event);
        }
    }

    void NullViewportWidget::keyPressEvent(QKeyEvent* event)
    {
        if (m_cameraController.HandleKey(*event, /*pressed=*/true, ViewportSize()))
        {
            update();
            return;
        }
        QOpenGLWidget::keyPressEvent(event);
    }

    void NullViewportWidget::keyReleaseEvent(QKeyEvent* event)
    {
        if (m_cameraController.HandleKey(*event, /*pressed=*/false, ViewportSize()))
        {
            update();
            return;
        }
        QOpenGLWidget::keyReleaseEvent(event);
    }

    bool NullViewportWidget::HandleMouseEvent(
        AzToolsFramework::ViewportInteraction::MouseEvent mouseEvent, const QPoint& position, Qt::MouseButton eventButton,
        Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers, float /*wheelDelta*/)
    {
        namespace VI = AzToolsFramework::ViewportInteraction;

        // Compose the button mask: Qt's release event no longer lists the released button,
        // so fold it back in to describe which button generated the event.
        Qt::MouseButtons effectiveButtons = buttons;
        if (eventButton != Qt::NoButton)
        {
            effectiveButtons |= eventButton;
        }

        const AzFramework::ScreenPoint screenPoint = VI::ScreenPointFromQPoint(position);
        const VI::MousePick mousePick = VI::BuildMousePick(m_cameraState, screenPoint);
        const VI::InteractionId interactionId(AZ::EntityId(), static_cast<int>(m_viewportId));
        const VI::MouseInteraction mouseInteraction = VI::BuildMouseInteraction(
            mousePick, VI::BuildMouseButtons(effectiveButtons), interactionId, VI::BuildKeyboardModifiers(modifiers));
        const VI::MouseInteractionEvent interactionEvent = VI::BuildMouseInteractionEvent(mouseInteraction, mouseEvent);

        VI::MouseInteractionResult result = VI::MouseInteractionResult::None;
        AzToolsFramework::EditorInteractionSystemViewportSelectionRequestBus::EventResult(
            result, AzToolsFramework::GetEntityContextId(),
            &AzToolsFramework::ViewportInteraction::InternalMouseViewportRequests::InternalHandleAllMouseInteractions,
            interactionEvent);

        update();
        return result != VI::MouseInteractionResult::None;
    }
} // namespace CrossEngineEditor
