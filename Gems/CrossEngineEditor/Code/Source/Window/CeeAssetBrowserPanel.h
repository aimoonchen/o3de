/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! CEE Asset Browser - 100% reuse of the AzToolsFramework AssetBrowser view stack
//! (AssetBrowserModel -> AssetBrowserFilterModel -> AssetBrowserTreeView).
//! Only the data source is replaced (plan rule C3): the engine backend's IAssetSource
//! enumeration feeds an in-memory entry tree injected through the public
//! AssetBrowserModel::SetRootEntry. Design + verified API ledger: rbfx_migration.md §2.
//! The whole-component AssetBrowserComponent is intentionally not used (it requires an
//! AssetProcessor socket connection and an AssetDatabase, rbfx_migration.md §2.4).

#include <BackendAPI/IAssetSource.h>

#include <AzCore/RTTI/RTTI.h>
#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>

#include <AzToolsFramework/AssetBrowser/AssetBrowserBus.h>
#include <AzToolsFramework/AssetBrowser/Entries/RootAssetBrowserEntry.h>
#include <AzToolsFramework/AssetBrowser/Entries/FolderAssetBrowserEntry.h>
#include <AzToolsFramework/AssetBrowser/Entries/SourceAssetBrowserEntry.h>

#include <QString>
#include <QWidget>

class QModelIndex;

namespace AzToolsFramework
{
    namespace AssetBrowser
    {
        class AssetBrowserModel;
        class AssetBrowserFilterModel;
        class AssetBrowserTreeView;
        class SearchWidget;
    } // namespace AssetBrowser
} // namespace AzToolsFramework

namespace CrossEngineEditor
{
    class EntityMirrorBridge;

    //! Root entry of the CEE asset tree. The official RootAssetBrowserEntry rebases
    //! its children onto its own path in UpdateChildPaths (DB/scan-folder semantics);
    //! CEE entries carry absolute paths set through SetFullPath, so that rebase is
    //! disabled here (rbfx_migration.md §2.2).
    class CeeRootEntry : public AzToolsFramework::AssetBrowser::RootAssetBrowserEntry
    {
    public:
        AZ_RTTI(CeeRootEntry, "{A9B7F5E8-1C2D-4E6F-8A9B-3D4E5F6A7B8C}", AzToolsFramework::AssetBrowser::RootAssetBrowserEntry);
        AZ_CLASS_ALLOCATOR(CeeRootEntry, AZ::SystemAllocator);

        CeeRootEntry();

        //! AddChild is protected on AssetBrowserEntry; expose it for the tree builder.
        //! Subclassing is the official workaround for the friend-gating (rbfx_migration.md §2.2).
        void AddEntry(AzToolsFramework::AssetBrowser::AssetBrowserEntry* child) { AddChild(child); }

    protected:
        void UpdateChildPaths(AzToolsFramework::AssetBrowser::AssetBrowserEntry* child) const override;
    };

    //! Folder entry of the CEE asset tree (name/children settable by the builder).
    class CeeFolderEntry : public AzToolsFramework::AssetBrowser::FolderAssetBrowserEntry
    {
    public:
        AZ_RTTI(CeeFolderEntry, "{B8C6E4D7-2B3C-5D7E-9A8C-4E5F6A7B8C9D}", AzToolsFramework::AssetBrowser::FolderAssetBrowserEntry);
        AZ_CLASS_ALLOCATOR(CeeFolderEntry, AZ::SystemAllocator);

        explicit CeeFolderEntry(const AssetEntryInfo& info);

        void AddEntry(AzToolsFramework::AssetBrowser::AssetBrowserEntry* child) { AddChild(child); }
    };

    //! Source (file) entry of the CEE asset tree. GetExtension()/GetFileName() work
    //! because they derive from the full path (SourceAssetBrowserEntry.cpp), unlike
    //! the private m_extension member the official DB flow fills.
    class CeeSourceEntry : public AzToolsFramework::AssetBrowser::SourceAssetBrowserEntry
    {
    public:
        AZ_RTTI(CeeSourceEntry, "{C7D5F3E6-3A4B-6C8D-8B9D-5F6A7B8C9D0E}", AzToolsFramework::AssetBrowser::SourceAssetBrowserEntry);
        AZ_CLASS_ALLOCATOR(CeeSourceEntry, AZ::SystemAllocator);

        explicit CeeSourceEntry(const AssetEntryInfo& info);
    };

    //! Answers AssetBrowserInteractionNotificationBus::GetSourceFileDetails with the
    //! engine's stock per-extension icons. This is the official icon extension point
    //! consumed by AssetBrowserViewUtils::GetThumbnail / the TreeView EntryDelegate;
    //! no ThumbnailerService is required (rbfx_migration.md §2.3).
    class CeeAssetBrowserIconProvider
        : public AzToolsFramework::AssetBrowser::AssetBrowserInteractionNotificationBus::Handler
    {
    public:
        //! Answer before the default (empty) handlers.
        AZ::s32 GetPriority() const override { return 1; }

        AzToolsFramework::AssetBrowser::SourceFileDetails GetSourceFileDetails(const char* fullSourceFileName) override;
    };

    //! Dock panel hosting the reused AssetBrowser widget stack, fed by an IAssetSource.
    class CeeAssetBrowserPanel : public QWidget
    {
    public:
        explicit CeeAssetBrowserPanel(
            IAssetSource* source, EntityMirrorBridge* mirrorBridge, QWidget* parent = nullptr);
        ~CeeAssetBrowserPanel() override;

        //! Rebuild the whole entry tree from the source and reset the model.
        //! v1 rebuilds instead of incrementally updating (KISS, rbfx_migration.md §2.6).
        void Refresh();

        //! Target of resource-type double-clicks (.material/.mat): AtomToolsDocumentSystemRequestBus.
        void OpenOrAssignEntry(const QModelIndex& index);

    private:
        void BuildTree();
        void AddFolderChildren(CeeFolderEntry* folder, const AssetEntryInfo& folderInfo);
        CeeFolderEntry* CreateFolderEntry(const AssetEntryInfo& info);
        CeeSourceEntry* CreateSourceEntry(const AssetEntryInfo& info);
        void ExpandRoot();

        IAssetSource* m_source = nullptr;
        EntityMirrorBridge* m_mirrorBridge = nullptr;
        CeeAssetBrowserIconProvider m_iconProvider; // interaction-bus handler, connected in ctor
        AzToolsFramework::AssetBrowser::AssetBrowserModel* m_model = nullptr; // aznew, parented to this
        AzToolsFramework::AssetBrowser::AssetBrowserFilterModel* m_filterModel = nullptr; // parented to this
        AzToolsFramework::AssetBrowser::AssetBrowserTreeView* m_treeView = nullptr; // parented to this
        //! Stock search box (same widget the native Asset Browser composes); its composite
        //! filter is handed to the filter model once, then drives it via updatedSignal.
        AzToolsFramework::AssetBrowser::SearchWidget* m_searchWidget = nullptr; // parented to this
        //! Shares ownership of the entry tree with the model; releasing it after a
        //! refresh cascade-deletes the old tree (rbfx_migration.md §2.7).
        AZStd::shared_ptr<AzToolsFramework::AssetBrowser::RootAssetBrowserEntry> m_root;
    };
} // namespace CrossEngineEditor
