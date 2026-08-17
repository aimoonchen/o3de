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

#include <BackendAPI/IViewportTick.h>

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

namespace CrossEngineEditor
{
    class ISceneRenderer;
    class EngineViewport;
    class ViewportOverlayLabels;
    class EntityMirrorBridge;

    //! Engine-agnostic editor viewport controller (Plan §B2, revised).
    //!
    //! Post-refactor this is a plain QWidget that HOSTS a native GPU surface rather than
    //! owning an OpenGL context: it embeds an EngineViewport (QWindow + createWindowContainer,
    //! see EngineViewport.h) into which the active backend's swapchain presents directly.
    //! No initializeGL/paintGL/resizeGL, no GL context - the reference design lists
    //! "QOpenGLWidget wrapping the engine" as an explicit anti-pattern (Plan §B2).
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
        , public IViewportTick
        , public AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler
        , public AzToolsFramework::ViewportInteraction::EditorEntityViewportInteractionRequestBus::Handler
    {
        Q_OBJECT
    public:
        //! The mirror bridge triggers the full engine -> editor re-sync after an asset drop
        //! spawns an engine object (not owned; owned by the application, which outlives us).
        explicit EditorViewportWidget(
            AzFramework::ViewportId viewportId, EntityMirrorBridge* mirrorBridge, QWidget* parent = nullptr);
        ~EditorViewportWidget() override;

        AzFramework::ViewportId GetViewportId() const { return m_viewportId; }

        // IViewportTick - driven once per frame by the editor's single loop (OnIdle), replacing
        // the old independent render QTimer. Steps the camera, generates overlays and presents
        // exactly one frame. Skips presenting while the surface is hidden.
        void TickRender(float deltaSeconds) override;

        //! Route overlay rendering through the given backend scene renderer (set per Plan §B2).
        //! Passing nullptr detaches and the viewport renders nothing.
        void SetSceneRenderer(ISceneRenderer* renderer) { m_sceneRenderer = renderer; }

        // AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Handler...
        AzFramework::CameraState GetCameraState() override;
        AzFramework::ScreenPoint ViewportWorldToScreen(const AZ::Vector3& worldPosition) override;
        AZ::Vector3 ViewportScreenToWorld(const AzFramework::ScreenPoint& screenPosition) override;
        AzToolsFramework::ViewportInteraction::ProjectedViewportRay ViewportScreenToWorldRay(
            const AzFramework::ScreenPoint& screenPosition) override;
        float DeviceScalingFactor() override;

        // AzToolsFramework::ViewportInteraction::EditorEntityViewportInteractionRequestBus::Handler...
        //! Return the entities the picker may test against this frame. This handler is REQUIRED:
        //! EditorVisibleEntityDataCache broadcasts FindVisibleEntities every frame, and without a
        //! responder the visible-entity cache is empty, so EditorHelpers::FindEntityIdUnderCursor
        //! iterates nothing and clicking never selects. We return every loose mirror entity (those
        //! carrying an EngineNodeComponent) directly, deliberately skipping frustum culling: mirror
        //! counts are modest, picking is low frequency, and this avoids depending on the visibility
        //! octree being populated (its entity insertion is driven by the game entity context, which
        //! this editor-only app does not run). Frustum culling can be layered in later as pure
        //! optimisation without changing this contract.
        void FindVisibleEntities(AZStd::vector<AZ::EntityId>& visibleEntities) override;

    protected:
        // QWidget - keep the 2D text-label overlay sized to the surface.
        void resizeEvent(QResizeEvent* event) override;

    private Q_SLOTS:
        //! Backend swapchain lifecycle, forwarded from the embedded EngineViewport.
        void OnNativeReady(void* nativeHandle, QSize physicalPx);
        void OnSurfaceResized(QSize physicalPx);
        void OnSurfaceAboutToClose();

    private:
        //! Raw input forwarded from the native render window; translated to editor interactions.
        void HandleNativeInput(QEvent* event);

        //! Handle an asset dropped onto the viewport surface (forwarded from the native window,
        //! rbfx_migration.md §3.4). Migration P0: whitelisted assets spawn an engine object at the
        //! drop point (fixed-distance fallback on a miss), then trigger a full mirror re-sync.
        void HandleAssetDrop(QDropEvent* dropEvent);

        //! Current viewport size as an engine-neutral screen size in physical pixels (min 1x1).
        AzFramework::ScreenSize ViewportSize() const;

        //! Advance the camera controller. deltaSeconds is the real wall-clock time since the last
        //! frame (passed by the single loop via TickRender); the default is used by surface
        //! callbacks that just need a settled state, not a timed step.
        void UpdateCameraState(float deltaSeconds = 1.0f / 60.0f);

        //! Refresh the 2D text-label overlay (drag readouts) from the last frame's labels.
        void UpdateTextLabels();

        //! Apply the coalesced mouse-move (if any) once per frame: step the camera and run
        //! selection/manipulator hover for the latest cursor position. Called from TickRender.
        void ApplyPendingMouseMove();

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
        //! Engine <-> editor object sync; the asset-drop spawn calls RefreshFromEngine through it.
        EntityMirrorBridge* m_mirrorBridge = nullptr;

        //! Native GPU render surface embedded in this widget (owns the QWindow).
        EngineViewport* m_engineViewport = nullptr;
        //! Transparent 2D overlay on top of the surface for world-space text labels.
        ViewportOverlayLabels* m_labelOverlay = nullptr;

        //! When false (surface hidden: dock tab / auto-hide), TickRender steps the camera but
        //! skips generating overlays and presenting - no point drawing to an invisible surface.
        //! Replaces the old QTimer start/stop; the single loop keeps calling TickRender regardless.
        bool m_surfaceVisible = true;

        //! Coalesced mouse-move state (industry-standard input flood control). During camera
        //! orbit the OS delivers hundreds of WM_MOUSEMOVE per frame; stepping the camera and
        //! running selection-hover picking on each one is what produced the 300-490 ms pump
        //! stalls. Like Godot (Input::accumulate + flush_buffered_events), O3DE
        //! (MoveX/MoveY -> OnTick) and Blender (INBETWEEN_MOUSEMOVE), we keep each move event
        //! cheap: HandleNativeInput only records the LATEST position/buttons/modifiers here, and
        //! TickRender applies it once per frame. Non-move events stay immediate. The camera sees
        //! the frame's total cursor delta (last position vs previous frame), so orbit precision
        //! is unchanged.
        bool m_hasPendingMove = false;
        QPointF m_pendingMovePos{ 0.0, 0.0 };
        Qt::MouseButtons m_pendingMoveButtons = Qt::NoButton;
        Qt::KeyboardModifiers m_pendingMoveModifiers = Qt::NoModifier;

        //! Cached physical size of the native surface (updated on ready/resize).
        QSize m_physicalSize{ 1, 1 };
        qreal m_pixelRatio = 1.0;
    };
} // namespace CrossEngineEditor
