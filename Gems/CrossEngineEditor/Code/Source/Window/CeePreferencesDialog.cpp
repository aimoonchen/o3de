/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/CeePreferencesDialog.h>
#include <Window/CeePreferences.h>

#include <AzCore/Interface/Interface.h>
#include <AzCore/Serialization/SerializeContext.h>

#include <AzToolsFramework/ActionManager/Action/ActionManagerInternalInterface.h>
#include <AzToolsFramework/ActionManager/HotKey/HotKeyManagerInterface.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzToolsFramework/UI/PropertyEditor/ReflectedPropertyEditor.hxx>

#include <AzQtComponents/Components/Widgets/Card.h>
#include <AzQtComponents/Components/Widgets/CardHeader.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>
AZ_POP_DISABLE_WARNING

namespace
{
    //! Modal dialog recording the next key press as a QKeySequence (the rebinding editor).
    //! Escape cancels; Backspace/Delete clears the binding.
    class KeyCaptureDialog final : public QDialog
    {
    public:
        explicit KeyCaptureDialog(QWidget* parent)
            : QDialog(parent)
        {
            setWindowTitle(QStringLiteral("Press keys..."));
            setModal(true);
            auto* layout = new QVBoxLayout(this);
            auto* label = new QLabel(QStringLiteral("Press the new key combination.\n\n(Esc cancels, Backspace clears)"), this);
            label->setAlignment(Qt::AlignCenter);
            layout->addWidget(label);
            resize(280, 110);
        }

        void keyPressEvent(QKeyEvent* event) override
        {
            const int key = event->key();
            if (key == Qt::Key_Escape)
            {
                reject();
                return;
            }
            if (key == Qt::Key_Backspace || key == Qt::Key_Delete)
            {
                m_sequence = QKeySequence();
                accept();
                return;
            }
            if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta)
            {
                return; // modifier alone - keep waiting.
            }
            m_sequence = QKeySequence(key | static_cast<int>(event->modifiers()));
            accept();
        }

        QKeySequence m_sequence;
    };
} // namespace

namespace CrossEngineEditor
{
    CeePreferencesDialog::CeePreferencesDialog(QWidget* parent, CeePreferences* preferences, QList<QAction*> commands)
        : QDialog(parent)
        , m_preferences(preferences)
        , m_commands(AZStd::move(commands))
    {
        setWindowTitle(QStringLiteral("Preferences"));
        setObjectName(QStringLiteral("CeePreferencesDialog"));
        resize(760, 520);

        auto* rootLayout = new QVBoxLayout(this);

        auto* body = new QWidget(this);
        auto* bodyLayout = new QHBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);

        // Category list (D9: QTreeWidget-shaped navigation; a flat list widget carries the same
        // structure for two pages without fake nesting).
        auto* categories = new QListWidget(body);
        categories->setObjectName(QStringLiteral("PreferenceCategories"));
        categories->setFixedWidth(180);
        auto* pages = new QStackedWidget(body);
        pages->addWidget(BuildSettingsPage());
        pages->addWidget(BuildShortcutsPage());
        bodyLayout->addWidget(categories);
        bodyLayout->addWidget(pages, /*stretch=*/1);

