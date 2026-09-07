/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentInspector.h"

#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace CrossEngineEditor
{
    class CeeMaterialDocument;

    //! Extended document inspector that creates toggle-property group headers (S7).
    //! When a MaterialPropertyGroupDesc has m_toggleProperty non-empty, the group header
    //! gets a checkbox that toggles the associated bool property.
    //!
    //! Implementation approach: override SetDocumentId to capture the toggle-property map
    //! before Populate() runs. Override CreateGroupHeader to return CeeInspectorGroupHeaderWidget
    //! for groups that have a toggle property. Uses m_populating flag and m_currentGroupIndex
    //! to correlate groups during the Populate() pass.
    class CeeMaterialDocumentInspector final : public AtomToolsFramework::AtomToolsDocumentInspector
    {
        Q_OBJECT
    public:
        explicit CeeMaterialDocumentInspector(const AZ::Crc32& toolId, QWidget* parent = nullptr);

        //! Override to store the document ID and rebuild the toggle-property map from schema.
        void SetDocumentId(const AZ::Uuid& documentId);

        [[nodiscard]] AZ::Uuid GetDocumentId() const { return m_documentId; }

    protected:
        //! Override to return CeeInspectorGroupHeaderWidget for groups with toggle properties.
        AtomToolsFramework::InspectorGroupHeaderWidget* CreateGroupHeader(QWidget* parent) override;

        //! Respect schema m_defaultCollapsed when user has no persisted choice.
        bool ShouldGroupAutoExpanded(const AZStd::string& groupName) const override;

    private:
        //! Build toggle-property map from CeeMaterialDocument.
        void BuildToggleMapFromDocument(const CeeMaterialDocument* doc);

        //! Map of group name -> toggle property id, populated from schema.
        AZStd::unordered_map<AZStd::string, AZStd::string> m_toggleProperties;

        //! Map of group name -> initial toggle state when the document was loaded.
        AZStd::unordered_map<AZStd::string, bool> m_toggleInitialStates;

        //! Map of group name -> defaultCollapsed from schema.
        AZStd::unordered_map<AZStd::string, bool> m_defaultCollapsed;

        //! Current group index during Populate(), incremented by AddGroup calls.
        //! Set to 0 at the start of Populate() (via AddGroupsBegin).
        int m_currentGroupIndex = 0;

        //! Group names in order, populated during Populate() from the document's ObjectInfo.
        //! Created fresh each time SetDocumentId is called.
        AZStd::vector<AZStd::string> m_groupNames;

        //! Flag: true while Populate() is running so CreateGroupHeader can use m_currentGroupIndex.
        bool m_populating = false;
    };
} // namespace CrossEngineEditor
