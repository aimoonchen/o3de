/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Generic resource property panel (rbfx_migration.md §3.3): the O3DE generic reflection
//! grid (EntityPropertyEditor) absorbs the engine's per-type resource editors. The panel
//! hosts one detached AZ::Entity carrying an EngineNodeComponent whose PropertyBag the
//! backend fills via ReadResourceProperties, writes back via WriteResourceProperties on
//! change, and persists via SaveResource (the panel's Save button). 100% reused O3DE
//! controls - only the data source (the backend contract) is engine-specific.
//!
//! The host entity is NOT in the editor entity context (ComponentApplicationBus only),
//! so it never appears in the Outliner / selection / undo system; the grid pins it via
//! SetOverrideEntityIds, the same mechanism the official Inspector uses for override
//! selection. v1 scope: Serializable attributes + Material shader parameters; other
//! resource types open empty (the backend warns why on the Console).

#include <AzCore/Component/EntityId.h>
#include <AzCore/std/string/string.h>

#include <AzToolsFramework/UI/PropertyEditor/PropertyEditorAPI.h>

#include <QWidget>

class QLabel;
class QPushButton;

namespace AZ
{
    class Entity;
}

namespace AzToolsFramework
{
    class EntityPropertyEditor;
}

namespace CrossEngineEditor
{
    class EntityMirrorBridge;

    class ResourcePropertiesPanel
        : public QWidget
        , private AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler
    {
    public:
        explicit ResourcePropertiesPanel(EntityMirrorBridge* bridge, QWidget* parent = nullptr);
        ~ResourcePropertiesPanel() override;

        //! Load a resource into the generic property grid. Returns false (grid cleared)
        //! when the backend reports no editable surface for the type; the backend warns
        //! on the Console trace panel with the reason.
        bool OpenResource(const AZStd::string& type, const AZStd::string& path);

    private:
        // PropertyEditorEntityChangeNotificationBus::MultiHandler...
        void OnEntityComponentPropertyChanged(AZ::ComponentId componentId) override;

        //! Save button: persists the edited cached resource to its source file.
        void OnSaveClicked();

        //! Build the detached host entity for the current m_type/m_path and attach the grid.
        bool CreateHostEntity();
        //! Tear down the current host entity + grid rows (exact reverse of CreateHostEntity).
        void CloseHostEntity();

        EntityMirrorBridge* m_bridge = nullptr;
        AzToolsFramework::EntityPropertyEditor* m_propertyEditor = nullptr;
        QLabel* m_pathLabel = nullptr;
        QPushButton* m_saveButton = nullptr; //!< Enabled only while a resource is open.
        AZ::Entity* m_hostEntity = nullptr; //!< Detached; owned (aznew/delete) by this panel.
        AZStd::string m_type;               //!< Engine resource type name (e.g. "Material").
        AZStd::string m_path;               //!< Resource path of the open resource.
    };
} // namespace CrossEngineEditor
