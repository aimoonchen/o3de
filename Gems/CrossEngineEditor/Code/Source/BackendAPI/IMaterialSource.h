/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Material data source feeding the reused AtomToolsFramework document + inspector stack
//! (material_migration.md §2, §4). Fourth sub-contract on IEngineBackend.
//!
//! Two-layer model: the backend owns the LIVE material instance (the only source of truth
//! for values) and hands the editor a schema plus a value map. The editor turns that into
//! DynamicPropertyGroup / DynamicProperty and lets the stock ReflectedPropertyEditor render
//! it. Edited values come back one at a time; the backend applies them to its live material
//! so the preview updates, and serializes that same instance on save. Round-trip fidelity is
//! therefore structural: engine-specific fields the schema never exposes are never touched.
//!
//! Contract rules (Plan §A6 C4): every value type below is POD or AZStd, no Qt types, no
//! engine types. Engine-native values (Urho3D::Variant, godot::Variant,
//! filament::MaterialInstance) stay inside the backend. Pure virtual count is asserted at 8
//! by check_no_atom.ps1 (sentinels are not counted) - new capabilities go through a sentinel
//! default.

#include <AzCore/Math/Color.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/base.h>
#include <AzCore/RTTI/RTTI.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/variant.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/optional.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>

namespace CrossEngineEditor
{
    //! Backend-allocated opaque handle for one live material instance. 0 = invalid.
    //! Same shape as IEntityMirror::CreateObject returning an AZ::EntityId (IEntityMirror.h:80).
    using MaterialHandle = AZ::u64;

    //! Closed value set shared by every engine's material parameters. Texture and resource
    //! references travel as engine-relative path strings (rbfx resource name, Godot res://
    //! path, Filament image path) - an O3DE AssetId would be meaningless to those engines.
    using MaterialPropertyValue = AZStd::variant<
        AZStd::monostate,   //!< unset / invalid
        bool,
        AZ::s32,
        AZ::u32,
        float,
        AZ::Vector2,
        AZ::Vector3,
        AZ::Vector4,
        AZ::Color,
        AZStd::string>;

    using MaterialPropertyValueMap = AZStd::unordered_map<AZStd::string, MaterialPropertyValue>;

    //! Declared type of a property. Drives both the value alternative and the stock editor:
    //! Enum -> u32 index + combo box, Texture -> string + file browse control.
    enum class MaterialPropertyType : AZ::u8
    { Bool, Int, UInt, Float, Vec2, Vec3, Vec4, Color, Enum, Texture, String };

    //! Color space the stored value lives in, so the picker shows the artist the right swatch.
    //! Getting this wrong is the classic cross-engine color bug: rbfx stores MatDiffColor and
    //! MatEmissiveColor in GAMMA space (its own glTF importer calls LinearToGamma explicitly,
    //! GLTFImporter.cpp:2522) while Atom, Filament and Godot store linear.
    enum class MaterialColorSpace : AZ::u8 { Linear, Srgb };

    //! Outcome of one property edit. Tri-state so the editor knows whether the schema and the
    //! values have to be re-read (mirrors the tri-state IEntityMirror::RaycastNode convention).
    enum class SetResult : AZ::u8
    {
        Failed,               //!< unknown property or value rejected; editor reverts
        Applied,              //!< value applied, nothing else changed - refresh the group only
        AppliedSchemaChanged, //!< visibility set and/or OTHER property values may have changed;
                              //!< the editor calls GetMaterialState again and rebuilds the group
    };

    //! Preview mesh. Deliberately three shapes: every engine's own material inspector offers
    //! exactly sphere / box / plane (Godot material_editor_plugin.cpp:264-448).
    enum class PreviewModel : AZ::u8 { Sphere, Cube, Plane };

    //! Outcome of AcquirePreviewImage. The panel keeps its existing image on Unchanged, so a
    //! backend that has not rendered the requested frame yet costs one extra 60fps gate, never
    //! a readback. Unsupported is the sentinel default: backends without preview capability
    //! return it and the panel shows a placeholder.
    enum class PreviewResult : AZ::u8 { Unsupported, Unchanged, Updated };

