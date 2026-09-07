/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/CeeAssetBrowserPanel.h>

#include <Application/EntityMirrorBridge.h>

#include <AzCore/Console/IConsole.h>
#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/std/containers/unordered_map.h>

#include <AzToolsFramework/AzToolsFrameworkAPI.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserModel.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserFilterModel.h>
#include <AzToolsFramework/AssetBrowser/Views/AssetBrowserTreeView.h>
#include <AzToolsFramework/UI/UICore/QTreeViewStateSaver.hxx>

// Vendor: AtomToolsFramework Document system request bus
#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentSystemRequestBus.h"

#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

//! AzToolsFramework CVar (defined in AzToolsFrameworkModule.cpp, externed in
//! AssetBrowserFilterModel.cpp) that switches the tree to the path+name column
//! layout. CEE entries carry no display path, so keep the classic single "Name"
//! column for v1 (rbfx_migration.md §2.6: TreeView column 0 only). Must be set
//! before the filter model is constructed.
AZ_CVAR_API_EXTERNED(AZTF_API, bool, ed_useNewAssetBrowserListView);

namespace CrossEngineEditor
{
    namespace
    {
        constexpr const char* IconDir = "/Assets/Editor/Icons/AssetBrowser/";
        constexpr const char* FolderIcon = "Folder_80.svg";
        constexpr const char* DefaultIcon = "Default_16.svg";

        //! Absolute path of one of the engine's stock AssetBrowser icons. The tree
        //! delegate accepts absolute paths unchanged (AssetBrowserViewUtils.cpp
        //! findIconPath), so no Qt resource or asset-system resolution is involved.
        AZStd::string IconPath(const char* iconFile)
        {
            AZStd::string result(AZ::Utils::GetEnginePath().c_str());
            result += IconDir;
            result += iconFile;
            return result;
        }

        //! rbfx-facing extension table. scene/material/prefab/terrain are XML on
        //! disk; .mdl is rbfx's compiled model format.
        const char* IconFileForExtension(const AZStd::string& extension)
        {
            static const AZStd::unordered_map<AZStd::string, const char*> kIconByExtension = {
                { "xml", "XML_80.svg" },      { "scene", "XML_80.svg" },    { "material", "XML_80.svg" },
                { "prefab", "XML_80.svg" },   { "terrain", "XML_80.svg" },
                { "png", "PNG_80.svg" },      { "jpg", "PNG_80.svg" },      { "jpeg", "PNG_80.svg" },
                { "bmp", "PNG_80.svg" },      { "tga", "PNG_80.svg" },      { "dds", "PNG_80.svg" },
                { "ktx", "PNG_80.svg" },
                { "fbx", "FBX_80.svg" },      { "gltf", "FBX_80.svg" },     { "glb", "FBX_80.svg" },
                { "mdl", "FBX_80.svg" },      { "blend", "FBX_80.svg" },
                { "wav", "WAV_80.svg" },      { "ogg", "WAV_80.svg" },      { "mp3", "WAV_80.svg" },
                { "flac", "WAV_80.svg" },
                { "json", "JSON_80.svg" },
                { "lua", "Lua_80.svg" },      { "as", "Lua_80.svg" },
                { "hlsl", "Shader_80.svg" },  { "glsl", "Shader_80.svg" },  { "vert", "Shader_80.svg" },
                { "frag", "Shader_80.svg" },  { "comp", "Shader_80.svg" },  { "spirv", "Shader_80.svg" },
                { "ttf", "Font_80.svg" },     { "otf", "Font_80.svg" },
                { "txt", "TXT_80.svg" },      { "md", "TXT_80.svg" },       { "log", "TXT_80.svg" },
                { "cpp", "CPP_80.svg" },      { "h", "CPP_80.svg" },        { "hpp", "CPP_80.svg" },
                { "c", "CPP_80.svg" },        { "inl", "CPP_80.svg" },
                { "py", "PY_80.svg" },
            };

            const auto it = kIconByExtension.find(extension);
            return it != kIconByExtension.end() ? it->second : DefaultIcon;
        }
    } // namespace

    // ------------------------------------------------------------- CeeRootEntry

    CeeRootEntry::CeeRootEntry()
    {
        m_name = "Assets";
        m_displayName = QStringLiteral("Assets");
    }

    void CeeRootEntry::UpdateChildPaths(AssetBrowserEntry* /*child*/) const
    {
        // Deliberately no-op: RootAssetBrowserEntry rebases children onto the root's
        // own path (official DB/scan-folder flow); the CEE builder sets each entry's
        // absolute path through SetFullPath. The thumbnail keys the official chain
        // would refresh are unused by the v1 TreeView render path (icons resolve in
        // AssetBrowserViewUtils::GetThumbnail).
    }

