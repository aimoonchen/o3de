/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Do-nothing material backend for NullBackend and DiligentBackend.
//!
//! Provides a built-in "Demo PBR" schema with 8 L0 slots and an in-memory value map.
//! AcquirePreviewImage returns Unsupported (no engine = no preview).

#include <BackendAPI/IMaterialSource.h>

#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/smart_ptr/make_shared.h>

namespace CrossEngineEditor
{
    class NullMaterialSource final : public IMaterialSource
    {
    public:
        NullMaterialSource();
        ~NullMaterialSource() override = default;

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
        void SetPreviewMaterial([[maybe_unused]] MaterialHandle handle) override {}

    private:
        struct MaterialInstance
        {
            MaterialTypeDesc m_schema;
            MaterialPropertyValueMap m_values;
        };

        static MaterialTypeDesc BuildDemoPbrSchema();

        MaterialHandle m_nextHandle = 1;
        AZStd::unordered_map<MaterialHandle, AZStd::shared_ptr<MaterialInstance>> m_materials;
    };
} // namespace CrossEngineEditor
