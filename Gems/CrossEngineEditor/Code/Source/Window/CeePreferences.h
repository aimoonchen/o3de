/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Editor preferences model + SettingsRegistry persistence (editor_polish.md P2 / D9).
//!
//! "SettingsRegistry persistence first": values live under /CEE/Preferences in the settings
//! registry (queryable like any O3DE setting) and round-trip through a JSON file next to the
//! executable (cee_preferences.setreg). The Preferences dialog edits this object through a
//! ReflectedPropertyEditor, so no bespoke settings framework exists - only the reflected
//! struct. Every field has a live consumer (零死条目): autosave drives the timer, helpers
//! visibility feeds AzToolsFramework::SetHelpersVisible (read per frame by EditorHelpers),
//! the default gizmo style seeds GizmoManager on startup.

#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/RTTI/RTTI.h>
#include <AzCore/RTTI/TypeInfoSimple.h>
#include <AzCore/std/string/string.h>

namespace AZ
{
    class ReflectContext;
}

namespace CrossEngineEditor
{
    class CeePreferences final
    {
    public:
        //! Startup transform-gizmo look (indices match GizmoTheme's GizmoStyle).
        enum class GizmoStylePreference
        {
            Blender = 0,
            Unreal = 1,
        };

        AZ_TYPE_INFO(CeePreferences, "{3F1A5C62-8E4B-4D07-9A2C-6C0E7F5B1A44}");
        AZ_CLASS_ALLOCATOR(CeePreferences, AZ::SystemAllocator);

        static void Reflect(AZ::ReflectContext* context);

        //! Absolute path of the persisted settings file (next to the executable).
        static AZStd::string SettingsFilePath();

        //! Load from the settings registry (merging the file first when present). Missing
        //! values keep the defaults below.
        void Load();
        //! Push every field into the registry and dump /CEE/Preferences to the settings file.
        void Save() const;

        bool m_autosaveEnabled = true;
        int m_autosaveIntervalMinutes = 5;   //!< 1..120, clamped on load.
        bool m_showViewportHelpers = true;   //!< Wireframe gizmos + editor helpers in the viewport.
        GizmoStylePreference m_defaultGizmoStyle = GizmoStylePreference::Blender;
    };
} // namespace CrossEngineEditor
