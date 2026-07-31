/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/EditorViewportWidget.h>
#include <Viewport/EngineViewport.h>
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
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    //! Transparent 2D overlay painted on top of the native GPU surface for world-space text
    //! labels (numeric drag readouts). The native swapchain owns the pixels underneath, so this
    //! widget must not paint any background - only the text - and must let mouse events fall
    //! through to the surface below (WA_TransparentForMouseEvents).
    class ViewportOverlayLabels final : public QWidget
    {
    public:
        explicit ViewportOverlayLabels(QWidget* parent)
            : QWidget(parent)
        {
            setAttribute(Qt::WA_TransparentForMouseEvents, true);
            setAttribute(Qt::WA_NoSystemBackground, true);
            setAttribute(Qt::WA_TranslucentBackground, true);
            setAttribute(Qt::WA_AlwaysStackOnTop, true);
        }

        void SetLabels(AZStd::vector<DebugTextLabel> labels, const AzFramework::CameraState& camera, qreal pixelRatio)
        {
            m_labels = AZStd::move(labels);
            m_camera = camera;
            m_pixelRatio = pixelRatio;
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            if (m_labels.empty())
            {
                return;
            }

            const AZ::Vector3 camPos = m_camera.m_position;
            const AZ::Vector3 camForward = m_camera.m_forward;
            const float dpr = m_pixelRatio > 0.0 ? aznumeric_cast<float>(m_pixelRatio) : 1.0f;

            QPainter painter(this);
            painter.setRenderHint(QPainter::TextAntialiasing, true);
            QFont font = painter.font();
            font.setPointSizeF(9.0);
            painter.setFont(font);

            for (const DebugTextLabel& label : m_labels)
            {
                // Cull labels behind the camera: WorldToScreen has no depth sign, so a point
                // behind the camera would otherwise fold to a bogus on-screen position.
                if ((label.m_worldPosition - camPos).Dot(camForward) <= 0.0f)
                {
                    continue;
                }

                const AzFramework::ScreenPoint sp = AzFramework::WorldToScreen(label.m_worldPosition, m_camera);
                const QPointF pos(sp.m_x / dpr, sp.m_y / dpr);

                const QColor color =
                    QColor::fromRgbF(label.m_color.GetR(), label.m_color.GetG(), label.m_color.GetB(), label.m_color.GetA());
                const QString text = QString::fromUtf8(label.m_text.c_str());

                QPointF drawPos = pos;
                if (label.m_center)
                {
                    const QRectF bounds = painter.fontMetrics().boundingRect(text);
                    drawPos.rx() -= bounds.width() * 0.5;
                    drawPos.ry() += bounds.height() * 0.5;
                }

                painter.setPen(QColor(0, 0, 0, 180));
                painter.drawText(drawPos + QPointF(1.0, 1.0), text);
                painter.setPen(color);
                painter.drawText(drawPos, text);
            }
        }

    private:
        AZStd::vector<DebugTextLabel> m_labels;
        AzFramework::CameraState m_camera;
        qreal m_pixelRatio = 1.0;
    };

    namespace
    {
        //! Match the native surface type to the RHI the backend presents with. On Windows the
        //! Diligent D3D12 backend needs a Direct3D surface; other platforms use their native RHI.
#if defined(Q_OS_WIN)
        constexpr QSurface::SurfaceType k_viewportSurfaceType = QSurface::Direct3DSurface;
#elif defined(Q_OS_MAC)
        constexpr QSurface::SurfaceType k_viewportSurfaceType = QSurface::MetalSurface;
#else
        constexpr QSurface::SurfaceType k_viewportSurfaceType = QSurface::VulkanSurface;
#endif
    } // namespace

    EditorViewportWidget::EditorViewportWidget(AzFramework::ViewportId viewportId, QWidget* parent)
        : QWidget(parent)
        , m_viewportId(viewportId)
    {
        setObjectName(QStringLiteral("EditorViewport"));
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        // The native surface fills us completely; Qt must not paint a background over it.
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);

        // Embed the native GPU render surface (QWindow + createWindowContainer). The backend
        // swapchain presents straight into this - no GL context, no backing store.
        m_engineViewport = new EngineViewport(k_viewportSurfaceType, this);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_engineViewport);

        // 2D text overlay sits above the surface. It is a child of THIS widget (not the native
        // container) so Qt composites it over, not under, the swapchain, and is resized manually.
        m_labelOverlay = new ViewportOverlayLabels(this);
        m_labelOverlay->setGeometry(rect());
        m_labelOverlay->raise();

        // Surface lifecycle: create/resize/destroy the backend swapchain in lockstep with the
        // native window (plan §2.5/§2.6/§2.8). AboutToClose is a synchronous DirectConnection so
        // the swapchain is released before the surface is gone.
        connect(m_engineViewport, &EngineViewport::NativeReady, this, &EditorViewportWidget::OnNativeReady);
        connect(m_engineViewport, &EngineViewport::Resized, this, &EditorViewportWidget::OnSurfaceResized);
        connect(m_engineViewport, &EngineViewport::AboutToClose, this, &EditorViewportWidget::OnSurfaceAboutToClose,
            Qt::DirectConnection);
        connect(m_engineViewport, &EngineViewport::InputEvent, this,
            [this](QEvent* event)
            {
                HandleNativeInput(event);
            });
        // Pause the idle tick while the surface is hidden (dock tab / auto-hide): no point
        // presenting to an invisible surface. Resume when it comes back (new_editor_plan §2.14).
        connect(m_engineViewport, &EngineViewport::VisibilityChanged, this,
            [this](bool visible)
            {
                if (!m_frameTimer)
                {
                    return;
                }
                if (visible)
                {
                    if (!m_frameTimer->isActive())
                    {
                        m_frameTimer->start(16);
                    }
                }
                else
                {
                    m_frameTimer->stop();
                }
            });

        UpdateCameraState();
        AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler::BusConnect(m_viewportId);

        // Drive continuous repaint so hover/animation feedback stays live (v1 idle tick). Frames
        // only present once the surface is ready; before that we still step the camera controller.
        m_frameTimer = new QTimer(this);
        // PreciseTimer, not the default CoarseTimer: on Windows the coarse timer snaps to the
        // ~15.6ms system tick, so a 16ms interval jitters between 30 and 60 fps. A precise timer
        // holds a steady cadence for the main-thread present path (v1). See new_editor_plan §2.9.
        m_frameTimer->setTimerType(Qt::PreciseTimer);
        connect(m_frameTimer, &QTimer::timeout, this, &EditorViewportWidget::OnFrameTick);
        m_frameTimer->start(16);
    }

    EditorViewportWidget::~EditorViewportWidget()
    {
        AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler::BusDisconnect();
        // The backend releases its swapchain via OnSurfaceAboutToBeDestroyed (driven by the
        // EngineViewport's AboutToClose during native surface teardown); nothing GL-specific here.
    }

    void EditorViewportWidget::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (m_labelOverlay)
        {
            m_labelOverlay->setGeometry(rect());
        }
    }

    void EditorViewportWidget::OnNativeReady(void* nativeHandle, QSize physicalPx)
    {
        m_physicalSize = physicalPx.isEmpty() ? QSize(1, 1) : physicalPx;
        m_pixelRatio = m_engineViewport ? m_engineViewport->PixelRatio() : 1.0;
        UpdateCameraState();
        if (m_sceneRenderer)
        {
            m_sceneRenderer->OnSurfaceCreated(
                nativeHandle, static_cast<uint32_t>(m_physicalSize.width()), static_cast<uint32_t>(m_physicalSize.height()));
        }
    }

    void EditorViewportWidget::OnSurfaceResized(QSize physicalPx)
    {
        m_physicalSize = physicalPx.isEmpty() ? QSize(1, 1) : physicalPx;
        m_pixelRatio = m_engineViewport ? m_engineViewport->PixelRatio() : 1.0;
        UpdateCameraState();
        if (m_sceneRenderer)
        {
            m_sceneRenderer->OnSurfaceResized(
                static_cast<uint32_t>(m_physicalSize.width()), static_cast<uint32_t>(m_physicalSize.height()));
        }
    }

    void EditorViewportWidget::OnSurfaceAboutToClose()
    {
        if (m_sceneRenderer)
        {
            m_sceneRenderer->OnSurfaceAboutToBeDestroyed();
        }
    }

    void EditorViewportWidget::OnFrameTick()
    {
        // Step the camera each frame so smoothing/inertia and held-key movement advance, even
        // before the surface exists (so the view is settled the moment it comes up).
        UpdateCameraState();

        if (!m_sceneRenderer || !m_sceneRenderer->IsSurfaceReady())
        {
            return;
        }

        // Hand the current camera to the backend so it can set up its world->clip transform,
        // then let AzToolsFramework emit gizmo/selection geometry into GenericDebugDisplay.
        const AZ::Matrix4x4 worldToView = AZ::Matrix4x4::CreateFromMatrix3x4(AzFramework::CameraView(m_cameraState));
        const AZ::Matrix4x4 viewToClip = AzFramework::CameraProjection(m_cameraState);

        m_sceneRenderer->BeginOverlayFrame(worldToView, viewToClip);

        m_debugDisplay.ClearFrame();

        // Editor overlays owned by the shell (grid) draw first, then engine/manipulator overlays.
        m_grid.Draw(m_debugDisplay, m_cameraState);

        const AzFramework::ViewportInfo viewportInfo{ static_cast<int>(m_viewportId) };
        AzFramework::ViewportDebugDisplayEventBus::Event(
            AzToolsFramework::GetEntityContextId(), &AzFramework::ViewportDebugDisplayEvents::DisplayViewport, viewportInfo,
            m_debugDisplay);

        m_debugDisplay.Flush(*m_sceneRenderer);

        // Present the scene + overlay for this camera.
        m_sceneRenderer->EndOverlayFrame();

        UpdateTextLabels();
    }

    void EditorViewportWidget::UpdateTextLabels()
    {
        if (m_labelOverlay)
        {
            m_labelOverlay->SetLabels(m_debugDisplay.TextLabels(), m_cameraState, m_pixelRatio);
        }
    }

    AzFramework::ScreenSize EditorViewportWidget::ViewportSize() const
    {
        // Camera math + picking work in physical pixels (the swapchain size), matching how the
        // manipulators author screen-space sizes against DeviceScalingFactor.
        return AzFramework::ScreenSize(AZStd::max(m_physicalSize.width(), 1), AZStd::max(m_physicalSize.height(), 1));
    }

    void EditorViewportWidget::UpdateCameraState()
    {
        // Advance the reusable AzFramework camera controller (orbit/pan/dolly/fly) and take
        // the resulting engine-neutral camera state for rendering and picking.
        m_cameraState = m_cameraController.StepCamera(ViewportSize(), 1.0f / 60.0f);
    }

    AzFramework::CameraState EditorViewportWidget::GetCameraState()
    {
        return m_cameraState;
    }

    AzFramework::ScreenPoint EditorViewportWidget::ViewportWorldToScreen(const AZ::Vector3& worldPosition)
    {
        return AzFramework::WorldToScreen(worldPosition, m_cameraState);
    }

    AZ::Vector3 EditorViewportWidget::ViewportScreenToWorld(const AzFramework::ScreenPoint& screenPosition)
    {
        return AzFramework::ScreenToWorld(screenPosition, m_cameraState);
    }

    AzToolsFramework::ViewportInteraction::ProjectedViewportRay EditorViewportWidget::ViewportScreenToWorldRay(
        const AzFramework::ScreenPoint& screenPosition)
    {
        return AzToolsFramework::ViewportInteraction::ViewportScreenToWorldRay(m_cameraState, screenPosition);
    }

    float EditorViewportWidget::DeviceScalingFactor()
    {
        return m_pixelRatio > 0.0 ? aznumeric_cast<float>(m_pixelRatio) : 1.0f;
    }

    AzFramework::ScreenPoint EditorViewportWidget::ToPhysicalScreenPoint(const QPointF& logicalPos) const
    {
        // Native window input arrives in the window's logical pixels; the camera/picking math is
        // in physical pixels, so scale by the device pixel ratio.
        const float dpr = m_pixelRatio > 0.0 ? aznumeric_cast<float>(m_pixelRatio) : 1.0f;
        return AzFramework::ScreenPoint(
            static_cast<int>(logicalPos.x() * dpr + 0.5f), static_cast<int>(logicalPos.y() * dpr + 0.5f));
    }

    void EditorViewportWidget::HandleNativeInput(QEvent* event)
    {
        using AzToolsFramework::ViewportInteraction::MouseEvent;

        switch (event->type())
        {
        case QEvent::MouseButtonPress:
            {
                auto* me = static_cast<QMouseEvent*>(event);
                if (m_cameraController.HandleMousePress(*me, ViewportSize()))
                {
                    break;
                }
                HandleMouseEvent(
                    MouseEvent::Down, me->position().toPoint(), me->button(), me->buttons(), me->modifiers(), 0.0f);
                break;
            }
        case QEvent::MouseButtonRelease:
            {
                auto* me = static_cast<QMouseEvent*>(event);
                m_cameraController.HandleMouseRelease(*me, ViewportSize());
                HandleMouseEvent(
                    MouseEvent::Up, me->position().toPoint(), me->button(), me->buttons(), me->modifiers(), 0.0f);
                break;
            }
        case QEvent::MouseMove:
            {
                auto* me = static_cast<QMouseEvent*>(event);
                m_cameraController.HandleMouseMove(*me, ViewportSize());
                HandleMouseEvent(
                    MouseEvent::Move, me->position().toPoint(), Qt::NoButton, me->buttons(), me->modifiers(), 0.0f);
                break;
            }
        case QEvent::MouseButtonDblClick:
            {
                auto* me = static_cast<QMouseEvent*>(event);
                HandleMouseEvent(
                    MouseEvent::DoubleClick, me->position().toPoint(), me->button(), me->buttons(), me->modifiers(), 0.0f);
                break;
            }
        case QEvent::Wheel:
            {
                auto* we = static_cast<QWheelEvent*>(event);
                if (m_cameraController.HandleWheel(*we, ViewportSize()))
                {
                    break;
                }
                const float delta = aznumeric_cast<float>(we->angleDelta().y());
                HandleMouseEvent(
                    MouseEvent::Wheel, we->position().toPoint(), Qt::NoButton, we->buttons(), we->modifiers(), delta);
                break;
            }
        case QEvent::KeyPress:
            {
                auto* ke = static_cast<QKeyEvent*>(event);
                m_cameraController.HandleKey(*ke, /*pressed=*/true, ViewportSize());
                break;
            }
        case QEvent::KeyRelease:
            {
                auto* ke = static_cast<QKeyEvent*>(event);
                m_cameraController.HandleKey(*ke, /*pressed=*/false, ViewportSize());
                break;
            }
        // Drag & drop is forwarded from the native QWindow (EngineViewportWindow::event, §2.15).
        // Accept the drag phases so Qt permits a drop over the surface, then dispatch the drop.
        case QEvent::DragEnter:
            {
                auto* de = static_cast<QDragEnterEvent*>(event);
                if (de->mimeData() != nullptr)
                {
                    de->acceptProposedAction();
                }
                break;
            }
        case QEvent::DragMove:
            {
                static_cast<QDragMoveEvent*>(event)->acceptProposedAction();
                break;
            }
        case QEvent::Drop:
            {
                auto* dropEvent = static_cast<QDropEvent*>(event);
                HandleAssetDrop(dropEvent);
                dropEvent->acceptProposedAction();
                break;
            }
        default:
            break;
        }
    }

    void EditorViewportWidget::HandleAssetDrop(QDropEvent* dropEvent)
    {
        // v1: the asset instantiation pipeline is not wired yet, so translate the drop point to a
        // world-space ray (the natural spawn location) and hand it out for backends to consume.
        // Keeping the mechanics correct here means the drop lands instead of being silently
        // swallowed by the native QWindow; wiring an actual spawn is a follow-up.
        const AzFramework::ScreenPoint screenPoint = ToPhysicalScreenPoint(dropEvent->position());
        const AZ::Vector3 worldPosition = AzFramework::ScreenToWorld(screenPoint, m_cameraState);
        if (m_sceneRenderer != nullptr && dropEvent->mimeData() != nullptr)
        {
            // Placeholder hook: backends that support asset spawning can observe this later.
            AZ_UNUSED(worldPosition);
        }
    }

    bool EditorViewportWidget::HandleMouseEvent(
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

        const AzFramework::ScreenPoint screenPoint = ToPhysicalScreenPoint(QPointF(position));
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

        return result != VI::MouseInteractionResult::None;
    }
} // namespace CrossEngineEditor
