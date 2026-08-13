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
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>

#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/algorithm.h>

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
    {
        m_gizmoManager = AZStd::make_unique<GizmoManager>(AzToolsFramework::GetMainManipulatorManagerId(), style);

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
    }
} // namespace CrossEngineEditor
