/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/ResourcePropertiesPanel.h>

#include <Application/EntityMirrorBridge.h>
#include <Framework/EngineNodeComponent.h>

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/IO/Path/Path.h>

#include <AzToolsFramework/UI/PropertyEditor/EntityPropertyEditor.hxx>

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace CrossEngineEditor
{
    ResourcePropertiesPanel::ResourcePropertiesPanel(EntityMirrorBridge* bridge, QWidget* parent)
        : QWidget(parent)
        , m_bridge(bridge)
    {
        m_pathLabel = new QLabel(QStringLiteral("No resource open"), this);
        m_pathLabel->setWordWrap(true);

        // Save persists the edited cached resource to its source file (rbfx_migration.md
        // §3.3); enabled only while a resource is open.
        m_saveButton = new QPushButton(QStringLiteral("Save"), this);
        m_saveButton->setEnabled(false);
        connect(m_saveButton, &QPushButton::clicked, this, &ResourcePropertiesPanel::OnSaveClicked);

        auto* headerRow = new QHBoxLayout;
        headerRow->addWidget(m_pathLabel, 1);
        headerRow->addWidget(m_saveButton);

        // The reused AzToolsFramework property grid; SetOverrideEntityIds pins it to the
        // detached host entity, independent of the editor selection.
        m_propertyEditor = new AzToolsFramework::EntityPropertyEditor(this);
        // HideComponentPalette is private, so the palette header stays visible; it is inert
        // (adding a component to the detached host never touches the engine). Cosmetic v1 note.

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addLayout(headerRow);
        layout->addWidget(m_propertyEditor, 1);
    }

    ResourcePropertiesPanel::~ResourcePropertiesPanel()
    {
        CloseHostEntity();
    }

    bool ResourcePropertiesPanel::OpenResource(const AZStd::string& type, const AZStd::string& path)
    {
        CloseHostEntity();

        m_type = type;
        m_path = path;
        return CreateHostEntity();
    }

    bool ResourcePropertiesPanel::CreateHostEntity()
    {
        // Reflect the resource into a PropertyBag through the contract (rbfx_migration.md
        // §3.3: rbfx reflects Serializable attributes / Material shader parameters). A failed
        // read leaves the grid empty; the backend warned on the Console.
        PropertyBag bag;
        if (!m_bridge || !m_bridge->ReadResourceProperties(m_type, m_path, bag))
        {
            m_pathLabel->setText(
                QStringLiteral("Not editable (no property surface): %1").arg(QString::fromUtf8(m_path.c_str())));
            m_saveButton->setEnabled(false);
            return false;
        }

        // Host entity: EngineNodeComponent reuses the exact dynamic edit-data grid mirror
        // entities use in the Inspector. Registered with the component application ONLY, so it
        // stays invisible to the Outliner / selection / gizmo.
        const AZStd::string entityName(AZ::IO::PathView(m_path).Filename().Native());
        m_hostEntity = aznew AZ::Entity(entityName.c_str());
        auto* nodeComp = m_hostEntity->CreateComponent<EngineNodeComponent>();
        nodeComp->SetMirrorData(m_type, AZStd::move(bag));
        m_hostEntity->Init();
        AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationBus::Events::AddEntity, m_hostEntity);
        m_hostEntity->Activate();

        // Any change pushes the whole bag back (the backend writes name-matched values only).
        // Full-bag write per change is v1 KISS: no undo integration, so no diff bookkeeping.
        AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler::BusConnect(
            m_hostEntity->GetId());

        AzToolsFramework::EntityIdSet ids;
        ids.insert(m_hostEntity->GetId());
        m_propertyEditor->SetOverrideEntityIds(ids);

        m_pathLabel->setText(QString::fromUtf8(m_path.c_str()));
        m_saveButton->setEnabled(true);
        return true;
    }

    void ResourcePropertiesPanel::CloseHostEntity()
    {
        if (!m_hostEntity)
        {
            return;
        }

        AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler::BusDisconnect();
        // Detach the grid BEFORE tearing the entity down so rows stop referencing it.
        m_propertyEditor->SetOverrideEntityIds({});

        if (m_hostEntity->GetState() == AZ::Entity::State::Active)
        {
            m_hostEntity->Deactivate();
        }
        AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationBus::Events::RemoveEntity, m_hostEntity);
        delete m_hostEntity;
        m_hostEntity = nullptr;

        m_pathLabel->setText(QStringLiteral("No resource open"));
        m_saveButton->setEnabled(false);
    }

    void ResourcePropertiesPanel::OnSaveClicked()
    {
        if (!m_bridge || m_type.empty())
        {
            return;
        }
        // The backend serializes the cached resource (already edited via the property bus)
        // and reports on the Console; a user-initiated action gets direct failure feedback.
        if (!m_bridge->SaveResource(m_type, m_path))
        {
            QMessageBox::warning(
                this,
                QStringLiteral("Save Resource"),
                QStringLiteral("Could not save '%1' (see Console for details).")
                    .arg(QString::fromUtf8(m_path.c_str())));
        }
    }

    void ResourcePropertiesPanel::OnEntityComponentPropertyChanged(AZ::ComponentId /*componentId*/)
    {
        if (!m_hostEntity || !m_bridge)
        {
            return;
        }
        const EngineNodeComponent* comp = m_hostEntity->FindComponent<EngineNodeComponent>();
        if (!comp)
        {
            return;
        }
        m_bridge->WriteResourceProperties(m_type, m_path, comp->GetProperties());
    }
} // namespace CrossEngineEditor
