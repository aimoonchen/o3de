/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/CrossEngineViewportSelection.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Manipulators/ManipulatorManager.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>

#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/algorithm.h>

namespace CrossEngineEditor
{
    using AzToolsFramework::GetCameraState;
    using AzToolsFramework::ManipulatorManager;
    using AzToolsFramework::ToolsApplicationRequestBus;
    namespace VI = AzToolsFramework::ViewportInteraction;

    CrossEngineViewportSelection::CrossEngineViewportSelection(
        const AzToolsFramework::EditorVisibleEntityDataCacheInterface* entityDataCache, GizmoStyle style)
        : m_entityDataCache(entityDataCache)
        , m_editorHelpers(AZStd::make_unique<AzToolsFramework::EditorHelpers>(entityDataCache))
        , m_manipulatorManager(AZStd::make_shared<ManipulatorManager>(AzToolsFramework::GetMainManipulatorManagerId()))
    {
        m_gizmoManager = AZStd::make_unique<GizmoManager>(AzToolsFramework::GetMainManipulatorManagerId(), style);
    }

    CrossEngineViewportSelection::~CrossEngineViewportSelection()
    {
        // Destroy the gizmo manager (unregisters manipulators) before the manager it registered with.
        m_gizmoManager.reset();
        m_manipulatorManager.reset();
    }

    bool CrossEngineViewportSelection::InternalHandleMouseManipulatorInteraction(
        const VI::MouseInteractionEvent& mouseInteractionEvent)
    {
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
        const AzFramework::CameraState cameraState = GetCameraState(viewportInfo.m_viewportId);

        // Entity icons / shape visualizations (engine-agnostic helper).
        m_editorHelpers->DisplayHelpers(
            viewportInfo, cameraState, debugDisplay, [](AZ::EntityId) { return true; });

        // Our themed transform gizmos (and any other registered manipulators).
        m_currentInteraction.m_keyboardModifiers = VI::QueryKeyboardModifiers();
        m_manipulatorManager->DrawManipulators(debugDisplay, cameraState, m_currentInteraction);

        // Gizmo-level overlays drawn once per frame (rotate-mode trackball view ring).
        m_gizmoManager->DrawOverlay(debugDisplay, cameraState);
    }
} // namespace CrossEngineEditor
