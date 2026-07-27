/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Cross-engine viewport selection handler (plan: route B, §9). Replaces the engine-native
//! EditorDefaultSelection so that ONLY our self-drawn transform gizmos are shown.
//!
//! It keeps the reused framework pieces that are engine-agnostic:
//!   - EditorHelpers        : entity icons / hover highlight / click picking
//!   - ManipulatorManager   : hit-testing / hover / drag dispatch for manipulators
//! and hosts a GizmoManager which owns our themed manipulators. The engine's
//! EditorTransformComponentSelection (which would draw its own gizmos) is intentionally
//! not used.

#if !defined(Q_MOC_RUN)
#include <Viewport/GizmoManager.h>
#include <Viewport/GizmoTheme.h>

#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/ViewportSelection/EditorHelpers.h>
#endif

namespace AzToolsFramework
{
    class EditorVisibleEntityDataCacheInterface;
    class ManipulatorManager;
} // namespace AzToolsFramework

namespace CrossEngineEditor
{
    //! Selection + self-drawn-gizmo viewport handler installed via
    //! EditorInteractionSystemViewportSelectionRequestBus::SetHandler.
    class CrossEngineViewportSelection : public AzToolsFramework::ViewportInteraction::InternalViewportSelectionRequests
    {
    public:
        CrossEngineViewportSelection(
            const AzToolsFramework::EditorVisibleEntityDataCacheInterface* entityDataCache, GizmoStyle style);
        ~CrossEngineViewportSelection() override;

        //! Access the gizmo manager to drive mode / theme switching from the UI.
        GizmoManager& GetGizmoManager()
        {
            return *m_gizmoManager;
        }

        // InternalViewportSelectionRequests / InternalMouseViewportRequests ...
        bool InternalHandleMouseViewportInteraction(
            const AzToolsFramework::ViewportInteraction::MouseInteractionEvent& mouseInteraction) override;
        bool InternalHandleMouseManipulatorInteraction(
            const AzToolsFramework::ViewportInteraction::MouseInteractionEvent& mouseInteraction) override;
        void DisplayViewportSelection(
            const AzFramework::ViewportInfo& viewportInfo, AzFramework::DebugDisplayRequests& debugDisplay) override;

    private:
        const AzToolsFramework::EditorVisibleEntityDataCacheInterface* m_entityDataCache = nullptr;
        AZStd::unique_ptr<AzToolsFramework::EditorHelpers> m_editorHelpers;
        AZStd::shared_ptr<AzToolsFramework::ManipulatorManager> m_manipulatorManager;
        AZStd::unique_ptr<GizmoManager> m_gizmoManager;

        AZ::EntityId m_hoveredEntityId; //!< Entity currently highlighted under the cursor.
        AzToolsFramework::ViewportInteraction::MouseInteraction m_currentInteraction; //!< For drawing manipulators.
    };
} // namespace CrossEngineEditor
