/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/EditorViewportWidget.h>
#include <Viewport/EngineViewport.h>
#include <Application/EntityMirrorBridge.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <BackendAPI/ISceneRenderer.h>

#include <AzCore/Component/TransformBus.h>
#include <AzCore/Interface/Interface.h>
#include <Framework/EngineNodeComponent.h>

#include <Profiling/CrossEngineProfiler.h>

#include <AzCore/Component/Entity.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzFramework/Viewport/ScreenGeometry.h>
#include <AzFramework/Viewport/ViewportScreen.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/Viewport/ViewportTypes.h>
#include <AzToolsFramework/ViewportSelection/EditorInteractionSystemViewportSelectionRequestBus.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
AZ_POP_DISABLE_WARNING

namespace
{
    // Asset-drop whitelist (migration P0/P1, Plan §B8): only extensions the engine can actually
    // use. .mdl = rbfx Model, .xml = scene/prefab XML (CreateObject tries Model first, then
    // XMLFile); .mat/.ani assign to the current selection instead of spawning (migration L1).
    // Everything else is ignored at DragEnter so the drop cursor stays honest.
    bool IsWhitelistedAssetPath(const QString& path)
    {
        return path.endsWith(QStringLiteral(".mdl"), Qt::CaseInsensitive)
            || path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
            || path.endsWith(QStringLiteral(".mat"), Qt::CaseInsensitive)
            || path.endsWith(QStringLiteral(".material"), Qt::CaseInsensitive)
            || path.endsWith(QStringLiteral(".ani"), Qt::CaseInsensitive);
    }

