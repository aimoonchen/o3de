/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Real rbfx material backend (material_migration_final.md SS3).
//!
//! Implements IMaterialSource for the rbfx (Urho3D fork) engine. Uses rbfx's
//! ResourceCache for shared instances and SceneRendererToTexture for preview.
//!
//! Compiled only when the rbfx submodule is available (CEE_HAVE_RBFX).

#include <BackendAPI/IMaterialSource.h>

#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/containers/unordered_map.h>

namespace Urho3D
{
    class Context;
    class Material;
    class SceneRendererToTexture;
}

namespace CrossEngineEditor
{
    class RbfxMaterialSource final : public IMaterialSource
    {
    public:
        //! state is the shared engine context from RbfxBackend. The pointer may be null
        //! during early init (before OnSurfaceCreated), so all methods guard against it.
        explicit RbfxMaterialSource(Urho3D::Context* context);
        ~RbfxMaterialSource() override;

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
            Urho3D::Material* m_material = nullptr;  // borrowed from ResourceCache
            MaterialTypeDesc m_schema;
            bool m_dirty = false; // True after SetPropertyValue
        };

        //! Load the StandardPBR.json schema once at construction.
        void LoadSchema();

        //! Build MaterialPropertyValueMap from the live Material instance.
        void ReadValues(Urho3D::Material* material, MaterialPropertyValueMap& outValues) const;

        Urho3D::Context* m_context = nullptr;
        MaterialTypeDesc m_schema; // Loaded once from JSON
        MaterialHandle m_nextHandle = 1;
        AZStd::unordered_map<MaterialHandle, AZStd::shared_ptr<MaterialInstance>> m_materials;

        // Preview
        MaterialHandle m_previewTarget = 0;
        AZStd::shared_ptr<Urho3D::SceneRendererToTexture> m_rtt;
        bool m_previewDirty = false;
    };
} // namespace CrossEngineEditor
