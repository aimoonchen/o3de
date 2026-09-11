/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Filament material backend (filament_migration.md §10).
//!
//! Type = precompiled .filamat template (cee_lit_opaque, built by the CEE matc pipeline and
//! resgen-embedded). Document = value JSON referencing the type (extension .fmat.json) - the
//! engine has no material instance format, the JSON sidecar is the only lossless container.
//! Preview = offscreen RenderTarget rendered inside the main frame + async readPixels, the
//! native three-state (Unchanged until the callback lands, then Updated) - zero stall.
//!
//! Compiled only when CEE_HAVE_FILAMENT is defined.

#include <BackendAPI/IMaterialSource.h>

#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/containers/vector.h>

#include <cstdint>

namespace filament
{
    class Engine;
    class Material;
    class MaterialInstance;
    class Renderer;
    class Texture;
    class VertexBuffer;
    class IndexBuffer;
}

namespace utils
{
    class Entity;
}

namespace CrossEngineEditor
{
    //! Preview scene owned by the source (built lazily on first SetPreviewMaterial).
    struct FilamentPreviewScene;

    class FilamentMaterialSource final : public IMaterialSource
    {
    public:
        //! The engine and surface-ready flag are owned by FilamentBackend and change with the
        //! viewport lifecycle, so they are read through pointers every call.
        FilamentMaterialSource(filament::Engine** engine, const bool* surfaceReady);
        ~FilamentMaterialSource() override;

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
        [[nodiscard]] PreviewResult AcquirePreviewImage(
            uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels) override;

        //! Render the preview view, called from FilamentBackend::EndOverlayFrame INSIDE
        //! beginFrame/endFrame when a preview frame was requested. readPixels is issued here
        //! so the callback lands a few frames later (the panel polls AcquirePreviewImage).
        void RenderPreviewFrame(filament::Renderer& renderer);

    private:
        struct MaterialEntry
        {
            filament::MaterialInstance* m_instance = nullptr; //!< Owned by the engine.
            AZStd::string m_sourcePath;             //!< JSON path ("" = created new).
            filament::Texture* m_texture = nullptr; //!< Loaded baseColorMap (owned; null = white fallback).
            MaterialPropertyValueMap m_values;      //!< Editor-visible values (filament has no
                                                    //!< parameter read-back; setParameter only).
        };

        //! Build the .filamat template material once (lazily; needs the engine).
        filament::Material* GetTemplateMaterial();
        //! Load the curated schema JSON once (TestAssets/MaterialTypes/filament/LitOpaque.json).
        void LoadSchema();
        void BuildPreviewScene();
        void DestroyPreviewScene();
        //! The 1x1 white "unset sampler" texture (builds the preview scene if needed).
        filament::Texture* GetPreviewWhite();
        //! Lat-long UV sphere with analytic TBN quaternions (S11: no engine primitive lib).
        utils::Entity CreatePreviewSphere(filament::Engine& engine);

        filament::Engine** m_engine = nullptr;
        const bool* m_surfaceReady = nullptr;

        MaterialTypeDesc m_schema;
        filament::Material* m_template = nullptr;
        filament::Texture* m_whiteTexture = nullptr; //!< Lazy 1x1 "unset sampler" binding.
        MaterialHandle m_nextHandle = 1;
        AZStd::unordered_map<MaterialHandle, AZStd::unique_ptr<MaterialEntry>> m_materials;

        AZStd::unique_ptr<FilamentPreviewScene> m_preview;
        MaterialHandle m_previewTarget = 0;
        bool m_previewRenderRequested = false;
        bool m_pixelsReady = false;
        AZStd::vector<AZ::u8> m_pixels;
    };
} // namespace CrossEngineEditor