    //! One editable property. Pure metadata plus a default - the CURRENT value travels
    //! separately in MaterialPropertyValueMap, so this struct is also exactly what a curated
    //! schema JSON file deserializes into (rbfx / Filament, §7).
    struct MaterialPropertyDesc final
    {
        AZ_RTTI(MaterialPropertyDesc, "{B4A2C8E1-3D5F-4A7B-9C0E-1F2D3E4A5B6C}");
        AZStd::string m_id;           //!< engine-native property name, flat and globally
                                      //!< unique inside the material. The editor only routes
                                      //!< it back verbatim; it never parses or prefixes it.
        AZStd::string m_displayName;  //!< falls back to m_id when empty
        AZStd::string m_description;  //!< tooltip. Treat as required (Blender precedent);
                                      //!< the schema validator warns on empty.

        MaterialPropertyType m_type = MaterialPropertyType::Float;
        MaterialPropertyValue m_defaultValue;   //!< drives the "modified" indicator

        //! Numeric range hints. Hard range clamps on write; soft range only bounds the slider
        //! (Godot "or_greater", Blender rna_def_property_ui_range). Applied to Int / UInt /
        //! Float / Vec2-4 (per component).
        AZStd::optional<double> m_min, m_max, m_softMin, m_softMax, m_step;

        //! Unit shown after the number ("m", "deg", "nits"). Free at the widget level - every
        //! stock numeric control already consumes AZ::Edit::Attributes::Suffix. Engine-native
        //! unit, never normalized across engines (Plan §A6 C4 normalization scope).
        AZStd::string m_suffix;

        AZStd::vector<AZStd::string> m_enumValues;     //!< Enum: display names, index = value
        AZStd::vector<AZStd::string> m_vectorLabels;   //!< {"X","Y","Z"} / {"U","V"}
        AZStd::vector<AZStd::string> m_fileExtensions; //!< Texture: picker filter, e.g. {"png","dds"}

        MaterialColorSpace m_colorSpace = MaterialColorSpace::Linear;  //!< Color only

        bool m_visible = true;    //!< backend already evaluated it against the current values
        bool m_readOnly = false;
    };

    //! A group of properties, one level of nesting (matches Godot GROUP / SUBGROUP).
    struct MaterialPropertyGroupDesc final
    {
        AZ_RTTI(MaterialPropertyGroupDesc, "{C5B3D9F2-4E6A-5B8C-AD1F-2A3E4F5B6C7D}");
        AZStd::string m_id;
        AZStd::string m_displayName;
        AZStd::string m_description;
        bool m_defaultCollapsed = false;   //!< advanced groups start folded (Blender precedent)

        //! Optional: id of a Bool property inside this group that gates the whole feature.
        //! When set, the editor hoists that checkbox into the group header and hides its
        //! ordinary row (§5.4 / S7). Empty means an ordinary group. Godot fills this for
        //! free from PROPERTY_HINT_GROUP_ENABLE - BaseMaterial3D uses it 15 times
        //! (material.cpp:3603-3734); rbfx and Filament leave it empty.
        AZStd::string m_toggleProperty;

        AZStd::vector<MaterialPropertyDesc> m_properties;
        AZStd::vector<MaterialPropertyGroupDesc> m_groups;
    };

    //! The schema of one open material: pure metadata, no values.
    struct MaterialTypeDesc final
    {
        AZ_RTTI(MaterialTypeDesc, "{D6C4E0A3-5F7B-6C9D-BE2A-3B4F5A6D7E8F}");
        AZStd::string m_id;           //!< matches a MaterialTypeInfo::m_id
        AZStd::string m_displayName;
        AZStd::string m_description;
        AZStd::vector<MaterialPropertyGroupDesc> m_groups;
    };

    //! A material template the engine can instantiate: an rbfx technique + defines combo, a
    //! Godot material class, a Filament .filamat package.
    struct MaterialTypeInfo final
    {
        AZ_RTTI(MaterialTypeInfo, "{E7D5F1B4-6A8C-7DAE-CF3B-4C5A6B7E8F9A}");
        AZStd::string m_id;           //!< engine-native identifier, round-tripped verbatim
        AZStd::string m_displayName;
        AZStd::string m_description;
        AZStd::vector<AZStd::string> m_fileExtensions;  //!< feeds DocumentTypeInfo's
                                                        //!< m_supportedExtensionsToCreate/Open/Save
    };

