/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/CrossEngineViewportSelection.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ComponentEntitySelectionBus.h>
#include <AzToolsFramework/API/ViewportEditorModeTrackerInterface.h>
#include <AzToolsFramework/Manipulators/ManipulatorManager.h>
#include <AzToolsFramework/Undo/UndoSystem.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>
#include <AzToolsFramework/ViewportSelection/EditorVisibleEntityDataCache.h>

#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/limits.h>

#include <AzFramework/Viewport/ViewportScreen.h>

#include <Profiling/CrossEngineProfiler.h>

namespace CrossEngineEditor
{
    using AzToolsFramework::GetCameraState;
    using AzToolsFramework::ManipulatorManager;
    using AzToolsFramework::ToolsApplicationRequestBus;
    namespace VI = AzToolsFramework::ViewportInteraction;

    CrossEngineViewportSelection::CrossEngineViewportSelection(
        const AzToolsFramework::EditorVisibleEntityDataCacheInterface* entityDataCache,
        AzToolsFramework::ViewportEditorModeTrackerInterface* viewportEditorModeTracker,
        GizmoStyle style)
        : m_entityDataCache(entityDataCache)
        , m_viewportEditorModeTracker(viewportEditorModeTracker)
        , m_editorHelpers(AZStd::make_unique<AzToolsFramework::EditorHelpers>(entityDataCache))
        , m_manipulatorManager(AZStd::make_shared<ManipulatorManager>(AzToolsFramework::GetMainManipulatorManagerId()))
        , m_componentModeCollection(viewportEditorModeTracker)
    {
        // Register the (empty) ComponentMode collection exactly where the stock
        // EditorDefaultSelection does: this class REPLACED that handler, so it inherits this
        // duty - framework action handlers query the interface at registration time and
        // null-deref without it (see the header comment).
        AZ::Interface<AzToolsFramework::ComponentModeCollectionInterface>::Register(&m_componentModeCollection);

        m_gizmoManager = AZStd::make_unique<GizmoManager>(AzToolsFramework::GetMainManipulatorManagerId(), style);

        // Box select (editor_polish.md P2 / D3): the state machine calls back on release; the
        // commit applies the industry-standard modifier semantics over the projected bounds.
        m_boxSelect.InstallLeftMouseUp([this]
        {
            CommitBoxSelect();
        });

        // Activate the Default viewport editor mode for our editor entity context, exactly as the
        // stock EditorDefaultSelection does. We replaced that handler, so without this the tracker
        // has no entry for this context: any framework query of GetViewportEditorModes(context)
        // returns nullptr, and consumers like ContainerEntitySystemComponent::FindHighestSelectableEntity
        // dereference it and crash on the first pick.
        if (m_viewportEditorModeTracker)
        {
            m_viewportEditorModeTracker->ActivateMode(
                { AzToolsFramework::GetEntityContextId() }, AzToolsFramework::ViewportEditorMode::Default);
        }
    }

    CrossEngineViewportSelection::~CrossEngineViewportSelection()
    {
        if (m_viewportEditorModeTracker)
        {
            m_viewportEditorModeTracker->DeactivateMode(
                { AzToolsFramework::GetEntityContextId() }, AzToolsFramework::ViewportEditorMode::Default);
        }

        AZ::Interface<AzToolsFramework::ComponentModeCollectionInterface>::Unregister(&m_componentModeCollection);

        // Destroy the gizmo manager (unregisters manipulators) before the manager it registered with.
        m_gizmoManager.reset();
        m_manipulatorManager.reset();
    }

    bool CrossEngineViewportSelection::InternalHandleMouseManipulatorInteraction(
        const VI::MouseInteractionEvent& mouseInteractionEvent)
    {
        CEE_PROFILE_FUNCTION();
        const auto& mouseInteraction = mouseInteractionEvent.m_mouseInteraction;
        m_currentInteraction = mouseInteraction; // cached for DrawManipulators.

        switch (mouseInteractionEvent.m_mouseEvent)
        {
        case VI::MouseEvent::Down:
            return m_manipulatorManager->ConsumeViewportMousePress(mouseInteraction);
        case VI::MouseEvent::Move:
            return m_manipulatorManager->ConsumeViewportMouseMove(mouseInteraction) ==
                ManipulatorManager::ConsumeMouseMoveResult::Interacting;
        case VI::MouseEvent::Up:
            return m_manipulatorManager->ConsumeViewportMouseRelease(mouseInteraction);
        case VI::MouseEvent::Wheel:
            return m_manipulatorManager->ConsumeViewportMouseWheel(mouseInteraction);
        default:
            return false;
        }
    }

