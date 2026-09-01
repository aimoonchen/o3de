/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! CEE-owned ActionManager identifiers shared by more than one file (editor_polish.md D2b /
//! S4): CeeActionsHandler registers the actions/updaters, EditorMainWindow looks the QActions
//! up by id (main toolbar) and triggers the updaters. Ids only used in one file stay local.

#include <AzCore/std/string/string_view.h>

namespace CrossEngineEditor::CeeActions
{
    // Actions referenced from EditorMainWindow (toolbar).
    inline constexpr AZStd::string_view FileNew = "cee.action.file.newLevel";
    inline constexpr AZStd::string_view FileOpen = "cee.action.file.openLevel";
    inline constexpr AZStd::string_view FileSave = "cee.action.file.saveLevel";
    inline constexpr AZStd::string_view EditUndo = "cee.action.edit.undo";
    inline constexpr AZStd::string_view EditRedo = "cee.action.edit.redo";

    // Updaters (enabled/checked state refresh triggers).
    inline constexpr AZStd::string_view SelectionUpdater = "cee.updater.selection";
    inline constexpr AZStd::string_view RecentUpdater = "cee.updater.recent";
    inline constexpr AZStd::string_view PanelsUpdater = "cee.updater.panels";
} // namespace CrossEngineEditor::CeeActions
