/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! C8: asset data source feeding the reused AzToolsFramework AssetBrowser.
//!
//! Prototype stage (plan §4.3) builds AssetBrowserEntry trees directly in memory;
//! the production stage bridges to a SQLite catalog. Either way the engine backend
//! only has to enumerate its own project files and provide thumbnails.

#include <BackendAPI/BackendTypes.h>

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

#include <QIcon>

namespace CrossEngineEditor
{
    //! Minimal, engine-agnostic description of one entry in the asset tree.
    struct AssetEntryInfo
    {
        AZStd::string m_path;        //!< Absolute or project-relative path.
        AZStd::string m_displayName;
        AZStd::string m_extension;
        bool m_isFolder = false;
    };

    class IAssetSource
    {
    public:
        virtual ~IAssetSource() = default;

        //! Top-level entries under the engine project root.
        virtual void EnumerateRoot(AZStd::vector<AssetEntryInfo>& out) = 0;

        //! Children of a folder entry.
        virtual void EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out) = 0;

        //! Thumbnail/icon for an entry (by extension or rendered preview).
        virtual QIcon GetThumbnail(const AssetEntryInfo& entry) = 0;
    };
} // namespace CrossEngineEditor
