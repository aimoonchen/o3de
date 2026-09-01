/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/CommandPalette.h>

// AZ_PUSH/POP_DISABLE_WARNING live in PlatformDef.h; include it explicitly so this TU does not
// depend on unity-build include order (it became the first file of its unity batch when
// CeeActionsHandler.cpp joined the target and the grouping shifted).
#include <AzCore/PlatformDef.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QAction>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    void CommandPalette::Show(QWidget* parent, const QList<QAction*>& actions)
    {
        CommandPalette palette(parent, actions);
        palette.exec();
    }

    CommandPalette::CommandPalette(QWidget* parent, const QList<QAction*>& actions)
        : QDialog(parent)
        , m_actions(actions)
    {
        setWindowTitle(QStringLiteral("Command Palette"));
        setModal(true);
        setMinimumWidth(480);

        auto* layout = new QVBoxLayout(this);
        m_search = new QLineEdit(this);
        m_search->setPlaceholderText(QStringLiteral("Type a command..."));
        m_list = new QListWidget(this);
        layout->addWidget(m_search);
        layout->addWidget(m_list);

        connect(m_search, &QLineEdit::textChanged, this, &CommandPalette::Refilter);
        connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { AcceptCurrent(); });

        // Let the arrow keys and Enter drive the list while typing stays in the search box.
        m_search->installEventFilter(this);
        m_search->setFocus();

        Refilter(QString());
    }

    bool CommandPalette::FuzzyMatch(const QString& query, const QString& text)
    {
        if (query.isEmpty())
        {
            return true;
        }
        int q = 0;
        for (int t = 0; t < text.size() && q < query.size(); ++t)
        {
            if (text[t].toLower() == query[q].toLower())
            {
                ++q;
            }
        }
        return q == query.size();
    }

    void CommandPalette::Refilter(const QString& query)
    {
        m_list->clear();
        for (QAction* action : m_actions)
        {
            const QString label = action->text().remove(QLatin1Char('&'));
            if (label.isEmpty() || !FuzzyMatch(query, label))
            {
                continue;
            }
            auto* item = new QListWidgetItem(label, m_list);
            item->setData(Qt::UserRole, QVariant::fromValue(reinterpret_cast<quintptr>(action)));
        }
        if (m_list->count() > 0)
        {
            m_list->setCurrentRow(0);
        }
    }

    void CommandPalette::AcceptCurrent()
    {
        if (QListWidgetItem* item = m_list->currentItem())
        {
            auto* action = reinterpret_cast<QAction*>(item->data(Qt::UserRole).value<quintptr>());
            accept();
            if (action)
            {
                action->trigger();
            }
        }
    }

    // Route navigation keys from the search box to the results list (VSCode feel).
    bool CommandPalette::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == m_search && event->type() == QEvent::KeyPress)
        {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            switch (keyEvent->key())
            {
            case Qt::Key_Down:
                m_list->setCurrentRow(qMin(m_list->currentRow() + 1, m_list->count() - 1));
                return true;
            case Qt::Key_Up:
                m_list->setCurrentRow(qMax(m_list->currentRow() - 1, 0));
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                AcceptCurrent();
                return true;
            default:
                break;
            }
        }
        return QDialog::eventFilter(watched, event);
    }
} // namespace CrossEngineEditor