    // ------------------------------------------------------------- entry construction

    CeeFolderEntry::CeeFolderEntry(const AssetEntryInfo& info)
    {
        m_name = info.m_displayName;
        m_displayName = QString::fromUtf8(info.m_displayName.c_str());
        m_diskSize = 0;
        SetFullPath(AZ::IO::Path(info.m_path.c_str()));
    }

    CeeSourceEntry::CeeSourceEntry(const AssetEntryInfo& info)
    {
        m_name = info.m_displayName;
        m_displayName = QString::fromUtf8(info.m_displayName.c_str());
        m_diskSize = 0;
        SetFullPath(AZ::IO::Path(info.m_path.c_str()));
    }

    // ------------------------------------------------------------- icon provider

    AzToolsFramework::AssetBrowser::SourceFileDetails CeeAssetBrowserIconProvider::GetSourceFileDetails(
        const char* fullSourceFileName)
    {
        // The tree root has no path; give it the folder icon.
        if (fullSourceFileName == nullptr || fullSourceFileName[0] == '\0')
        {
            const AZStd::string iconPath = IconPath(FolderIcon);
            return AzToolsFramework::AssetBrowser::SourceFileDetails(iconPath.c_str());
        }

        AZStd::string extension;
        if (!AZ::StringFunc::Path::GetExtension(fullSourceFileName, extension, false))
        {
            const AZStd::string iconPath = IconPath(DefaultIcon);
            return AzToolsFramework::AssetBrowser::SourceFileDetails(iconPath.c_str());
        }
        for (char& c : extension)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c += 'a' - 'A';
            }
        }

        const AZStd::string iconPath = IconPath(IconFileForExtension(extension));
        return AzToolsFramework::AssetBrowser::SourceFileDetails(iconPath.c_str());
    }

    // ------------------------------------------------------------- panel

    CeeAssetBrowserPanel::CeeAssetBrowserPanel(
        IAssetSource* source, EntityMirrorBridge* mirrorBridge, QWidget* parent)
        : QWidget(parent)
        , m_source(source)
        , m_mirrorBridge(mirrorBridge)
    {
        ed_useNewAssetBrowserListView = false; // before the filter model reads it

        m_iconProvider.BusConnect();

        // Official minimal wiring recipe (AssetBrowserComponent.cpp:44-59 minus the
        // database/socket machinery). The root is set before any proxy or view attaches
        // per the official recipe (AssetBrowserModel guards a null root by returning an
        // invalid index - ordering hygiene, not crash defense).
        m_model = aznew AzToolsFramework::AssetBrowser::AssetBrowserModel(this);
        BuildTree();

        m_filterModel = aznew AzToolsFramework::AssetBrowser::AssetBrowserFilterModel(this, false);
        m_filterModel->setSourceModel(m_model);

        m_treeView = new AzToolsFramework::AssetBrowser::AssetBrowserTreeView(this);
        m_treeView->setModel(m_filterModel); // asserts it's a FilterModel, sorts by name

        connect(m_treeView, &QTreeView::doubleClicked, this,
            [this](const QModelIndex& index) { OpenOrAssignEntry(index); });

        auto* refreshButton = new QToolButton(this);
        refreshButton->setText(QStringLiteral("Refresh"));
        refreshButton->setToolTip(QStringLiteral("Re-enumerate the asset source"));
        connect(refreshButton, &QToolButton::clicked, this, [this]() { Refresh(); });

        auto* toolbar = new QHBoxLayout();
        toolbar->addWidget(refreshButton);
        toolbar->addStretch(1);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addLayout(toolbar);
        layout->addWidget(m_treeView);

        ExpandRoot();
    }

    CeeAssetBrowserPanel::~CeeAssetBrowserPanel()
    {
        m_iconProvider.BusDisconnect();
    }

    void CeeAssetBrowserPanel::Refresh()
    {
        // Keep the user's expansion across the full re-enumeration (editor_polish.md P2; the
        // stock TreeViewState helper works on any QTreeView - the AssetBrowserTreeView is a
        // framework widget we cannot rebase onto QTreeViewWithStateSaving). Captured BEFORE the
        // tree swap, applied after; the model reset would otherwise collapse everything.
        AZStd::unique_ptr<AzToolsFramework::TreeViewState> treeState(AzToolsFramework::TreeViewState::CreateTreeViewState());
        treeState->CaptureSnapshot(m_treeView);

        BuildTree();
        ExpandRoot();

        treeState->ApplySnapshot(m_treeView);
    }

    void CeeAssetBrowserPanel::BuildTree()
    {
        AZStd::shared_ptr<CeeRootEntry> newRoot = AZStd::make_shared<CeeRootEntry>();
        if (m_source)
        {
            AZStd::vector<AssetEntryInfo> entries;
            m_source->EnumerateRoot(entries);
            for (const AssetEntryInfo& info : entries)
            {
                if (info.m_isFolder)
                {
                    CeeFolderEntry* folder = CreateFolderEntry(info);
                    newRoot->AddEntry(folder);
                    AddFolderChildren(folder, info);
                }
                else
                {
                    newRoot->AddEntry(CreateSourceEntry(info));
                }
            }
        }

        // Swap the root, then reset the model. Releasing the old shared_ptr here
        // cascade-deletes the old tree (RemoveChild wraps children in unique_ptr,
        // rbfx_migration.md §2.7); nothing may hold an old entry pointer past this.
        m_root = newRoot;
        m_model->SetRootEntry(newRoot);
        m_model->BeginReset();
        m_model->EndReset();
    }

    void CeeAssetBrowserPanel::AddFolderChildren(CeeFolderEntry* folder, const AssetEntryInfo& folderInfo)
    {
        if (!m_source)
        {
            return;
        }

        AZStd::vector<AssetEntryInfo> entries;
        m_source->EnumerateChildren(folderInfo, entries);
        for (const AssetEntryInfo& info : entries)
        {
            if (info.m_isFolder)
            {
                CeeFolderEntry* child = CreateFolderEntry(info);
                folder->AddEntry(child);
                AddFolderChildren(child, info);
            }
            else
            {
                folder->AddEntry(CreateSourceEntry(info));
            }
        }
    }

    CeeFolderEntry* CeeAssetBrowserPanel::CreateFolderEntry(const AssetEntryInfo& info)
    {
        return aznew CeeFolderEntry(info);
    }

    CeeSourceEntry* CeeAssetBrowserPanel::CreateSourceEntry(const AssetEntryInfo& info)
    {
        return aznew CeeSourceEntry(info);
    }

    void CeeAssetBrowserPanel::ExpandRoot()
    {
        // Keep the top-level "Assets" node expanded so the tree reads as a browser.
        const QModelIndex rootIndex = m_model->index(0, 0);
        if (rootIndex.isValid())
        {
            m_treeView->expand(m_filterModel->mapFromSource(rootIndex));
        }
    }

    void CeeAssetBrowserPanel::OpenOrAssignEntry(const QModelIndex& index)
    {
        // Migration L1 (rbfx_migration.md §3.4 双击打开). The official index -> entry
        // pattern is internalPointer on the SOURCE index (AssetBrowserTreeView.cpp:716).
        const QModelIndex sourceIndex = m_filterModel->mapToSource(index);
        const auto* entry = sourceIndex.isValid()
            ? static_cast<const AzToolsFramework::AssetBrowser::AssetBrowserEntry*>(sourceIndex.internalPointer())
            : nullptr;
        if (entry == nullptr
            || entry->GetEntryType() != AzToolsFramework::AssetBrowser::AssetBrowserEntry::AssetEntryType::Source)
        {
            return; // folders just expand/collapse
        }

        const AZ::IO::Path fullPath = entry->GetFullPath();
        const AZStd::string path(fullPath.c_str());
        AZStd::string extension;
        if (!AZ::StringFunc::Path::GetExtension(path.c_str(), extension, false))
        {
            return;
        }

        if (azstricmp(extension.c_str(), "mdl") == 0 || azstricmp(extension.c_str(), "xml") == 0)
        {
            if (m_mirrorBridge)
            {
                m_mirrorBridge->SpawnAssetAtOrigin(path);
            }
            return;
        }
        // Material double-click opens the document (AtomToolsDocumentSystemRequestBus).
        if (azstricmp(extension.c_str(), "mat") == 0 || azstricmp(extension.c_str(), "material") == 0)
        {
            AtomToolsFramework::AtomToolsDocumentSystemRequestBus::Broadcast(
                &AtomToolsFramework::AtomToolsDocumentSystemRequestBus::Events::OpenDocument, path);
            return;
        }
        if (azstricmp(extension.c_str(), "ani") == 0)
        {
            // TODO: Animation document (v2).
            return;
        }

        // Anything else has no open action in v1; say so on the Console trace panel.
        AZ_Warning("CrossEngineEditor", false,
            "AssetBrowser double-click: no open action for .%s yet (v1 spawns .mdl/.xml, opens .mat/.ani).",
            extension.c_str());
    }
} // namespace CrossEngineEditor
