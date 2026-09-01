/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/CeeActionsHandler.h>
#include <Window/CeeActionIds.h>
#include <Window/EditorMainWindow.h>

#include <AzCore/Interface/Interface.h>

#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/ActionManager/HotKey/HotKeyManagerInterface.h>
#include <AzToolsFramework/ActionManager/Menu/MenuManagerInterface.h>
#include <AzToolsFramework/Editor/ActionManagerIdentifiers/EditorContextIdentifiers.h>
#include <AzToolsFramework/Editor/ActionManagerIdentifiers/EditorMenuIdentifiers.h>

#include <BackendAPI/IEngineBackend.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QDesktopServices>
#include <QUrl>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    namespace
    {
        // CEE action ids (editor_polish.md D2b / S4): all cee.action.* - CEE has no o3de.action.*
        // collisions, and engine-registered actions arrive as cee.action.<engine>.* via the
        // backend seam (M1). Ids shared with EditorMainWindow live in CeeActionIds.h.
        constexpr AZStd::string_view k_entityCreate = "cee.action.entity.create";
        constexpr AZStd::string_view k_entityCut = "cee.action.entity.cut";
        constexpr AZStd::string_view k_entityCopy = "cee.action.entity.copy";
        constexpr AZStd::string_view k_entityPaste = "cee.action.entity.paste";
        constexpr AZStd::string_view k_entityDuplicate = "cee.action.entity.duplicate";
        constexpr AZStd::string_view k_entityDelete = "cee.action.entity.delete";
        constexpr AZStd::string_view k_focusSelection = "cee.action.view.focusSelection";
        constexpr AZStd::string_view k_workspaceSave = "cee.action.view.saveWorkspace";
        constexpr AZStd::string_view k_workspaceRestore = "cee.action.view.restoreWorkspace";
        constexpr AZStd::string_view k_commandPalette = "cee.action.view.commandPalette";
        constexpr AZStd::string_view k_preferences = "cee.action.edit.preferences";
        constexpr AZStd::string_view k_helpDocs = "cee.action.help.documentation";
        constexpr AZStd::string_view k_helpApi = "cee.action.help.api";
        constexpr AZStd::string_view k_helpGithub = "cee.action.help.github";

        //! Why Undo/Redo are registered but disabled (R1-A): the undo stack is alive but the
        //! engine backends are not command-ified - a re-mirror rebuilds mirror entities and
        //! orphans the history. Until rbfx v1.5 lands: visible, greyed, honest tooltip.
        constexpr const char* k_undoDisabledTooltip =
            "Undo is disabled until engine-side edits become undo commands (rbfx v1.5).";

        AZStd::string ViewPanelActionId(AZStd::string_view dockName)
        {
            return AZStd::string::format("cee.action.view.panel.%.*s",
                aznumeric_cast<int>(dockName.size()), dockName.data());
        }

        AZStd::string ToolsOpenActionId(AZStd::string_view dockName)
        {
            return AZStd::string::format("cee.action.tools.open.%.*s",
                aznumeric_cast<int>(dockName.size()), dockName.data());
        }
    } // namespace

    CeeActionsHandler::CeeActionsHandler(EditorMainWindow* mainWindow)
        : m_mainWindow(mainWindow)
    {
        m_actionManagerInterface = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
        m_hotKeyManagerInterface = AZ::Interface<AzToolsFramework::HotKeyManagerInterface>::Get();
        m_menuManagerInterface = AZ::Interface<AzToolsFramework::MenuManagerInterface>::Get();

        AzToolsFramework::ActionManagerRegistrationNotificationBus::Handler::BusConnect();
        AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusConnect();
    }

    CeeActionsHandler::~CeeActionsHandler()
    {
        AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusDisconnect();
        AzToolsFramework::ActionManagerRegistrationNotificationBus::Handler::BusDisconnect();
    }

    // ------------------------------------------------------------------ helpers

    void CeeActionsHandler::RegisterMainWindowAction(
        const char* actionIdentifier, const char* name, const char* description, AZStd::function<void()> operation,
        const char* hotKey)
    {
        AzToolsFramework::ActionProperties properties;
        properties.m_name = name;
        properties.m_description = description;
        properties.m_category = "Editor";
        properties.m_menuVisibility = AzToolsFramework::ActionVisibility::AlwaysShow;

        m_actionManagerInterface->RegisterAction(
            AZStd::string(EditorIdentifiers::MainWindowActionContextIdentifier), AZStd::string(actionIdentifier),
            properties, AZStd::move(operation));

        if (hotKey != nullptr)
        {
            m_hotKeyManagerInterface->SetActionHotKey(AZStd::string(actionIdentifier), hotKey);
        }
    }

    void CeeActionsHandler::RegisterSelectionSensitiveAction(
        const char* actionIdentifier, const char* name, const char* description, AZStd::function<void()> operation,
        const char* hotKey)
    {
        RegisterMainWindowAction(actionIdentifier, name, description, AZStd::move(operation), hotKey);
        // Enabled = something selected (paste = clipboard instead); evaluated whenever the
        // selection updater fires (selection change + relevant menu aboutToShow) - Ruler 2.
        const bool clipboardBased = AZStd::string_view(actionIdentifier) == k_entityPaste;
        m_actionManagerInterface->InstallEnabledStateCallback(
            AZStd::string(actionIdentifier),
            [clipboardBased]() -> bool
            {
                return clipboardBased ? EditorMainWindow::HasNodeClipboard() : AnyEntitySelected();
            });
        m_actionManagerInterface->AddActionToUpdater(
            AZStd::string(CeeActions::SelectionUpdater), AZStd::string(actionIdentifier));
    }

    void CeeActionsHandler::RegisterCheckableAction(
        const char* actionIdentifier, const char* name, AZStd::function<void()> handler,
        AZStd::function<bool()> checkState, int menuSortKey, const AZStd::string& menuId)
    {
        AzToolsFramework::ActionProperties properties;
        properties.m_name = name;
        properties.m_category = "View";
        properties.m_menuVisibility = AzToolsFramework::ActionVisibility::AlwaysShow;

        m_actionManagerInterface->RegisterCheckableAction(
            AZStd::string(EditorIdentifiers::MainWindowActionContextIdentifier), AZStd::string(actionIdentifier),
            properties, AZStd::move(handler), AZStd::move(checkState));

        m_actionManagerInterface->AddActionToUpdater(AZStd::string(CeeActions::PanelsUpdater), AZStd::string(actionIdentifier));
        m_menuManagerInterface->AddActionToMenu(menuId, AZStd::string(actionIdentifier), menuSortKey);
    }

    bool CeeActionsHandler::AnyEntitySelected()
    {
        AzToolsFramework::EntityIdList selection;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selection, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        return !selection.empty();
    }

    // ------------------------------------------------------------------ hooks

    void CeeActionsHandler::OnActionContextRegistrationHook()
    {
        // Same context set as the native EditorActionsHandler::OnActionContextRegistrationHook.
        // The main window context is the one the shortcut upstream bridge targets
        // (editor_polish.md P0-2); the other three exist so framework widgets that
        // self-assign (EntityPropertyEditor does, EntityPropertyEditor.cpp) resolve.
        {
            AzToolsFramework::ActionContextProperties contextProperties;
            contextProperties.m_name = "Cross-Engine Editor";
            m_actionManagerInterface->RegisterActionContext(
                AZStd::string(EditorIdentifiers::MainWindowActionContextIdentifier), contextProperties);

            // Installing the ActionContextWidgetWatcher on the main window is what makes
            // registered shortcuts fire: the watcher matches ShortcutOverride events that reach
            // the main window (natively they bubble from the focused child; from the viewport
            // they arrive via the upstream bridge).
            m_hotKeyManagerInterface->AssignWidgetToActionContext(
                AZStd::string(EditorIdentifiers::MainWindowActionContextIdentifier), m_mainWindow);
        }

        for (const AZStd::string_view contextId :
             { EditorIdentifiers::EditorAssetBrowserActionContextIdentifier,
               EditorIdentifiers::EditorConsoleActionContextIdentifier,
               EditorIdentifiers::EditorEntityPropertyEditorActionContextIdentifier })
        {
            AzToolsFramework::ActionContextProperties contextProperties;
            contextProperties.m_name = "Cross-Engine Editor";
            m_actionManagerInterface->RegisterActionContext(AZStd::string(contextId), contextProperties);
        }
    }

    void CeeActionsHandler::OnActionUpdaterRegistrationHook()
    {
        m_actionManagerInterface->RegisterActionUpdater(AZStd::string(CeeActions::SelectionUpdater));
        m_actionManagerInterface->RegisterActionUpdater(AZStd::string(CeeActions::RecentUpdater));
        m_actionManagerInterface->RegisterActionUpdater(AZStd::string(CeeActions::PanelsUpdater));
    }

    void CeeActionsHandler::OnActionRegistrationHook()
    {
        const AZStd::string context(EditorIdentifiers::MainWindowActionContextIdentifier);
        EditorMainWindow* mainWindow = m_mainWindow;

        // --- Undo / Redo placeholders (P0-7 / R1-A): registered + hotkeyed + driven through the
        // framework's undo entry points, but held disabled with an honest tooltip until rbfx
        // v1.5. AlwaysShow keeps them visible while disabled.
        struct UndoEntry
        {
            AZStd::string_view m_id;
            const char* m_name;
            const char* m_hotkey;
            void (AzToolsFramework::ToolsApplicationRequests::*m_request)();
        };
        for (const UndoEntry& entry :
             { UndoEntry{ CeeActions::EditUndo, "Undo", "Ctrl+Z", &AzToolsFramework::ToolsApplicationRequests::UndoPressed },
               UndoEntry{ CeeActions::EditRedo, "Redo", "Ctrl+Shift+Z",
                          &AzToolsFramework::ToolsApplicationRequests::RedoPressed } })
        {
            AzToolsFramework::ActionProperties properties;
            properties.m_name = entry.m_name;
            properties.m_description = k_undoDisabledTooltip;
            properties.m_category = "Edit";
            properties.m_menuVisibility = AzToolsFramework::ActionVisibility::AlwaysShow;

            m_actionManagerInterface->RegisterAction(
                context, AZStd::string(entry.m_id), properties,
                [request = entry.m_request]
                {
                    AzToolsFramework::ToolsApplicationRequestBus::Broadcast(request);
                });
            m_actionManagerInterface->InstallEnabledStateCallback(AZStd::string(entry.m_id), [] { return false; });
            m_hotKeyManagerInterface->SetActionHotKey(AZStd::string(entry.m_id), entry.m_hotkey);
        }

        // --- File (P1-12: same operations the handwritten menu drove, hotkeys via HotKeyManager).
        RegisterMainWindowAction(
            CeeActions::FileNew.data(), "New Level", "Create a fresh in-memory level", [mainWindow]
            {
                mainWindow->NewLevel();
            },
            "Ctrl+N");
        RegisterMainWindowAction(
            CeeActions::FileOpen.data(), "Open Level...", "Open a level prefab", [mainWindow]
            {
                mainWindow->OpenLevel();
            },
            "Ctrl+O");
        RegisterMainWindowAction(
            CeeActions::FileSave.data(), "Save Level", "Save the engine scene (or the level prefab as a fallback)",
            [mainWindow]
            {
                mainWindow->SaveLevel();
            },
            "Ctrl+S");

        // --- Preferences (P2 / D9 + E1 hotkey rebinding page).
        RegisterMainWindowAction(
            k_preferences.data(), "Preferences...", "Editor preferences (autosave, viewport, hotkeys)", [mainWindow]
            {
                mainWindow->ShowPreferences();
            },
            "Ctrl+Alt+P");

        // --- Engine-semantic entity pool (P0-5): every entry delegates to the SAME main-window
        // operation (single source of truth). Selection-sensitive entries gray out with no
        // selection (Ruler 2); paste tracks the node clipboard.
        RegisterMainWindowAction(
            k_entityCreate.data(), "Empty Node", "Create an empty engine node and re-sync the mirror", [mainWindow]
            {
                mainWindow->CreateEngineEntity();
            },
            "Ctrl+Shift+N");
        RegisterSelectionSensitiveAction(
            k_entityCut.data(), "Cut", "Cut the selected entities to the node clipboard", [mainWindow]
            {
                mainWindow->CutSelection();
            },
            "Ctrl+X");
        RegisterSelectionSensitiveAction(
            k_entityCopy.data(), "Copy", "Copy the selected entities to the node clipboard", [mainWindow]
            {
                mainWindow->CopySelection();
            },
            "Ctrl+C");
        RegisterSelectionSensitiveAction(
            k_entityPaste.data(), "Paste", "Paste the node clipboard under the first selected entity", [mainWindow]
            {
                mainWindow->PasteSelection();
            },
            "Ctrl+V");
        RegisterSelectionSensitiveAction(
            k_entityDuplicate.data(), "Duplicate", "Duplicate the selected entities (Blender-style)", [mainWindow]
            {
                mainWindow->DuplicateSelection();
            },
            "Ctrl+D");
        RegisterSelectionSensitiveAction(
            k_entityDelete.data(), "Delete", "Delete the selected entities on both sides of the mirror", [mainWindow]
            {
                mainWindow->DeleteSelection();
            },
            "Delete");
        RegisterSelectionSensitiveAction(
            k_focusSelection.data(), "Focus Selection", "Frame the camera on the selected entities", [mainWindow]
            {
                mainWindow->FocusSelection();
            },
            "F");

        // --- View: workspaces / command palette.
        RegisterMainWindowAction(
            k_workspaceSave.data(), "Save Workspace", "Save the current layout as the default workspace",
            [mainWindow]
            {
                mainWindow->SaveWorkspace();
            });
        RegisterMainWindowAction(
            k_workspaceRestore.data(), "Restore Workspace", "Restore the default workspace layout", [mainWindow]
            {
                mainWindow->RestoreWorkspace();
            });
        RegisterMainWindowAction(
            k_commandPalette.data(), "Command Palette", "Fuzzy search and run any registered command", [mainWindow]
            {
                mainWindow->ShowCommandPalette();
            },
            "Ctrl+P");

        // --- Help: external links (P2).
        RegisterMainWindowAction(
            k_helpDocs.data(), "Documentation", "Open the O3DE documentation in a browser", []
            {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://o3de.org/docs/")));
            });
        RegisterMainWindowAction(
            k_helpApi.data(), "API Reference", "Open the O3DE API reference in a browser", []
            {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://o3de.org/docs/api/")));
            });
        RegisterMainWindowAction(
            k_helpGithub.data(), "O3DE on GitHub", "Open the O3DE repository in a browser", []
            {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/o3de/o3de")));
            });

        // --- View panel toggles + Tools pane openers (P1-14): real registered actions per
        // registry pane (the pane set is static before registration runs). State and open/close
        // go straight through the docking system - the only source of truth (A1).
        const AZStd::string viewMenu(EditorIdentifiers::ViewMenuIdentifier);
        for (const ViewPaneEntry& pane : mainWindow->GetPaneRegistry().Entries())
        {
            const AZStd::string dockName(pane.m_name.toUtf8().constData());

            RegisterCheckableAction(
                ViewPanelActionId(dockName).c_str(), pane.m_title.toUtf8().constData(),
                [mainWindow, dockName]
                {
                    mainWindow->SetPanelOpen(dockName.c_str(), !mainWindow->IsPanelOpen(dockName.c_str()));
                },
                [mainWindow, dockName]
                {
                    return mainWindow->IsPanelOpen(dockName.c_str());
                },
                1000 + pane.m_sortKey, viewMenu);

            RegisterMainWindowAction(
                ToolsOpenActionId(dockName).c_str(),
                AZStd::string::format("Open %s", pane.m_title.toUtf8().constData()).c_str(),
                AZStd::string::format("Bring the %s pane to the front", pane.m_title.toUtf8().constData()).c_str(),
                [mainWindow, paneName = pane.m_name]
                {
                    mainWindow->OpenViewPane(paneName);
                });
        }

        // --- Backend seam (P1-13 / M1): engine-specific actions come from the backend's static
        // table, registered by this GENERIC loop - a new engine wires its commands by
        // implementing one contract method, never by editing the shell. Patterns without a menu
        // id are command-palette only.
        if (auto* backend = AZ::Interface<IEngineBackend>::Get())
        {
            for (EngineActionPattern& pattern : backend->GetActionRegistrationPatterns())
            {
                if (pattern.m_id.empty() || !pattern.m_handler)
                {
                    continue;
                }

                AzToolsFramework::ActionProperties properties;
                properties.m_name = pattern.m_name;
                properties.m_description = pattern.m_description;
                properties.m_category = "Engine";
                properties.m_menuVisibility = AzToolsFramework::ActionVisibility::AlwaysShow;

                m_actionManagerInterface->RegisterAction(
                    context, pattern.m_id, properties, AZStd::move(pattern.m_handler));

                if (!pattern.m_hotKey.empty())
                {
                    m_hotKeyManagerInterface->SetActionHotKey(pattern.m_id, pattern.m_hotKey);
                }
                if (!pattern.m_menuIdentifier.empty())
                {
                    m_menuManagerInterface->AddActionToMenu(pattern.m_menuIdentifier, pattern.m_id, pattern.m_sortKey);
                }
            }
        }
    }

    void CeeActionsHandler::OnMenuBarRegistrationHook()
    {
        m_menuManagerInterface->RegisterMenuBar(
            AZStd::string(EditorIdentifiers::EditorMainWindowMenuBarIdentifier), m_mainWindow);
    }

    void CeeActionsHandler::OnMenuRegistrationHook()
    {
        struct MenuEntry
        {
            AZStd::string_view m_id;
            const char* m_name;
        };

        // Top menus (D2); Game stays reserved at sortkey 300 until ISimulation lands
        // (Plan §B12 E3). The Create submenu (P1-12/S3) gets engine types registered into it
        // as real actions on first enumeration (EditorMainWindow::PopulateCreateMenu).
        for (const MenuEntry& entry :
             { MenuEntry{ EditorIdentifiers::FileMenuIdentifier, "&File" },
               MenuEntry{ EditorIdentifiers::EditMenuIdentifier, "&Edit" },
               MenuEntry{ EditorIdentifiers::ToolsMenuIdentifier, "&Tools" },
               MenuEntry{ EditorIdentifiers::ViewMenuIdentifier, "&View" },
               MenuEntry{ EditorIdentifiers::HelpMenuIdentifier, "&Help" },
               MenuEntry{ EditorIdentifiers::EntityCreationMenuIdentifier, "Create" } })
        {
            AzToolsFramework::MenuProperties menuProperties;
            menuProperties.m_name = entry.m_name;
            m_menuManagerInterface->RegisterMenu(AZStd::string(entry.m_id), menuProperties);
        }

        // The three ActionManager-backed context menus (editor_polish.md P0-5 / F7). Ids reuse
        // the framework identifiers because the display sites hard-code them. The fourth
        // right-click surface (Inspector property rows) is a local QMenu the
        // EntityPropertyEditor builds itself - it needs no registration.
        for (const MenuEntry& entry :
             { MenuEntry{ EditorIdentifiers::EntityOutlinerContextMenuIdentifier, "Entity Outliner Context Menu" },
               MenuEntry{ EditorIdentifiers::ViewportContextMenuIdentifier, "Viewport Context Menu" },
               MenuEntry{ EditorIdentifiers::InspectorEntityComponentContextMenuIdentifier,
                          "Inspector Component Context Menu" } })
        {
            AzToolsFramework::MenuProperties menuProperties;
            menuProperties.m_name = entry.m_name;
            m_menuManagerInterface->RegisterMenu(AZStd::string(entry.m_id), menuProperties);
        }
    }

    void CeeActionsHandler::OnMenuBindingHook()
    {
        const AZStd::string menuBar(EditorIdentifiers::EditorMainWindowMenuBarIdentifier);
        // Sort keys leave room exactly like the native (Game reserved at 300 for ISimulation).
        m_menuManagerInterface->AddMenuToMenuBar(menuBar, AZStd::string(EditorIdentifiers::FileMenuIdentifier), 100);
        m_menuManagerInterface->AddMenuToMenuBar(menuBar, AZStd::string(EditorIdentifiers::EditMenuIdentifier), 200);
        m_menuManagerInterface->AddMenuToMenuBar(menuBar, AZStd::string(EditorIdentifiers::ToolsMenuIdentifier), 400);
        m_menuManagerInterface->AddMenuToMenuBar(menuBar, AZStd::string(EditorIdentifiers::ViewMenuIdentifier), 500);
        m_menuManagerInterface->AddMenuToMenuBar(menuBar, AZStd::string(EditorIdentifiers::HelpMenuIdentifier), 600);

        // File: New / Open / [Open Recent - bound by the main window when entries exist] / Save.
        const AZStd::string fileMenu(EditorIdentifiers::FileMenuIdentifier);
        m_menuManagerInterface->AddActionToMenu(fileMenu, AZStd::string(CeeActions::FileNew), 100);
        m_menuManagerInterface->AddActionToMenu(fileMenu, AZStd::string(CeeActions::FileOpen), 200);
        m_menuManagerInterface->AddSeparatorToMenu(fileMenu, 400);
        m_menuManagerInterface->AddActionToMenu(fileMenu, AZStd::string(CeeActions::FileSave), 500);

        // Edit: Undo/Redo first (P0-7 placeholders), Create submenu, clipboard/delete, focus,
        // preferences.
        const AZStd::string editMenu(EditorIdentifiers::EditMenuIdentifier);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(CeeActions::EditUndo), 100);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(CeeActions::EditRedo), 110);
        m_menuManagerInterface->AddSubMenuToMenu(editMenu, AZStd::string(EditorIdentifiers::EntityCreationMenuIdentifier), 200);
        m_menuManagerInterface->AddSeparatorToMenu(editMenu, 5000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_entityCut), 20000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_entityCopy), 21000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_entityPaste), 22000);
        m_menuManagerInterface->AddSeparatorToMenu(editMenu, 30000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_entityDuplicate), 40000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_entityDelete), 40100);
        m_menuManagerInterface->AddSeparatorToMenu(editMenu, 60000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_focusSelection), 70000);
        m_menuManagerInterface->AddSeparatorToMenu(editMenu, 80000);
        m_menuManagerInterface->AddActionToMenu(editMenu, AZStd::string(k_preferences), 81000);

        // Create submenu: the shell-level "Empty Node" lives here; engine types are appended by
        // EditorMainWindow::PopulateCreateMenu on first enumeration.
        m_menuManagerInterface->AddActionToMenu(
            AZStd::string(EditorIdentifiers::EntityCreationMenuIdentifier), AZStd::string(k_entityCreate), 100);

        // Tools: one opener per registered pane (bound in registration order = pane sortKey).
        const AZStd::string toolsMenu(EditorIdentifiers::ToolsMenuIdentifier);
        for (const ViewPaneEntry& pane : m_mainWindow->GetPaneRegistry().Entries())
        {
            m_menuManagerInterface->AddActionToMenu(
                toolsMenu, ToolsOpenActionId(AZStd::string(pane.m_name.toUtf8().constData())), pane.m_sortKey);
        }

        // View: workspaces / palette (panel toggles bind themselves in
        // RegisterCheckableAction), plus help links below.
        const AZStd::string viewMenu(EditorIdentifiers::ViewMenuIdentifier);
        m_menuManagerInterface->AddSeparatorToMenu(viewMenu, 90000);
        m_menuManagerInterface->AddActionToMenu(viewMenu, AZStd::string(k_workspaceSave), 91000);
        m_menuManagerInterface->AddActionToMenu(viewMenu, AZStd::string(k_workspaceRestore), 92000);
        m_menuManagerInterface->AddSeparatorToMenu(viewMenu, 93000);
        m_menuManagerInterface->AddActionToMenu(viewMenu, AZStd::string(k_commandPalette), 94000);

        // Help.
        const AZStd::string helpMenu(EditorIdentifiers::HelpMenuIdentifier);
        m_menuManagerInterface->AddActionToMenu(helpMenu, AZStd::string(k_helpDocs), 100);
        m_menuManagerInterface->AddActionToMenu(helpMenu, AZStd::string(k_helpApi), 200);
        m_menuManagerInterface->AddSeparatorToMenu(helpMenu, 300);
        m_menuManagerInterface->AddActionToMenu(helpMenu, AZStd::string(k_helpGithub), 400);

        // Bind the entity pool into all three context menus (the native's sort-key layout:
        // create ~200, clipboard ~20k/30k, duplicate/delete ~40k, focus ~80k).
        for (const AZStd::string_view menuId :
             { EditorIdentifiers::EntityOutlinerContextMenuIdentifier,
               EditorIdentifiers::ViewportContextMenuIdentifier,
               EditorIdentifiers::InspectorEntityComponentContextMenuIdentifier })
        {
            const AZStd::string menu(menuId);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_entityCreate), 200);
            m_menuManagerInterface->AddSeparatorToMenu(menu, 10000);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_entityCut), 20000);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_entityCopy), 21000);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_entityPaste), 22000);
            m_menuManagerInterface->AddSeparatorToMenu(menu, 30000);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_entityDuplicate), 40000);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_entityDelete), 40100);
            m_menuManagerInterface->AddSeparatorToMenu(menu, 70000);
            m_menuManagerInterface->AddActionToMenu(menu, AZStd::string(k_focusSelection), 80000);
        }
    }

    void CeeActionsHandler::OnPostActionManagerRegistrationHook()
    {
        // Registration bookmark for the acceptance log (P0-4): every handler connected to this
        // bus before TriggerRegistrationNotifications() has now registered.
        AZ_Printf("CrossEngineEditor", "ActionManager registration complete (CeeActionsHandler post-hook).\n");

        // Let the main window build its toolbar and wire dynamic menu content (P1-12/16).
        if (m_mainWindow)
        {
            m_mainWindow->OnActionManagerReady();
        }

        // Prime the enable states once so the first menu opens honest.
        m_actionManagerInterface->TriggerActionUpdater(AZStd::string(CeeActions::SelectionUpdater));
        m_actionManagerInterface->TriggerActionUpdater(AZStd::string(CeeActions::PanelsUpdater));
    }

    void CeeActionsHandler::AfterEntitySelectionChanged(
        [[maybe_unused]] const AzToolsFramework::EntityIdList& newlySelectedEntities,
        [[maybe_unused]] const AzToolsFramework::EntityIdList& newlyDeselectedEntities)
    {
        m_actionManagerInterface->TriggerActionUpdater(AZStd::string(CeeActions::SelectionUpdater));
    }
} // namespace CrossEngineEditor
