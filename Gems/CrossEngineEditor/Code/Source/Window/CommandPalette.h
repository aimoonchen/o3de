/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! VSCode/Blender-style command palette (plan §5.2 / 阶段3.3).
//!
//! A lightweight modal popup that fuzzy-filters a flat list of QActions (the editor's
//! commands) and triggers the chosen one. The data source is simply the actions already
//! present on the menus, so no separate command registry is needed - this is the KISS
//! path to a discoverable command surface until a full ActionManager phase lands.

#if !defined(Q_MOC_RUN)
#include <QDialog>
#include <QList>
#endif

class QAction;
class QLineEdit;
class QListWidget;

namespace CrossEngineEditor
{
    class CommandPalette : public QDialog
    {
        Q_OBJECT
    public:
        //! Show the palette centred on the parent, filtering over the given actions.
        //! Blocks until the user picks a command or dismisses it; triggers the chosen action.
        static void Show(QWidget* parent, const QList<QAction*>& actions);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        CommandPalette(QWidget* parent, const QList<QAction*>& actions);

        void Refilter(const QString& query);
        void AcceptCurrent();

        //! Case-insensitive subsequence match (fuzzy): every query char appears in order.
        static bool FuzzyMatch(const QString& query, const QString& text);

        QLineEdit* m_search = nullptr;
        QListWidget* m_list = nullptr;
        QList<QAction*> m_actions;
    };
} // namespace CrossEngineEditor
