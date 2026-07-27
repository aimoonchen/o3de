/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Phase 4 (plan §6 4.3/4.4): real asset browser panel backed by the engine backend's
//! IAssetSource. Replaces the placeholder QLabel. The tree lazily enumerates folders
//! through the contract and shows per-entry thumbnails, so it works for any backend.

#if !defined(Q_MOC_RUN)
#include <BackendAPI/IAssetSource.h>

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <QAbstractItemModel>
#include <QTreeView>
#endif

namespace CrossEngineEditor
{
    class IAssetSource;

    //! Lazy tree model that pulls entries from an IAssetSource on demand.
    class AssetSourceModel : public QAbstractItemModel
    {
        Q_OBJECT
    public:
        explicit AssetSourceModel(IAssetSource* source, QObject* parent = nullptr);
        ~AssetSourceModel() override;

        QModelIndex index(int row, int column, const QModelIndex& parent) const override;
        QModelIndex parent(const QModelIndex& child) const override;
        int rowCount(const QModelIndex& parent) const override;
        int columnCount(const QModelIndex& parent) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        bool hasChildren(const QModelIndex& parent) const override;
        bool canFetchMore(const QModelIndex& parent) const override;
        void fetchMore(const QModelIndex& parent) override;

    private:
        struct Node
        {
            AssetEntryInfo m_info;
            Node* m_parent = nullptr;
            AZStd::vector<AZStd::unique_ptr<Node>> m_children;
            bool m_populated = false;
        };

        Node* NodeFromIndex(const QModelIndex& index) const;
        void Populate(Node* node);

        IAssetSource* m_source = nullptr;
        AZStd::unique_ptr<Node> m_root;
    };

    //! Dock panel widget hosting the asset tree view.
    class AssetBrowserPanel : public QTreeView
    {
        Q_OBJECT
    public:
        explicit AssetBrowserPanel(IAssetSource* source, QWidget* parent = nullptr);
    };
} // namespace CrossEngineEditor
