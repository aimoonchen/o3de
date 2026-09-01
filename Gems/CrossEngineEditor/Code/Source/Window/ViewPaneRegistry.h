/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! ViewPane thin registry (editor_polish.md P1-14 / D6).
//!
//! Deliberately NOT a port of the native QtViewPaneManager (1500 lines + Cry resource ids +
//! XmlNodeRef serialization - D1): this is exactly "name -> factory -> dock". Its real job is
//! giving every panel a STABLE name - ADS restoreState resolves docks by name
//! (DockManager.cpp:427 findDockWidget), and the Tools menu / View toggles enumerate from here.
//! Layout serialization stays with ADS saveState (single source of truth).

#if !defined(Q_MOC_RUN)
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/functional.h>
#endif

#include <QString>

class QWidget;

namespace CrossEngineEditor
{
    //! One registered dockable tool pane.
    struct ViewPaneEntry
    {
        //! Stable dock object name (the ADS lookup key) - never rename for an existing pane.
        QString m_name;
        //! User-facing title shown on the dock tab.
        QString m_title;
        //! Tools-menu group.
        QString m_category;
        //! Initial dock area for a first launch (afterwards the persisted layout wins).
        Qt::DockWidgetArea m_area = Qt::LeftDockWidgetArea;
        //! Tools / View menu ordering.
        int m_sortKey = 0;
        //! Creates the pane content widget (parented by the caller).
        AZStd::function<QWidget*()> m_factory;
    };

    //! Ordered registry of tool panes. Populated once at startup, before ActionManager
    //! registration runs (pane actions are registered from it, editor_polish.md P1-12).
    class ViewPaneRegistry final
    {
    public:
        void RegisterPane(ViewPaneEntry entry);
        [[nodiscard]] const AZStd::vector<ViewPaneEntry>& Entries() const
        {
            return m_entries;
        }
        [[nodiscard]] const ViewPaneEntry* Find(const QString& name) const;

    private:
        AZStd::vector<ViewPaneEntry> m_entries;
    };
} // namespace CrossEngineEditor
