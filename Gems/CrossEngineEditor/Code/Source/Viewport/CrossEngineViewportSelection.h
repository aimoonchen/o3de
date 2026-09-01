/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Cross-engine viewport selection handler (Plan §B7, route B). Replaces the engine-native
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

#include <AzToolsFramework/ComponentMode/ComponentModeCollection.h>
#include <AzToolsFramework/Viewport/EditorContextMenu.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/ViewportSelection/EditorBoxSelect.h>
#include <AzToolsFramework/ViewportSelection/EditorHelpers.h>
#endif

namespace AzToolsFramework
{
    class EditorVisibleEntityDataCacheInterface;
    class ManipulatorManager;
    class ViewportEditorModeTrackerInterface;
} // namespace AzToolsFramework

namespace CrossEngineEditor
{
    //! Selection + self-drawn-gizmo viewport handler installed via
    //! EditorInteractionSystemViewportSelectionRequestBus::SetHandler.
    class CrossEngineViewportSelection : public AzToolsFramework::ViewportInteraction::InternalViewportSelectionRequests
    {
    public:
        CrossEngineViewportSelection(
            const AzToolsFramework::EditorVisibleEntityDataCacheInterface* entityDataCache,
            AzToolsFramework::ViewportEditorModeTrackerInterface* viewportEditorModeTracker,
            GizmoStyle style);
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
        AzToolsFramework::ViewportEditorModeTrackerInterface* m_viewportEditorModeTracker = nullptr;
        AZStd::unique_ptr<AzToolsFramework::EditorHelpers> m_editorHelpers;
        AZStd::shared_ptr<AzToolsFramework::ManipulatorManager> m_manipulatorManager;
        AZStd::unique_ptr<GizmoManager> m_gizmoManager;

        AzToolsFramework::ViewportInteraction::MouseInteraction m_currentInteraction; //!< For drawing manipulators.

        //! Viewport context-menu state (editor_polish.md P0-5): records the RMB press point and
        //! opens the registered "o3de.menu.editor.viewport.context" menu on a click-without-drag.
        AzToolsFramework::EditorContextMenu m_contextMenu;

        //! Box-select state machine (editor_polish.md P2 / D3): the stock EditorBoxSelect with
        //! industry-standard selection semantics on release (bounds-overlap screen test,
        //! replace / Ctrl = add / Ctrl+Shift = subtract, one undo point).
        AzToolsFramework::EditorBoxSelect m_boxSelect;

        //! Box-select commit: evaluate the finished marquee against every visible entity's
        //! screen-projected bounds and apply the modifier semantics in one SetSelectedEntities.
        void CommitBoxSelect();

        //! Empty ComponentMode collection: the stock selection handler owns/registers this
        //! interface, and framework action handlers (e.g. EditorVertexSelection, driven by
        //! LmbrCentral's registration hook) query it in enabled-state callbacks - without a
        //! registered instance those callbacks null-deref once TriggerRegistrationNotifications
        //! runs (editor_polish.md P0-4: powering the registration chain exposed this).
        AzToolsFramework::ComponentModeFramework::ComponentModeCollection m_componentModeCollection;
    };
} // namespace CrossEngineEditor
