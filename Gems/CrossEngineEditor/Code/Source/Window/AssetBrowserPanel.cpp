/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/AssetBrowserPanel.h>

#include <QHeaderView>

namespace CrossEngineEditor
{
    AssetSourceModel::AssetSourceModel(IAssetSource* source, QObject* parent)
        : QAbstractItemModel(parent)
        , m_source(source)
    {
        // Synthetic root; its children are the source's top-level entries.
        m_root = AZStd::make_unique<Node>();
    }

    AssetSourceModel::~AssetSourceModel() = default;

    AssetSourceModel::Node* AssetSourceModel::NodeFromIndex(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return m_root.get();
        }
        return static_cast<Node*>(index.internalPointer());
    }

    void AssetSourceModel::Populate(Node* node)
    {
        if (node->m_populated || m_source == nullptr)
        {
            return;
        }
        node->m_populated = true;

        AZStd::vector<AssetEntryInfo> entries;
        if (node == m_root.get())
        {
            m_source->EnumerateRoot(entries);
        }
        else
        {
            m_source->EnumerateChildren(node->m_info, entries);
        }

        node->m_children.reserve(entries.size());
        for (AssetEntryInfo& info : entries)
        {
            auto child = AZStd::make_unique<Node>();
            child->m_info = AZStd::move(info);
            child->m_parent = node;
            node->m_children.push_back(AZStd::move(child));
        }
    }

    QModelIndex AssetSourceModel::index(int row, int column, const QModelIndex& parent) const
    {
        if (column != 0)
        {
            return {};
        }
        Node* parentNode = NodeFromIndex(parent);
        if (row < 0 || row >= static_cast<int>(parentNode->m_children.size()))
        {
            return {};
        }
        return createIndex(row, column, parentNode->m_children[row].get());
    }

    QModelIndex AssetSourceModel::parent(const QModelIndex& child) const
    {
        Node* node = NodeFromIndex(child);
        if (node == nullptr || node->m_parent == nullptr || node->m_parent == m_root.get())
        {
            return {};
        }
        Node* grandParent = node->m_parent->m_parent;
        const auto& siblings = grandParent->m_children;
        for (int row = 0; row < static_cast<int>(siblings.size()); ++row)
        {
            if (siblings[row].get() == node->m_parent)
            {
                return createIndex(row, 0, node->m_parent);
            }
        }
        return {};
    }

    int AssetSourceModel::rowCount(const QModelIndex& parent) const
    {
        Node* node = NodeFromIndex(parent);
        return static_cast<int>(node->m_children.size());
    }

    int AssetSourceModel::columnCount(const QModelIndex& /*parent*/) const
    {
        return 1;
    }

    QVariant AssetSourceModel::data(const QModelIndex& index, int role) const
    {
        Node* node = NodeFromIndex(index);
        if (node == nullptr || node == m_root.get())
        {
            return {};
        }
        if (role == Qt::DisplayRole)
        {
            return QString::fromUtf8(node->m_info.m_displayName.c_str());
        }
        if (role == Qt::DecorationRole && m_source != nullptr)
        {
            return m_source->GetThumbnail(node->m_info);
        }
        return {};
    }

    bool AssetSourceModel::hasChildren(const QModelIndex& parent) const
    {
        Node* node = NodeFromIndex(parent);
        if (node == m_root.get())
        {
            return true;
        }
        return node->m_info.m_isFolder;
    }

    bool AssetSourceModel::canFetchMore(const QModelIndex& parent) const
    {
        Node* node = NodeFromIndex(parent);
        return !node->m_populated && (node == m_root.get() || node->m_info.m_isFolder);
    }

    void AssetSourceModel::fetchMore(const QModelIndex& parent)
    {
        Node* node = NodeFromIndex(parent);
        if (node->m_populated)
        {
            return;
        }
        Populate(node);
        const int count = static_cast<int>(node->m_children.size());
        if (count > 0)
        {
            beginInsertRows(parent, 0, count - 1);
            endInsertRows();
        }
    }

    AssetBrowserPanel::AssetBrowserPanel(IAssetSource* source, QWidget* parent)
        : QTreeView(parent)
    {
        setModel(new AssetSourceModel(source, this));
        setHeaderHidden(true);
        setUniformRowHeights(true);
    }
} // namespace CrossEngineEditor