        categories->addItem(QStringLiteral("General"));
        categories->addItem(QStringLiteral("Shortcuts"));
        categories->setCurrentRow(0);
        pages->setCurrentIndex(0);
        connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        rootLayout->addWidget(body, /*stretch=*/1);
        rootLayout->addWidget(buttons);
    }

    CeePreferencesDialog::~CeePreferencesDialog() = default;

    QWidget* CeePreferencesDialog::BuildSettingsPage()
    {
        // Reflected grid over the SettingsRegistry-backed CeePreferences (D9: data source =
        // SettingsRegistry), framed by a Card (AzQtComponents, P2 A10 control upgrade).
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);

        auto* card = new AzQtComponents::Card(page);
        card->setTitle(QStringLiteral("Editor settings"));

        m_propertyEditor = new AzToolsFramework::ReflectedPropertyEditor(card);
        AZ::SerializeContext* serializeContext = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            serializeContext, &AZ::ComponentApplicationRequests::GetSerializeContext);
        m_propertyEditor->Setup(serializeContext, this, /*enableScrollbars=*/true, 220);
        if (m_preferences)
        {
            m_propertyEditor->AddInstance(m_preferences);
            m_propertyEditor->InvalidateAll();
        }

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 12, 12, 12);
        cardLayout->addWidget(m_propertyEditor);

        layout->addWidget(card);
        layout->addStretch(1);
        return page;
    }

    QWidget* CeePreferencesDialog::BuildShortcutsPage()
    {
        // Hotkey rebinding (E1): HotKeyManagerInterface::SetActionHotKey is public; upstream
        // has no persistence (F4), so bindings made here are stored in QSettings under
        // HotKeys/<action id> and re-applied at startup (EditorMainWindow::OnActionManagerReady).
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);

        auto* card = new AzQtComponents::Card(page);
        card->setTitle(QStringLiteral("Keyboard shortcuts"));
        auto* hint = new QLabel(
            QStringLiteral("Double-click a shortcut to change it.\nRebindings are applied immediately and restored on the next launch."), card);
        hint->setWordWrap(true);

        m_shortcutTable = new QTableWidget(/*rows=*/0, /*columns=*/2, card);
        m_shortcutTable->setHorizontalHeaderLabels({ QStringLiteral("Command"), QStringLiteral("Shortcut") });
        m_shortcutTable->horizontalHeader()->setStretchLastSection(false);
        m_shortcutTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_shortcutTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_shortcutTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

        for (QAction* action : m_commands)
        {
            if (action == nullptr || action->objectName().isEmpty() || !action->objectName().startsWith(QStringLiteral("cee.action.")))
            {
                continue; // only CEE-owned actions are rebindable in v1.
            }
            const int row = m_shortcutTable->rowCount();
            m_shortcutTable->insertRow(row);
            m_shortcutTable->setItem(row, 0, new QTableWidgetItem(action->text().remove(QChar('&'))));
            m_shortcutTable->setItem(row, 1, new QTableWidgetItem(action->shortcut().toString(QKeySequence::NativeText)));
            // Row items carry the action id for the rebind handler.
            m_shortcutTable->item(row, 0)->setData(Qt::UserRole, action->objectName());
            m_shortcutTable->item(row, 1)->setData(Qt::UserRole, action->objectName());
        }
        m_shortcutTable->sortItems(0);

        connect(m_shortcutTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column)
        {
            if (column != 1)
            {
                return;
            }
            const QString actionId = m_shortcutTable->item(row, 0)->data(Qt::UserRole).toString();
            KeyCaptureDialog capture(this);
            if (capture.exec() != QDialog::Accepted)
            {
                return;
            }
            auto* hotKeyManager = AZ::Interface<AzToolsFramework::HotKeyManagerInterface>::Get();
            if (!hotKeyManager)
            {
                return;
            }

            if (capture.m_sequence.isEmpty())
            {
                // Clearing: upstream SetActionHotKey has no clear concept (its validation
                // rejects an empty sequence), so clear the QAction directly - exactly what
                // EditorAction::SetHotKey does under the hood. Persistence stores the empty
                // marker and startup replay applies it as the cleared state.
                if (auto* actionManagerInternal = AZ::Interface<AzToolsFramework::ActionManagerInternalInterface>::Get();
                    QAction* action = actionManagerInternal
                        ? actionManagerInternal->GetAction(AZStd::string(actionId.toUtf8().constData()))
                        : nullptr)
                {
                    action->setShortcut(QKeySequence());
                    m_shortcutTable->item(row, 1)->setText(QString());
                    m_hotKeysChanged = true;
                }
                return;
            }

            const QString binding = capture.m_sequence.toString(QKeySequence::PortableText);
            const auto result = hotKeyManager->SetActionHotKey(
                AZStd::string(actionId.toUtf8().constData()), AZStd::string(binding.toUtf8().constData()));
            if (!result.IsSuccess())
            {
                return; // invalid combination - keep the old binding.
            }
            m_shortcutTable->item(row, 1)->setText(capture.m_sequence.toString(QKeySequence::NativeText));
            m_hotKeysChanged = true;
        });

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 12, 12, 12);
        cardLayout->addWidget(hint);
        cardLayout->addWidget(m_shortcutTable);

        layout->addWidget(card);
        return page;
    }

    void CeePreferencesDialog::AfterPropertyModified(AzToolsFramework::InstanceDataNode* /*node*/)
    {
        // Grid wrote straight into the shared CeePreferences; let the owner persist + apply.
        Q_EMIT PreferencesChanged();
    }

    void CeePreferencesDialog::BeforePropertyModified(AzToolsFramework::InstanceDataNode* /*node*/)
    {
    }

    void CeePreferencesDialog::SetPropertyEditingActive(AzToolsFramework::InstanceDataNode* /*node*/)
    {
    }

    void CeePreferencesDialog::SetPropertyEditingComplete(AzToolsFramework::InstanceDataNode* /*node*/)
    {
        // Editing finished (e.g. slider released) - same handling as a discrete edit.
        Q_EMIT PreferencesChanged();
    }

    void CeePreferencesDialog::SaveHotKeyBindings() const
    {
        // Persist every current cee.action.* binding (not only the changed ones) so a binding
        // removed elsewhere still lands in the file. Restored at startup.
        if (!m_hotKeysChanged)
        {
            return;
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("HotKeys"));
        for (QAction* action : m_commands)
        {
            if (action && action->objectName().startsWith(QStringLiteral("cee.action.")))
            {
                settings.setValue(action->objectName(), action->shortcut().toString(QKeySequence::PortableText));
            }
        }
        settings.endGroup();
    }

    void CeePreferencesDialog::accept()
    {
        SaveHotKeyBindings();
        QDialog::accept();
    }

    void CeePreferencesDialog::reject()
    {
        // Bindings already went live through SetActionHotKey; persist them even on Cancel so
        // the UI and the file never disagree (settings values themselves are saved on change).
        SaveHotKeyBindings();
        QDialog::reject();
    }
} // namespace CrossEngineEditor
