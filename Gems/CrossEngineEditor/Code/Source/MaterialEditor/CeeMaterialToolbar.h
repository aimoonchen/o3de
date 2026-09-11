/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Material document toolbar - combo shell for open documents.
//!
//! QComboBox listing open documents + Save/Close buttons.
//! Connects to AtomToolsDocumentNotificationBus for open/close events.

#include <AzCore/std/string/string.h>
#include <AzCore/std/containers/vector.h>

#include <QWidget>

class QComboBox;
class QPushButton;

namespace CrossEngineEditor
{
    class CeeMaterialToolbar final : public QWidget
    {
        Q_OBJECT
    public:
        explicit CeeMaterialToolbar(QWidget* parent = nullptr);
        ~CeeMaterialToolbar() override = default;

        //! Called when a document is opened or closed to refresh the combo.
        void RefreshDocumentList();

        //! Add a document to the combo (called on OnDocumentOpened).
        void AddDocument(const AZ::Uuid& documentId, const AZStd::string& path);

        //! Remove a document from the combo (called on OnDocumentCleared).
        void RemoveDocument(const AZ::Uuid& documentId);

        //! Update undo/redo button enable state from CanUndo/CanRedo.
        void UpdateUndoRedoState(bool canUndo, bool canRedo);

        //! Prefix the document's combo entry with "* " while it is modified (the upstream
        //! convention, AtomToolsDocumentMainWindow::UpdateDocumentTab).
        void SetDocumentModified(const AZ::Uuid& documentId, bool modified);

        //! Get the currently selected document ID.
        [[nodiscard]] AZ::Uuid CurrentDocumentId() const;

    Q_SIGNALS:
        void DocumentSelected(const AZStd::string& path);
        void NewDocumentRequested();
        void SaveRequested();
        void CloseRequested();
        void UndoRequested();
        void RedoRequested();

    private Q_SLOTS:
        void OnComboChanged(int index);
        void OnNewClicked();
        void OnSaveClicked();
        void OnCloseClicked();
        void OnUndoClicked();
        void OnRedoClicked();

    private:
        QComboBox* m_combo = nullptr;
        QPushButton* m_newButton = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_closeButton = nullptr;
        QPushButton* m_undoButton = nullptr;
        QPushButton* m_redoButton = nullptr;

        struct DocumentEntry
        {
            AZStd::string m_path;
            AZStd::string m_displayName;  //!< base file name; the "* " modified marker is combo-text only
            AZ::Uuid m_documentId;
        };
        AZStd::vector<DocumentEntry> m_documents;
    };
} // namespace CrossEngineEditor
