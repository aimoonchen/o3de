/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/CeeMaterialToolbar.h>

#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentSystemRequestBus.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentSystem.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace CrossEngineEditor
{
    CeeMaterialToolbar::CeeMaterialToolbar(QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(4);

        m_modifiedLabel = new QLabel(QStringLiteral("*"), this);
        m_modifiedLabel->setStyleSheet(QStringLiteral("color: orange; font-weight: bold;"));
        layout->addWidget(m_modifiedLabel);

        m_combo = new QComboBox(this);
        m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CeeMaterialToolbar::OnComboChanged);
        layout->addWidget(m_combo);

        m_newButton = new QPushButton(QStringLiteral("New"), this);
        connect(m_newButton, &QPushButton::clicked, this, &CeeMaterialToolbar::OnNewClicked);
        layout->addWidget(m_newButton);

        m_saveButton = new QPushButton(QStringLiteral("Save"), this);
        connect(m_saveButton, &QPushButton::clicked, this, &CeeMaterialToolbar::OnSaveClicked);
        layout->addWidget(m_saveButton);

        m_closeButton = new QPushButton(QStringLiteral("Close"), this);
        connect(m_closeButton, &QPushButton::clicked, this, &CeeMaterialToolbar::OnCloseClicked);
        layout->addWidget(m_closeButton);

        // Undo / Redo buttons
        layout->addSpacing(8);

        m_undoButton = new QPushButton(QStringLiteral("Undo"), this);
        m_undoButton->setEnabled(false);
        connect(m_undoButton, &QPushButton::clicked, this, &CeeMaterialToolbar::OnUndoClicked);
        layout->addWidget(m_undoButton);

        m_redoButton = new QPushButton(QStringLiteral("Redo"), this);
        m_redoButton->setEnabled(false);
        connect(m_redoButton, &QPushButton::clicked, this, &CeeMaterialToolbar::OnRedoClicked);
        layout->addWidget(m_redoButton);
    }

    void CeeMaterialToolbar::RefreshDocumentList()
    {
        m_combo->clear();

        // Rebuild the combo from the tracked document list.
        for (const auto& entry : m_documents)
        {
            m_combo->addItem(QString::fromUtf8(entry.m_displayName.c_str()));
        }

        if (m_combo->count() == 0)
        {
            m_combo->addItem(QStringLiteral("(no document)"));
        }
    }

    void CeeMaterialToolbar::AddDocument(const AZ::Uuid& documentId, const AZStd::string& path)
    {
        // Check for duplicates.
        for (const auto& entry : m_documents)
        {
            if (entry.m_documentId == documentId)
            {
                return;
            }
        }

        DocumentEntry entry;
        entry.m_documentId = documentId;
        entry.m_path = path;
        entry.m_displayName = QString::fromUtf8(path.c_str()).section(QLatin1Char('/'), -1).toUtf8().constData();
        m_documents.push_back(AZStd::move(entry));
        m_combo->addItem(QString::fromUtf8(m_documents.back().m_displayName.c_str()));

        // Remove the placeholder "(no document)" if present.
        if (m_combo->count() == 2 && m_combo->itemText(0) == QStringLiteral("(no document)"))
        {
            m_combo->removeItem(0);
        }
    }

    void CeeMaterialToolbar::RemoveDocument(const AZ::Uuid& documentId)
    {
        for (int i = 0; i < static_cast<int>(m_documents.size()); ++i)
        {
            if (m_documents[i].m_documentId == documentId)
            {
                m_documents.erase(m_documents.begin() + i);
                m_combo->removeItem(i);
                break;
            }
        }

        if (m_combo->count() == 0)
        {
            m_combo->addItem(QStringLiteral("(no document)"));
        }
    }

    AZ::Uuid CeeMaterialToolbar::CurrentDocumentId() const
    {
        const int idx = m_combo->currentIndex();
        if (idx >= 0 && idx < static_cast<int>(m_documents.size()))
        {
            return m_documents[idx].m_documentId;
        }
        return {};
    }

    void CeeMaterialToolbar::OnComboChanged([[maybe_unused]] int index)
    {
        const AZ::Uuid docId = CurrentDocumentId();
        if (docId.IsNull())
        {
            return;
        }

        AZStd::string path;
        AtomToolsFramework::AtomToolsDocumentRequestBus::EventResult(
            path, docId, &AtomToolsFramework::AtomToolsDocumentRequests::GetAbsolutePath);
        Q_EMIT DocumentSelected(path);

        bool canUndo = false;
        bool canRedo = false;
        AtomToolsFramework::AtomToolsDocumentRequestBus::EventResult(
            canUndo, docId, &AtomToolsFramework::AtomToolsDocumentRequests::CanUndo);
        AtomToolsFramework::AtomToolsDocumentRequestBus::EventResult(
            canRedo, docId, &AtomToolsFramework::AtomToolsDocumentRequests::CanRedo);
        UpdateUndoRedoState(canUndo, canRedo);
    }

    void CeeMaterialToolbar::OnNewClicked()
    {
        Q_EMIT NewDocumentRequested();
    }

    void CeeMaterialToolbar::OnSaveClicked()
    {
        Q_EMIT SaveRequested();
    }

    void CeeMaterialToolbar::OnCloseClicked()
    {
        Q_EMIT CloseRequested();
    }

    void CeeMaterialToolbar::OnUndoClicked()
    {
        Q_EMIT UndoRequested();
    }

    void CeeMaterialToolbar::OnRedoClicked()
    {
        Q_EMIT RedoRequested();
    }

    void CeeMaterialToolbar::UpdateUndoRedoState(bool canUndo, bool canRedo)
    {
        if (m_undoButton)
        {
            m_undoButton->setEnabled(canUndo);
        }
        if (m_redoButton)
        {
            m_redoButton->setEnabled(canRedo);
        }
    }
} // namespace CrossEngineEditor
