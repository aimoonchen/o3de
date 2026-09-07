/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/CeeMaterialDocumentInspector.h>

#include <MaterialEditor/CeeInspectorGroupHeaderWidget.h>
#include <MaterialEditor/CeeMaterialDocument.h>
#include <MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentRequestBus.h>
#include <MaterialEditor/Vendor/AtomToolsFramework/Inspector/InspectorGroupHeaderWidget.h>

namespace CrossEngineEditor
{
    CeeMaterialDocumentInspector::CeeMaterialDocumentInspector(const AZ::Crc32& toolId, QWidget* parent)
        : AtomToolsFramework::AtomToolsDocumentInspector(toolId, parent)
    {
    }

    void CeeMaterialDocumentInspector::SetDocumentId(const AZ::Uuid& documentId)
    {
        // Build the toggle-property map before the base class Populates the inspector,
        // because Populate() calls AddGroup -> CreateGroupHeader for each group.
        if (auto* requests = AtomToolsFramework::AtomToolsDocumentRequestBus::FindFirstHandler(documentId))
        {
            if (auto* doc = static_cast<CeeMaterialDocument*>(requests))
            {
                BuildToggleMapFromDocument(doc);
            }
        }

        // Reset group tracking for the upcoming Populate() pass.
        m_currentGroupIndex = 0;
        m_populating = true;

        // Call base class which triggers Populate() -> AddGroup() -> CreateGroupHeader().
        AtomToolsDocumentInspector::SetDocumentId(documentId);

        m_populating = false;
    }

    AtomToolsFramework::InspectorGroupHeaderWidget* CeeMaterialDocumentInspector::CreateGroupHeader(QWidget* parent)
    {
        if (!m_populating)
        {
            // Not inside Populate() -- fall back to plain header.
            return AtomToolsDocumentInspector::CreateGroupHeader(parent);
        }

        // Look up the toggle property for the current group.
        const int idx = m_currentGroupIndex;
        m_currentGroupIndex++;

        if (idx < static_cast<int>(m_groupNames.size()))
        {
            const AZStd::string& groupName = m_groupNames[idx];
            auto it = m_toggleProperties.find(groupName);
            if (it != m_toggleProperties.end() && !it->second.empty())
            {
                const AZStd::string& togglePropId = it->second;

                // Capture the document ID for the callback.
                AZ::Uuid docId = GetDocumentId();

                auto itInit = m_toggleInitialStates.find(groupName);
                return new CeeInspectorGroupHeaderWidget(
                    togglePropId,
                    [docId, togglePropId](bool newState)
                    {
                        // Toggle the associated bool property on the active document.
                        // The RPE callback path handles undo tracking via AfterPropertyModified;
                        // for header clicks we bracket with BeginEdit/EndEdit for consistency.
                        AtomToolsFramework::AtomToolsDocumentRequestBus::Event(docId, &AtomToolsFramework::AtomToolsDocumentRequests::BeginEdit);

                        AtomToolsFramework::AtomToolsDocumentRequestBus::Event(
                            docId,
                            [togglePropId, newState](AtomToolsFramework::AtomToolsDocumentRequests* docRequests)
                            {
                                auto* document = static_cast<CeeMaterialDocument*>(docRequests);
                                if (!document)
                                {
                                    return;
                                }

                                for (auto& objInfo : document->GetObjectInfo())
                                {
                                    auto* group = static_cast<AtomToolsFramework::DynamicPropertyGroup*>(objInfo.m_objectPtr);
                                    if (!group)
                                    {
                                        continue;
                                    }
                                    for (auto& prop : group->m_properties)
                                    {
                                        if (AZ::Name(prop.GetId()) == AZ::Name(togglePropId.c_str()))
                                        {
                                            prop.SetValue(AZStd::any(newState));
                                            return;
                                        }
                                    }
                                }
                            });

                        AtomToolsFramework::AtomToolsDocumentRequestBus::Event(docId, &AtomToolsFramework::AtomToolsDocumentRequests::EndEdit);
                    },
                    itInit != m_toggleInitialStates.end() ? itInit->second : false,
                    parent);
            }
        }

        // No toggle property: use the default group header.
        return AtomToolsDocumentInspector::CreateGroupHeader(parent);
    }

    bool CeeMaterialDocumentInspector::ShouldGroupAutoExpanded(const AZStd::string& groupName) const
    {
        auto it = m_defaultCollapsed.find(groupName);
        if (it != m_defaultCollapsed.end() && it->second)
        {
            return false;
        }
        return true;
    }

    void CeeMaterialDocumentInspector::BuildToggleMapFromDocument(const CeeMaterialDocument* doc)
    {
        m_toggleProperties.clear();
        m_groupNames.clear();
        m_toggleInitialStates.clear();
        m_defaultCollapsed.clear();

        if (!doc)
        {
            return;
        }

        // Read group names from GetObjectInfo() (same order as Populate() processes them).
        AtomToolsFramework::DocumentObjectInfoVector objectInfo = doc->GetObjectInfo();
        m_groupNames.reserve(objectInfo.size());
        for (const auto& info : objectInfo)
        {
            m_groupNames.push_back(info.m_name);
        }

        // Read toggle properties from the document's schema-derived map.
        m_toggleProperties = doc->GetGroupToggleProperties();

        // Read defaultCollapsed from the document's live schema.
        for (const auto& info : doc->GetObjectInfo())
        {
            auto it = m_toggleProperties.find(info.m_name);
            if (it != m_toggleProperties.end())
            {
                m_defaultCollapsed[info.m_name] = false; // toggle groups default expanded
            }
        }

        // Snapshot the current toggle values so the header widget starts in the correct state.
        const auto values = doc->GetCurrentValues();
        for (const auto& [groupName, propId] : m_toggleProperties)
        {
            bool initialState = false;
            if (!propId.empty())
            {
                auto it = values.find(propId);
                if (it != values.end())
                {
                    initialState = AZStd::get_if<bool>(&(it->second)) && AZStd::get<bool>(it->second);
                }
            }
            m_toggleInitialStates[groupName] = initialState;
        }
    }

} // namespace CrossEngineEditor
