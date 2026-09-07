/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Godot material backend using runtime reflection (zero hand-written schema).
//!
//! Walks get_property_list() via GDExtension to enumerate every editable property
//! on a live StandardMaterial3D instance, maps Godot hints to MaterialPropertyDesc,
//! and reads/writes values through get()/set() on the live object. Preview uses a
//! SubViewport with its own World3D, Camera3D, and DirectionalLight3D (matching
//! Godot's own material_editor_plugin.cpp:264-448).

#include <BackendAPI/IMaterialSource.h>

#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/containers/unordered_map.h>

// Forward declare GodotApi without pulling in the full header.
#include <Backends/GodotApi.h>

namespace CrossEngineEditor
{
    class GodotMaterialSource final : public IMaterialSource
    {
    public:
        explicit GodotMaterialSource(void* engineState);
        ~GodotMaterialSource() override = default;

        // IMaterialSource
        void EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out) override;
        [[nodiscard]] MaterialHandle CreateMaterial(AZStd::string_view typeId) override;
        [[nodiscard]] MaterialHandle LoadMaterial(AZStd::string_view absolutePath) override;
        void DestroyMaterial(MaterialHandle handle) override;
        [[nodiscard]] bool GetMaterialState(
            MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues) override;
        [[nodiscard]] SetResult SetPropertyValue(
            MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value) override;
        [[nodiscard]] bool SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath) override;
        void SetPreviewMaterial(MaterialHandle handle) override;
        bool SetPreviewModel(PreviewModel model) override;
        [[nodiscard]] PreviewResult AcquirePreviewImage(
            uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels) override;

    private:
        struct MaterialInstance
        {
            void* m_object = nullptr;  // Live Godot material object (opaque pointer)
            AZStd::string m_typeId;
            AZStd::string m_path;   // Absolute path if loaded; empty if created new.
            MaterialTypeDesc m_schema;
            MaterialPropertyValueMap m_values;
            bool m_dirty = false;   // True after any SetPropertyValue (triggers preview re-render).
        };

        struct PreviewState
        {
            void* m_viewport = nullptr;      // SubViewport for preview.
            void* m_world3d = nullptr;       // World3D owned by the SubViewport.
            void* m_camera = nullptr;        // Camera3D at (0,0,1.1) FOV 20.
            void* m_light1 = nullptr;        // DirectionalLight3D key light.
            void* m_light2 = nullptr;        // DirectionalLight3D fill light.
            void* m_sphere = nullptr;        // MeshInstance3D for sphere.
            void* m_box = nullptr;           // MeshInstance3D for box.
            void* m_quad = nullptr;          // MeshInstance3D for quad/plane.
            void* m_rotation = nullptr;      // Node3D parent for mesh instances.
            MaterialHandle m_boundHandle = 0; // Which material is currently bound.
            int m_model = 0;                 // PreviewModel (0=Sphere, 1=Cube, 2=Plane)
            bool m_sceneCreated = false;
            bool m_dirty = false;
            bool m_pendingReadback = false;
            uint32_t m_lastWidth = 0;
            uint32_t m_lastHeight = 0;
        };

        //! Build the schema for a Godot material class by walking get_property_list().
        MaterialTypeDesc BuildSchemaFromObject(void* obj) const;

        //! Read all property values from the live object into outValues.
        void ReadValuesFromObject(
            void* obj, const MaterialTypeDesc& schema, MaterialPropertyValueMap& outValues) const;

        //! Convert a Godot Variant to MaterialPropertyValue. Returns false if unsupported.
        bool VariantToPropertyValue(
            void* obj, const AZStd::string& propName,
            int64_t variantType, int64_t hint, const AZStd::string& hintString,
            MaterialPropertyValue& out) const;

        //! Convert a MaterialPropertyValue to a Godot Variant. Returns true on success.
        bool PropertyValueToVariant(
            const MaterialPropertyValue& value, MaterialPropertyType type, void* out) const;

        //! Parse a Godot PROPERTY_HINT_RANGE string: "min,max,step[,or_greater][,or_less][,suffix:xxx]"
        void ParseRangeHint(const AZStd::string& hintString,
            AZStd::optional<double>& min, AZStd::optional<double>& max,
            AZStd::optional<double>& step, AZStd::optional<double>& softMin,
            AZStd::optional<double>& softMax, AZStd::string& suffix) const;

        //! Lazy-create the preview SubViewport scene (camera, lights, meshes).
        void EnsurePreviewScene(PreviewState& preview);

        //! Teardown all preview objects.
        void DestroyPreviewScene(PreviewState& preview);

        //! Read back the preview viewport texture into outPixels. Returns Updated if ready,
        //! Unchanged if the frame is not yet rendered.
        PreviewResult ReadPreviewTexture(uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels);

        //! Helper: access the GodotApi via the stored pointer.
        CrossEngineEditor::GodotApi& Api() const;
        //! Helper: access the EngineState via the opaque pointer (casts to void* internally).
        void* State() const { return m_engineState; }

        void* m_engineState = nullptr; // Opaque pointer to GodotBackend::EngineState.
        CrossEngineEditor::GodotApi* m_api = nullptr; // Cached pointer into EngineState.m_api.

        MaterialHandle m_nextHandle = 1;
        AZStd::unordered_map<MaterialHandle, AZStd::shared_ptr<MaterialInstance>> m_materials;

        PreviewState m_preview;
    };
} // namespace CrossEngineEditor