    bool CrossEngineViewportSelection::InternalHandleMouseViewportInteraction(const VI::MouseInteractionEvent& mouseInteractionEvent)
    {
        CEE_PROFILE_FUNCTION();
        const auto& mouseInteraction = mouseInteractionEvent.m_mouseInteraction;
        const int viewportId = mouseInteraction.m_interactionId.m_viewportId;
        const AzFramework::CameraState cameraState = GetCameraState(viewportId);

        // Viewport context menu (editor_polish.md P0-5): the same public update helper the
        // native EditorTransformComponentSelection runs (its cpp:1896) - records the RMB press
        // and opens the registered menu on a click-without-drag (threshold-guarded, so an
        // orbit drag never pops a menu). Our route-B selection handler replaced the native one,
        // which is where this trigger was lost; EditorViewportWidget routes RMB presses here
        // even while the camera consumes them, so the down/up pair is always seen.
        AzToolsFramework::EditorContextMenuUpdate(m_contextMenu, mouseInteractionEvent);

        // Box select (editor_polish.md P2 / D3): the stock state machine swallows every event
        // from the moment a left DRAG starts (a plain click never activates it, so single picks
        // still fall through to the code below). Runs exactly where the native runs it.
        if (m_boxSelect.HandleMouseInteraction(mouseInteractionEvent))
        {
            return true;
        }

        // Left click selects the entity under the cursor (Ctrl adds/removes from the selection);
        // a click on empty space clears the selection. Manipulator drags are intercepted earlier.
        if (mouseInteraction.m_mouseButtons.Left() && mouseInteractionEvent.m_mouseEvent == VI::MouseEvent::Down)
        {
            const AZ::EntityId clicked =
                m_editorHelpers->FindEntityIdUnderCursor(cameraState, mouseInteractionEvent).ContainerAncestorEntityId();

            AzToolsFramework::EntityIdList selection;
            if (mouseInteraction.m_keyboardModifiers.Ctrl())
            {
                ToolsApplicationRequestBus::BroadcastResult(selection, &ToolsApplicationRequestBus::Events::GetSelectedEntities);
            }

            if (clicked.IsValid())
            {
                auto existing = AZStd::find(selection.begin(), selection.end(), clicked);
                if (existing != selection.end())
                {
                    selection.erase(existing); // Ctrl-click an already selected entity deselects it.
                }
                else
                {
                    selection.push_back(clicked);
                }
            }

            ToolsApplicationRequestBus::Broadcast(&ToolsApplicationRequestBus::Events::SetSelectedEntities, selection);
            return true;
        }

        return false;
    }

    void CrossEngineViewportSelection::DisplayViewportSelection(
        const AzFramework::ViewportInfo& viewportInfo, AzFramework::DebugDisplayRequests& debugDisplay)
    {
        CEE_PROFILE_FUNCTION();
        const AzFramework::CameraState cameraState = GetCameraState(viewportInfo.m_viewportId);

        // Entity icons / shape visualizations (engine-agnostic helper).
        m_editorHelpers->DisplayHelpers(
            viewportInfo, cameraState, debugDisplay, [](AZ::EntityId) { return true; });

        // Selection outline: a world-space wire box around each selected entity, from the SAME
        // source the picker uses (CalculateEditorEntitySelectionBounds -> EngineNodeComponent ->
        // backend GetWorldBounds), so the highlighted box and the clickable region always match.
        // Drawn depth-tested (occlusion correct, matching rbfx Editor's AddBoundingBox) BEFORE the
        // depth-off gizmo block below, so handles still draw in front of the outline.
        {
            AzToolsFramework::EntityIdList selectedEntities;
            ToolsApplicationRequestBus::BroadcastResult(
                selectedEntities, &ToolsApplicationRequestBus::Events::GetSelectedEntities);

            if (!selectedEntities.empty())
            {
                debugDisplay.DepthTestOn();
                debugDisplay.SetColor(AZ::Color(1.0f, 0.6f, 0.15f, 1.0f)); // selection orange (Blender/Godot/Unity).
                for (const AZ::EntityId entityId : selectedEntities)
                {
                    const AZ::Aabb bounds =
                        AzToolsFramework::CalculateEditorEntitySelectionBounds(entityId, viewportInfo);
                    if (bounds.IsValid())
                    {
                        debugDisplay.DrawWireBox(bounds.GetMin(), bounds.GetMax());
                    }
                }
            }
        }

        // Our themed transform gizmos (and any other registered manipulators). Gizmos are always
        // drawn-in-front (depth test off) so scene geometry never occludes the handles - the standard
        // editor behaviour and the user's requirement. DrawDragFeedback toggles depth internally too;
        // it is balanced there, and we restore depth-test-on afterwards for any later consumer.
        m_currentInteraction.m_keyboardModifiers = VI::QueryKeyboardModifiers();
        debugDisplay.DepthTestOff();
        m_manipulatorManager->DrawManipulators(debugDisplay, cameraState, m_currentInteraction);
        // Gizmo-level overlays drawn once per frame (rotate-mode trackball view ring, drag feedback).
        m_gizmoManager->DrawOverlay(debugDisplay, cameraState);
        debugDisplay.DepthTestOn();

        // Box-select marquee (P2): Display2d issues DrawWireQuad2d, which GenericDebugDisplay
        // routes to the Qt overlay painter (this display has no 2D primitives).
        if (m_boxSelect.Active())
        {
            m_boxSelect.Display2d(viewportInfo, debugDisplay);
        }
    }

