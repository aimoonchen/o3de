/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Material document for the cross-engine editor (material_migration.md §5).
//!
//! Extends AtomToolsDocument to provide the material editing workflow:
//! open / save / undo / redo / modified indicator. The backend contract
//! (IMaterialSource) supplies schema + values; this class turns them into
//! DynamicPropertyGroup trees for the stock ReflectedPropertyEditor.

#include <BackendAPI/IMaterialSource.h>

#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/RTTI/RTTI.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/containers/unordered_map.h>

// Vendor: AtomToolsFramework Core types for material editor stack
#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocument.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/DynamicProperty/DynamicProperty.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/DynamicProperty/DynamicPropertyGroup.h"

namespace CrossEngineEditor
{
    class CeeMaterialDocument final : public AtomToolsFramework::AtomToolsDocument
    {
    public:
        AZ_RTTI(
            CeeMaterialDocument,
            "{A3F7E1B2-6D9C-4B8A-AE51-3C2F9D7E8B46}",
            AtomToolsFramework::AtomToolsDocument);
        AZ_CLASS_ALLOCATOR(CeeMaterialDocument, AZ::SystemAllocator)

        CeeMaterialDocument() = default;
        CeeMaterialDocument(const AZ::Crc32& toolId, const AtomToolsFramework::DocumentTypeInfo& documentTypeInfo)
            : AtomToolsFramework::AtomToolsDocument(toolId, documentTypeInfo) {}

        static AtomToolsFramework::DocumentTypeInfo BuildDocumentTypeInfo();

        // AtomToolsDocument overrides
        AtomToolsFramework::DocumentObjectInfoVector GetObjectInfo() const override;
        bool Open(const AZStd::string& loadPath) override;
        bool Save() override;
        bool SaveAsCopy(const AZStd::string& savePath) override;
        bool SaveAsChild([[maybe_unused]] const AZStd::string& savePath) override { return false; }
        bool CanSaveAsChild() const override { return false; }
        bool IsModified() const override;
        bool BeginEdit() override;
        bool EndEdit() override;
        void Clear() override;

        //! Access this document's live material handle for preview binding.
        [[nodiscard]] MaterialHandle GetHandle() const { return m_handle; }

        //! Group name -> toggle property id mapping (S7). Populated from schema
        //! during RebuildFromBackend so the inspector can create toggle checkboxes.
        [[nodiscard]] const AZStd::unordered_map<AZStd::string, AZStd::string>& GetGroupToggleProperties() const
        {
            return m_groupToggleProperties;
        }

        //! Current property value snapshot used by the inspector to initialise toggle state.
        [[nodiscard]] const MaterialPropertyValueMap& GetCurrentValues() const
        {
            return m_valuesAtOpen;
        }

    private:
        //! Rebuild the DynamicPropertyGroup tree from the backend schema + values.
        void RebuildFromBackend();

        //! Apply a single edit to the live backend material.
        void ApplyEdit(const AZ::Name& id, const AZStd::any& value);

        //! Snapshot the current value map for undo baseline.
        MaterialPropertyValueMap SnapshotValues() const;

        //! Convert a MaterialPropertyValue to AZStd::any (variant -> any).
        static AZStd::any ValueToAny(const MaterialPropertyValue& value);

        //! Convert AZStd::any to MaterialPropertyValue (any -> variant).
        static MaterialPropertyValue AnyToValue(const AZStd::any& any, MaterialPropertyType expectedType);

        //! Build DynamicPropertyConfig from a MaterialPropertyDesc.
        AtomToolsFramework::DynamicPropertyConfig BuildPropertyConfig(
            const MaterialPropertyDesc& propDesc, const MaterialPropertyValue& currentValue);

        IMaterialSource* m_source = nullptr;          //!< borrowed from IEngineBackend
        MaterialHandle m_handle = 0;                  //!< this document's live instance
        AZStd::string m_loadPath;                     //!< absolute path used to open
        AZStd::string m_savePath;                     //!< current save target (empty = unsaved)

        AZStd::vector<AZStd::shared_ptr<AtomToolsFramework::DynamicPropertyGroup>> m_groups;

        //! Lightweight property cache built once from the schema so ApplyEdit stops re-querying.
        AZStd::unordered_map<AZStd::string, MaterialPropertyDesc> m_schemaProperties;

        MaterialPropertyValueMap m_valuesAtOpen;       //!< IsModified baseline
        MaterialPropertyValueMap m_valuesBeforeEdit;   //!< undo baseline
        bool m_undoRebuildPending = false;             //!< rebuilt-once flag for undo/redo replay
        bool m_schemaRebuildPending = false;           //!< deferred rebuild after AppliedSchemaChanged

        //! Group name -> toggle property id (S7). Populated from schema in RebuildFromBackend.
        AZStd::unordered_map<AZStd::string, AZStd::string> m_groupToggleProperties;
    };
} // namespace CrossEngineEditor
