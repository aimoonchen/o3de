/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#if !defined(Q_MOC_RUN)
#include <AzToolsFramework/UI/PropertyEditor/PropertyEditorAPI.h>
#include <AzToolsFramework/UI/PropertyEditor/PropertyEditorAPI_Internals.h>

#include <QDialog>
#endif

class QAction;
class QListWidget;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;

namespace AzToolsFramework
{
    class ReflectedPropertyEditor;
}

namespace CrossEngineEditor
{
    class CeePreferences;

    //! Preferences dialog (editor_polish.md P2 / D9 + E1): a category list and two pages - the
    //! reflected settings grid (SettingsRegistry-backed) and the hotkey rebinding table
    //! (HotKeyManager::SetActionHotKey is public API; upstream has no persistence layer - F4 -
    //! so CEE keeps bindings in QSettings and re-applies them at startup).
    class CeePreferencesDialog final
        : public QDialog
        , private AzToolsFramework::IPropertyEditorNotify
    {
        Q_OBJECT
    public:
        //! \param preferences shared settings object (edited in place).
        //! \param commands command actions to offer for rebinding (from CollectCommands).
        explicit CeePreferencesDialog(QWidget* parent, CeePreferences* preferences, QList<QAction*> commands);

        ~CeePreferencesDialog() override;

    Q_SIGNALS:
        //! Fired after any settings value changed (the owner saves + applies).
        void PreferencesChanged();

    protected:
        void accept() override;
        void reject() override;

    private:
        // AzToolsFramework::IPropertyEditorNotify...
        void BeforePropertyModified(AzToolsFramework::InstanceDataNode* node) override;
        void AfterPropertyModified(AzToolsFramework::InstanceDataNode* node) override;
        void SetPropertyEditingActive(AzToolsFramework::InstanceDataNode* node) override;
        void SetPropertyEditingComplete(AzToolsFramework::InstanceDataNode* node) override;
        void SealUndoStack() override {}

        QWidget* BuildSettingsPage();
        QWidget* BuildShortcutsPage();
        void SaveHotKeyBindings() const;

        CeePreferences* m_preferences = nullptr;
        QList<QAction*> m_commands;
        QTableWidget* m_shortcutTable = nullptr;
        AzToolsFramework::ReflectedPropertyEditor* m_propertyEditor = nullptr;
        bool m_hotKeysChanged = false;
    };
} // namespace CrossEngineEditor