    class IMaterialSource
    {
    public:
        virtual ~IMaterialSource() = default;

        // ---- discovery -------------------------------------------------------------
        //! Templates offered in the New Material dialog; the union of m_fileExtensions also
        //! defines the file filters the document type registers with.
        virtual void EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out) = 0;

        // ---- live instance lifetime ------------------------------------------------
        //! One instance per open document, not just the previewed one: Godot's
        //! _validate_property only evaluates on a live instance (material.cpp:2547-2726).
        [[nodiscard]] virtual MaterialHandle CreateMaterial(AZStd::string_view typeId) = 0;
        //! Load into the ENGINE'S SHARED CACHE and return the cached instance (rbfx:
        //! ResourceCache::GetResource(type, name, false) - CEE precedent RbfxBackend.cpp:1341;
        //! Godot: ResourceLoader::load(..., CACHE_MODE_REUSE)) - NOT a private copy. Only the
        //! shared instance keeps "edits visible in the main viewport" true (6.1 positive
        //! reason 4). DestroyMaterial releases the editor's reference only; it never rolls
        //! back values already pushed to the live instance (Revert = v1.5 via re-Load).
        [[nodiscard]] virtual MaterialHandle LoadMaterial(AZStd::string_view absolutePath) = 0;
        virtual void DestroyMaterial(MaterialHandle handle) = 0;

        // ---- read / edit -----------------------------------------------------------
        //! Schema (with m_visible / m_readOnly already evaluated against the current values)
        //! plus the current value map. Called on open and again after AppliedSchemaChanged.
        [[nodiscard]] virtual bool GetMaterialState(
            MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues) = 0;

        //! Apply one edited value to the live material. propertyId is a MaterialPropertyDesc
        //! m_id. Must be visible in the preview within one frame; the backend may defer the
        //! actual GPU work to its own tick. Applied means the value took effect AS-IS: the
        //! backend must NOT silently clamp (hard ranges belong in the schema m_min/m_max),
        //! and any backend-side rewrite of OTHER values must return AppliedSchemaChanged.
        [[nodiscard]] virtual SetResult SetPropertyValue(
            MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value) = 0;

        // ---- persist ---------------------------------------------------------------
        //! Serialize the live instance to the engine's native format. absolutePath is absolute.
        [[nodiscard]] virtual bool SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath) = 0;

        // ---- preview ---------------------------------------------------------------
        //! Bind this material to the preview object. The backend creates its preview scene
        //! (mesh + light + environment) lazily on the first call. 0 clears it.
        virtual void SetPreviewMaterial(MaterialHandle handle) = 0;

        //! Sentinel default - "not supported" only (Plan §A6 C4). Default: fixed sphere.
        virtual bool SetPreviewModel(PreviewModel model) { (void)model; return false; }

        //! Sentinel default - "not supported" only. Copy the preview image as tightly
        //! packed RGBA8 (length = width*height*4, no row padding) into outPixels.
        //!
        //! Contract (see §6.4.4): AcquirePreviewImage is only called while the panel is
        //! dirty, i.e. after SetPropertyValue / SetPreviewMaterial / SetPreviewModel. The
        //! backend marks its preview render target dirty on those three calls and renders
        //! it on demand (rbfx: SURFACE_MANUALUPDATE + QueueUpdate, RenderSurface.cpp:79-82;
        //! Godot: VIEWPORT_UPDATE_ONCE, renderer_viewport.cpp:955-957). It returns Unchanged
        //! until that frame is ready; in steady state the panel makes no calls at all, so
        //! no readback and no GPU stall ever happens unattended.
        [[nodiscard]] virtual PreviewResult AcquirePreviewImage(
            uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels)
        {
            (void)width; (void)height; (void)outPixels;
            return PreviewResult::Unsupported;
        }
    };
} // namespace CrossEngineEditor