    void CrossEngineViewportSelection::CommitBoxSelect()
    {
        // Box-select commit (editor_polish.md P2 / D3 design). Judgement is the industry
        // standard bounds-overlap test (UE/Blender/Unity/Godot agree): project each visible
        // entity's SELECTION bounds to screen space and keep entities whose projected rectangle
        // intersects the marquee - unlike the stock position-point test, this selects partially
        // enclosed objects and objects whose origin is outside the box. Semantics: plain =
        // replace selection, Ctrl = add (union), Ctrl+Shift = remove (difference); the result
        // lands as ONE SetSelectedEntities call, i.e. a single undo point.
        const auto region = m_boxSelect.BoxRegion();
        if (!region.has_value() || region->width() < 1 || region->height() < 1)
        {
            return;
        }
        const QRect marquee = region->normalized();

        const int viewportId = m_currentInteraction.m_interactionId.m_viewportId;
        const AzFramework::ViewportInfo viewportInfo{ viewportId };
        const AzFramework::CameraState cameraState = GetCameraState(viewportId);
        const VI::KeyboardModifiers modifiers = m_boxSelect.PreviousModifiers();
        const bool additive = modifiers.Ctrl() && !modifiers.Shift();
        const bool subtractive = modifiers.Ctrl() && modifiers.Shift();

        // Base selection for add/subtract semantics.
        AzToolsFramework::EntityIdList selection;
        if (additive || subtractive)
        {
            ToolsApplicationRequestBus::BroadcastResult(selection, &ToolsApplicationRequestBus::Events::GetSelectedEntities);
        }

        const size_t visibleCount = m_entityDataCache ? m_entityDataCache->VisibleEntityDataCount() : 0;
        for (size_t i = 0; i < visibleCount; ++i)
        {
            const AZ::EntityId entityId = m_entityDataCache->GetVisibleEntityId(i);
            const AZ::Aabb bounds = AzToolsFramework::CalculateEditorEntitySelectionBounds(entityId, viewportInfo);
            if (!bounds.IsValid())
            {
                continue; // nothing selectable on screen (e.g. an unknown non-visual node).
            }

            // Project the bounds corners that are in front of the camera; their min/max extents
            // form the screen rectangle (a corner behind the camera projects mirrored - dropping
            // it keeps the rectangle over the visible part instead of exploding to the screen).
            float minX = AZStd::numeric_limits<float>::max(), minY = AZStd::numeric_limits<float>::max();
            float maxX = AZStd::numeric_limits<float>::lowest(), maxY = AZStd::numeric_limits<float>::lowest();
            bool anyCornerVisible = false;
            for (int corner = 0; corner < 8; ++corner)
            {
                const AZ::Vector3 world(
                    (corner & 1) ? bounds.GetMax().GetX() : bounds.GetMin().GetX(),
                    (corner & 2) ? bounds.GetMax().GetY() : bounds.GetMin().GetY(),
                    (corner & 4) ? bounds.GetMax().GetZ() : bounds.GetMin().GetZ());
                if ((world - cameraState.m_position).Dot(cameraState.m_forward) <= 0.0f)
                {
                    continue;
                }
                const auto screen = AzFramework::WorldToScreen(world, cameraState);
                anyCornerVisible = true;
                minX = AZStd::min(minX, aznumeric_cast<float>(screen.m_x));
                minY = AZStd::min(minY, aznumeric_cast<float>(screen.m_y));
                maxX = AZStd::max(maxX, aznumeric_cast<float>(screen.m_x));
                maxY = AZStd::max(maxY, aznumeric_cast<float>(screen.m_y));
            }
            if (!anyCornerVisible)
            {
                continue;
            }

            const QRect projected(
                static_cast<int>(minX), static_cast<int>(minY),
                static_cast<int>(maxX - minX), static_cast<int>(maxY - minY));
            if (!projected.intersects(marquee))
            {
                continue;
            }

            auto existing = AZStd::find(selection.begin(), selection.end(), entityId);
            if (subtractive && existing != selection.end())
            {
                selection.erase(existing);
            }
            else if (!subtractive && existing == selection.end())
            {
                selection.push_back(entityId);
            }
        }

        AzToolsFramework::ScopedUndoBatch undoBatch("Box Select Entities");
        ToolsApplicationRequestBus::Broadcast(&ToolsApplicationRequestBus::Events::SetSelectedEntities, selection);
    }
} // namespace CrossEngineEditor