    //! Spawn distance when the drop ray hits nothing (empty sky): a fixed distance along the
    //! ray from the camera, so the asset still lands somewhere sensible in view.
    constexpr float k_spawnFallbackDistance = 10.0f;
} // namespace

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

        void SetLabels(
            AZStd::vector<DebugTextLabel> labels, const AzFramework::CameraState& camera, qreal pixelRatio,
            const AZStd::string& headerText)
        {
            m_labels = AZStd::move(labels);
            m_camera = camera;
            m_pixelRatio = pixelRatio;
            m_headerText = QString::fromUtf8(headerText.c_str());
            update();
        }

        void SetMarquee(const GenericDebugDisplay::Marquee& marquee)
        {
            m_marquee = marquee;
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            // Box-select marquee (editor_polish.md P2): normalized screen rect collected by
            // GenericDebugDisplay::DrawWireQuad2d this frame, painted here - the display has no
            // 2D primitives, so the Qt overlay path is the painter.
            if (m_marquee.m_valid)
            {
                QPainter painter(this);
                const QRectF rect(
                    QPointF(m_marquee.m_minX * width(), m_marquee.m_minY * height()),
                    QPointF(m_marquee.m_maxX * width(), m_marquee.m_maxY * height()));
                QColor fill(
                    aznumeric_cast<int>(m_marquee.m_color.GetR() * 255), aznumeric_cast<int>(m_marquee.m_color.GetG() * 255),
                    aznumeric_cast<int>(m_marquee.m_color.GetB() * 255),
                    aznumeric_cast<int>(m_marquee.m_color.GetA() * 63));
                painter.fillRect(rect, fill);
                QPen pen(QColor(
                    aznumeric_cast<int>(m_marquee.m_color.GetR() * 255),
                    aznumeric_cast<int>(m_marquee.m_color.GetG() * 255),
                    aznumeric_cast<int>(m_marquee.m_color.GetB() * 255)));
                pen.setWidthF(m_marquee.m_widthPx);
                painter.setPen(pen);
                painter.drawRect(rect);
            }

            if (m_labels.empty() && m_headerText.isEmpty())
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

            // Blender area-header status text: a single line at the top-left of the viewport, white
            // on a dark strip, shown during a drag (ED_area_status_text).
            if (!m_headerText.isEmpty())
            {
                const QRectF hb = painter.fontMetrics().boundingRect(m_headerText);
                const qreal pad = 6.0;
                const QRectF strip(0.0, 0.0, width(), hb.height() + 2.0 * pad);
                painter.fillRect(strip, QColor(30, 30, 30, 200));
                painter.setPen(QColor(230, 230, 230));
                painter.drawText(QPointF(pad, hb.height() + pad * 0.5), m_headerText);
            }

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
                const QRectF bounds = painter.fontMetrics().boundingRect(text);
                if (label.m_center)
                {
                    drawPos.rx() -= bounds.width() * 0.5;
                    drawPos.ry() += bounds.height() * 0.5;
                }

                if (label.m_backgroundBlock)
                {
                    // Unreal rotation HUD: white text on a semi-transparent black block, 5px margin
                    // (UnrealWidgetRender.cpp DrawHUD). The text baseline is at drawPos.y().
                    const qreal margin = 5.0;
                    const QRectF block(
                        drawPos.x() - margin, drawPos.y() - bounds.height() - margin + painter.fontMetrics().descent(),
                        bounds.width() + 2.0 * margin, bounds.height() + 2.0 * margin);
                    painter.fillRect(block, QColor(0, 0, 0, 64)); // FLinearColor(0,0,0,0.25).
                    painter.setPen(color);
                    painter.drawText(drawPos, text);
                }
                else
                {
                    painter.setPen(QColor(0, 0, 0, 180));
                    painter.drawText(drawPos + QPointF(1.0, 1.0), text);
                    painter.setPen(color);
                    painter.drawText(drawPos, text);
                }
            }
        }

    private:
        AZStd::vector<DebugTextLabel> m_labels;
        AzFramework::CameraState m_camera;
        qreal m_pixelRatio = 1.0;
        QString m_headerText;
        GenericDebugDisplay::Marquee m_marquee;
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

    EditorViewportWidget::EditorViewportWidget(
        AzFramework::ViewportId viewportId, EntityMirrorBridge* mirrorBridge, QWidget* parent)
        : QWidget(parent)
        , m_viewportId(viewportId)
        , m_mirrorBridge(mirrorBridge)
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
        // native window (Plan §B2). AboutToClose is a synchronous DirectConnection so
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
        // Track surface visibility so TickRender can skip presenting to a hidden surface (dock
        // tab / auto-hide, Plan §B3). The single loop keeps calling TickRender either way; we just
        // gate the actual draw+present. Replaces the old QTimer start/stop.
        connect(m_engineViewport, &EngineViewport::VisibilityChanged, this,
            [this](bool visible)
            {
                m_surfaceVisible = visible;
            });

        UpdateCameraState();
        AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler::BusConnect(m_viewportId);
        AzToolsFramework::ViewportInteraction::EditorEntityViewportInteractionRequestBus::Handler::BusConnect(m_viewportId);

        // Register as THE viewport render step for the editor's single main loop; OnIdle calls
        // TickRender() once per throttled frame (see IViewportTick.h). Replaces the old render QTimer.
        AZ::Interface<IViewportTick>::Register(this);
    }

    EditorViewportWidget::~EditorViewportWidget()
    {
        // Stop being driven by the single loop before anything else tears down.
        AZ::Interface<IViewportTick>::Unregister(this);
        AzToolsFramework::ViewportInteraction::EditorEntityViewportInteractionRequestBus::Handler::BusDisconnect();
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

    void EditorViewportWidget::TickRender(float deltaSeconds)
    {
        CEE_PROFILE_FUNCTION();
        // Apply the frame's coalesced mouse-move once (camera + hover) before stepping the camera,
        // so orbit navigation reflects the latest cursor without per-message flooding.
        ApplyPendingMouseMove();

        // Step the camera each frame so smoothing/inertia and held-key movement advance, even
        // before the surface exists (so the view is settled the moment it comes up).
        UpdateCameraState(deltaSeconds);

        // Skip the draw+present entirely when the surface is missing, not ready, or hidden
        // (dock tab / auto-hide). The single loop still called us so the camera stayed live.
        if (!m_sceneRenderer || !m_sceneRenderer->IsSurfaceReady() || !m_surfaceVisible)
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

        m_debugDisplay.Flush(*m_sceneRenderer, m_cameraState);

        // Present the scene + overlay for this camera.
        m_sceneRenderer->EndOverlayFrame();

        UpdateTextLabels();
    }

    void EditorViewportWidget::UpdateTextLabels()
    {
        if (m_labelOverlay)
        {
            m_labelOverlay->SetLabels(m_debugDisplay.TextLabels(), m_cameraState, m_pixelRatio, m_debugDisplay.HeaderText());
            m_labelOverlay->SetMarquee(m_debugDisplay.MarqueeRect());
        }
    }

    AzFramework::ScreenSize EditorViewportWidget::ViewportSize() const
    {
        // Camera math + picking work in physical pixels (the swapchain size), matching how the
        // manipulators author screen-space sizes against DeviceScalingFactor.
        return AzFramework::ScreenSize(AZStd::max(m_physicalSize.width(), 1), AZStd::max(m_physicalSize.height(), 1));
    }

    void EditorViewportWidget::ApplyPendingMouseMove()
    {
        if (!m_hasPendingMove)
        {
            return;
        }
        m_hasPendingMove = false;

        // Synthesize one QMouseEvent from the frame's latest coalesced position and drive the
        // camera + selection hover exactly once (industry-standard per-frame input apply). The
        // camera sees the total delta since the previous frame's position, so orbit is unchanged.
        CEE_PROFILE_SCOPE("Input::ApplyPendingMove");
        QMouseEvent moveEvent(
            QEvent::MouseMove, m_pendingMovePos, m_pendingMovePos, Qt::NoButton, m_pendingMoveButtons,
            m_pendingMoveModifiers);
        m_cameraController.HandleMouseMove(moveEvent, ViewportSize(), DeviceScalingFactor());
        HandleMouseEvent(
            AzToolsFramework::ViewportInteraction::MouseEvent::Move, m_pendingMovePos.toPoint(), Qt::NoButton,
            m_pendingMoveButtons, m_pendingMoveModifiers, 0.0f);
    }

    void EditorViewportWidget::UpdateCameraState(float deltaSeconds)
    {
        // Advance the reusable AzFramework camera controller (orbit/pan/dolly/fly) and take
        // the resulting engine-neutral camera state for rendering and picking. deltaSeconds is the
        // real elapsed time from the single loop (TickRender), so smoothing/inertia track the
        // actual frame cadence instead of an assumed 60 fps.
        if (deltaSeconds <= 0.0f)
        {
            deltaSeconds = 1.0f / 60.0f;
        }
        m_cameraState = m_cameraController.StepCamera(ViewportSize(), deltaSeconds);
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

    void EditorViewportWidget::FindVisibleEntities(AZStd::vector<AZ::EntityId>& visibleEntities)
    {
        CEE_PROFILE_FUNCTION();
        // The picker only tests entities returned here. Hand it every loose mirror entity (see the
        // header for why we skip frustum culling). Filtering to entities that carry an
        // EngineNodeComponent keeps prefab containers and other non-mirror entities out of the set.
        AzToolsFramework::EntityList looseEntities;
        AzToolsFramework::EditorEntityContextRequestBus::Broadcast(
            &AzToolsFramework::EditorEntityContextRequests::GetLooseEditorEntities, looseEntities);

        visibleEntities.reserve(looseEntities.size());
        for (const AZ::Entity* entity : looseEntities)
        {
            if (entity && entity->FindComponent<EngineNodeComponent>() != nullptr)
            {
                visibleEntities.push_back(entity->GetId());
            }
        }
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
        CEE_PROFILE_FUNCTION();
        using AzToolsFramework::ViewportInteraction::MouseEvent;

        // Flush any coalesced move before a discrete event (press/release/wheel/click) so it acts
        // on the latest cursor position - moves are recorded but only applied per frame otherwise.
        if (event->type() != QEvent::MouseMove)
        {
            ApplyPendingMouseMove();
        }

        switch (event->type())
        {
        case QEvent::MouseButtonPress:
            {
                auto* me = static_cast<QMouseEvent*>(event);
                const bool cameraConsumed = m_cameraController.HandleMousePress(*me, ViewportSize());
                // RMB is BOTH the camera look gesture and the context-menu gesture: even when
                // the camera consumed the press, route it to the interaction system as well so
                // EditorContextMenuUpdate (in CrossEngineViewportSelection, editor_polish.md
                // P0-5) sees the down-up pair and can tell click (menu) from drag (orbit).
                // RMB reaches no manipulator (they bind LMB) and no selection code, so this is
                // otherwise a no-op for the selection system.
                if (!cameraConsumed || me->button() == Qt::RightButton)
                {
                    HandleMouseEvent(
                        MouseEvent::Down, me->position().toPoint(), me->button(), me->buttons(), me->modifiers(), 0.0f);
                }
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
                // Coalesce: record only the latest move; TickRender applies it once per frame.
                // This is the fix for the pick-then-orbit stall - the OS floods hundreds of moves
                // per frame during orbit and stepping the camera + hover picking on each one cost
                // 300-490 ms/frame in the pump. Cheap here, applied once in ApplyPendingMouseMove.
                auto* me = static_cast<QMouseEvent*>(event);
                m_pendingMovePos = me->position();
                m_pendingMoveButtons = me->buttons();
                m_pendingMoveModifiers = me->modifiers();
                m_hasPendingMove = true;
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
                // Shortcut upstream bridge (editor_polish.md P0-2): the viewport is a bare
                // QWindow, so Qt never runs the widget ShortcutOverride flow for its keys -
                // registered shortcuts are deaf while it has focus (~all of the time).
                // BridgeShortcutToTarget re-dispatches the key to the main window where Qt
                // regenerates that flow for it. Skipped while the camera is navigating
                // (RMB look / MMB pan) so the fly keys (WASDQE) do not fire gizmo-mode shortcuts.
                if (!m_cameraController.HandlingEvents() && BridgeShortcutToTarget(*ke))
                {
                    break; // consumed by a registered shortcut - not a camera key.
                }
                m_cameraController.HandleKey(*ke, /*pressed=*/true, ViewportSize());
                break;
            }
        case QEvent::KeyRelease:
            {
                auto* ke = static_cast<QKeyEvent*>(event);
                m_cameraController.HandleKey(*ke, /*pressed=*/false, ViewportSize());
                break;
            }
        // Drag & drop is forwarded from the native QWindow (EngineViewportWindow::event, Plan §B2).
        // DragEnter runs the spawn whitelist (Plan §B8): only a local-file drag with a loadable
        // extension is accepted, so Qt never offers the drop for anything else. Both the
        // AssetBrowser drag and Explorer's text/uri-list pack local-file URLs (verified
        // AssetBrowserEntryUtils::ToMimeData, G:\o3de\Code\...\Entries\AssetBrowserEntryUtils.cpp:71).
        case QEvent::DragEnter:
            {
                auto* de = static_cast<QDragEnterEvent*>(event);
                const QMimeData* mime = de->mimeData();
                if (mime != nullptr && !mime->urls().isEmpty()
                    && IsWhitelistedAssetPath(mime->urls().first().toLocalFile()))
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
        // Migration P0: spawn the dropped asset as an engine object (plan §B9). The first local
        // file URL is the single extraction path (see the DragEnter whitelist comment); the
        // extension re-check here is defense in depth for drops routed past DragEnter.
        if (dropEvent->mimeData() == nullptr || dropEvent->mimeData()->urls().isEmpty())
        {
            return;
        }

        const QString assetPath = dropEvent->mimeData()->urls().first().toLocalFile();
        if (!IsWhitelistedAssetPath(assetPath))
        {
            return;
        }

        // Material / animation drops assign to the first selected mirror entity (the rbfx
        // editor's drop-onto-selection flow, migration L1). No engine object is created, so
        // no mirror re-sync is needed.
        if (m_mirrorBridge
            && (assetPath.endsWith(QStringLiteral(".mat"), Qt::CaseInsensitive)
                || assetPath.endsWith(QStringLiteral(".material"), Qt::CaseInsensitive)
                || assetPath.endsWith(QStringLiteral(".ani"), Qt::CaseInsensitive)))
        {
            if (!m_mirrorBridge->AssignAssetToSelection(assetPath.toUtf8().constData()))
            {
                AZ_Warning("CrossEngineEditor", false,
                    "Asset drop: nothing assigned from %s (no selected mirror entity or backend failure).",
                    assetPath.toUtf8().constData());
            }
            return;
        }

        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        // Ground placement: scene-wide precise raycast for the drop point; a miss (empty sky)
        // falls back to a fixed distance along the ray so the drop still lands in view.
        const AzFramework::ScreenPoint screenPoint = ToPhysicalScreenPoint(dropEvent->position());
        const AzToolsFramework::ViewportInteraction::ProjectedViewportRay ray =
            ViewportScreenToWorldRay(screenPoint);
        AZ::Vector3 hitPoint = ray.m_origin + ray.m_direction * k_spawnFallbackDistance;
        AZ::Vector3 hitNormal = AZ::Vector3::CreateAxisZ(); // unused for placement (identity rotation)
        backend->GetEntityMirror().RaycastScene(ray.m_origin, ray.m_direction, hitPoint, hitNormal);

        ObjectSpec spec;
        spec.m_assetPath = assetPath.toUtf8().constData();
        spec.m_transform = AZ::Transform::CreateTranslation(hitPoint);
        backend->GetEntityMirror().CreateObject(spec);

        // The mirror entity for the new object is built on the next full re-sync (rbfx
        // CreateObject deliberately returns an invalid id).
        if (m_mirrorBridge)
        {
            m_mirrorBridge->RefreshFromEngine();
        }
    }

    bool EditorViewportWidget::HandleMouseEvent(
        AzToolsFramework::ViewportInteraction::MouseEvent mouseEvent, const QPoint& position, Qt::MouseButton eventButton,
        Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers, float /*wheelDelta*/)
    {
        CEE_PROFILE_FUNCTION();
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

    bool EditorViewportWidget::BridgeShortcutToTarget(const QKeyEvent& keyEvent)
    {
        if (!m_shortcutBridgeTarget)
        {
            return false;
        }

        // A fresh, NON-spontaneous copy: QApplication::notify only synthesizes the
        // ShortcutOverride for non-spontaneous key events (qt_sendShortcutOverrideEvent). When
        // a shortcut fires the notify chain consumes the copy and sendEvent returns true; when
        // nothing matches the target ignores the key and we return false.
        QKeyEvent copy(
            QEvent::KeyPress, keyEvent.key(), keyEvent.modifiers(), keyEvent.text(), keyEvent.isAutoRepeat(),
            static_cast<quint16>(keyEvent.count()));
        return QApplication::sendEvent(m_shortcutBridgeTarget, &copy);
    }

    void EditorViewportWidget::FrameSelection()
    {
        // "Focus on selection" (editor_polish.md P0-3): fit the selection bounds into the view
        // keeping the current view direction - the standard editor F-key behavior. Bounds come
        // from the same source as picking, so lights / cameras / empty nodes (wireframe
        // extents) frame exactly like meshes.
        AzToolsFramework::EntityIdList selection;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selection, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        if (selection.empty())
        {
            return;
        }

        const AzFramework::ViewportInfo viewportInfo{ static_cast<int>(m_viewportId) };
        AZ::Aabb bounds = AZ::Aabb::CreateNull();
        for (const AZ::EntityId entityId : selection)
        {
            const AZ::Aabb entityBounds = AzToolsFramework::CalculateEditorEntitySelectionBounds(entityId, viewportInfo);
            if (entityBounds.IsValid())
            {
                bounds.AddAabb(entityBounds);
            }
        }
        if (!bounds.IsValid())
        {
            // Selection has no bounds at all (should not happen - wireframe kinds provide one):
            // fall back to framing a small sphere at the first entity's transform so the camera
            // still moves somewhere useful.
            AZ::Vector3 center = AZ::Vector3::CreateZero();
            AZ::TransformBus::EventResult(center, selection.front(), &AZ::TransformBus::Events::GetWorldTranslation);
            bounds = AZ::Aabb::CreateCenterRadius(center, 1.0f);
        }

        m_cameraController.FrameBounds(bounds, m_cameraState);
        UpdateCameraState();
    }
} // namespace CrossEngineEditor
