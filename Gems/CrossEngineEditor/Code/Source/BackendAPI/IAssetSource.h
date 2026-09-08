/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Asset data source feeding the reused AzToolsFramework AssetBrowser
//! (Plan §A6 C4 asset face / C5 integration cost: 2 enumeration pure-virtual only).
//!
//! The consumer (Window/CeeAssetBrowserPanel) builds AssetBrowserEntry trees in
//! memory from the enumerations below and injects them into the official
//! AssetBrowserModel / AssetBrowserFilterModel / AssetBrowserTreeView stack.
//! Wiring and verified API ledger: rbfx_migration.md §2 (reuse path, §2.1 wiring recipe).
//! The engine backend only has to enumerate its own project files; tree icons are
//! resolved editor-side through the official AssetBrowser interaction bus by
//! extension (CeeAssetBrowserIconProvider, rbfx_migration.md §2.3) — the contract
//! deliberately carries no Qt types and no icon semantics (review 2026-08-17).

#include <BackendAPI/BackendTypes.h>

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace CrossEngineEditor
{
    //! Minimal, engine-agnostic description of one entry in the asset tree.
    struct AssetEntryInfo
    {
        AZStd::string m_path;        //!< Absolute or project-relative path.
        AZStd::string m_displayName;
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
    };
} // namespace CrossEngineEditor
