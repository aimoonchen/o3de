/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Godot (libgodot) engine backend — godot_migration.md §2 (implemented).
//!
//! Render model: the editor owns the native window (HWND); Godot is created with
//! "--wid <hwnd-decimal>" (which sets the editor HWND as the Win32 *owner*) and then its real
//! main window is reparented (SetParent + WS_CHILD) into the viewport, so it presents straight
//! into the editor viewport. (Godot's --wid alone does NOT produce a WS_CHILD, so the reparent
//! step is required — see the EmbedGodotWindow note and PROGRESS.md.) Godot is embedded via the
//! libgodot C-API that ships in the checkout:
//!
//!   GDExtensionObjectPtr libgodot_create_godot_instance(int argc, char* argv[],
//!                                                        GDExtensionInitializationFunction init);
//!   void                 libgodot_destroy_godot_instance(GDExtensionObjectPtr instance);
//!
//! Per-frame we pump GodotInstance::iteration() (godot_migration.md §2).
//!
//! IMPORTANT (build vs. runtime): libgodot is built OUT OF TREE with scons into a shared
//! library (godot.<cfg>.dll). This backend loads that DLL at RUNTIME via LoadLibrary +
//! GetProcAddress, so it does NOT link Godot at build time and needs no generated Godot
//! headers to compile. The object-mirror / property / asset work (godot_migration.md §2) is driven through
//! the GDExtension interface obtained from the created instance (all implemented below).
//!
//! Coordinates cross the BackendAPI boundary in O3DE convention (Z-up / right-handed / metres)
//! and are converted at the edge by EngineTransformConverter (Godot = Y-up / right-handed /
//! -Z forward), i.e. O3DE [x,y,z] -> Godot [x, z, -y] (Plan §B6).
//!
//! Compiled only when the Godot checkout is available (CEE_HAVE_GODOT).

#include <BackendAPI/IAssetSource.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <BackendAPI/ISceneRenderer.h>
#include <Backends/GodotApi.h>
#include <Framework/EngineProperty.h>
#include <Framework/EngineTransformConverter.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace CrossEngineEditor
{
    //! Opaque handle to a created Godot instance (a GodotInstance GDExtension object).
    using GodotInstanceHandle = void*;

    //! Godot node handle stored in EngineNodeComponent's reflected node handle. Godot uses
    //! a 64-bit ObjectID for stable per-scene identity, mirrored here as AZ::u64.
    using GodotObjectId = AZ::u64;

    class GodotBackend final : public IEngineBackend
    {
    public:
        AZ_RTTI(GodotBackend, "{2E7B4C90-6D1A-4F35-9C22-8B0E3A7F5D42}", IEngineBackend);

        GodotBackend();
        ~GodotBackend() override;

        std::expected<void, BackendError> Initialize(const BackendInitParams& params) override;
        void Shutdown() override;
        void Tick(float deltaSeconds) override;

        ISceneRenderer& GetSceneRenderer() override { return m_sceneRenderer; }
        IEntityMirror& GetEntityMirror() override { return m_entityMirror; }
        IAssetSource& GetAssetSource() override { return m_assetSource; }

        //! Shared engine state, owned by the backend and referenced by the sub-services.
        //! Public so the GDExtension init callback (a free function in the .cpp, invoked by
        //! libgodot during create) can stash the resolved GodotApi into it.
        struct EngineState
        {
            void* m_libgodotModule = nullptr;      //!< HMODULE of the loaded libgodot DLL.
            GodotInstanceHandle m_instance = nullptr; //!< Created GodotInstance (or null).
            void* m_hostWindow = nullptr;          //!< Editor-owned HWND Godot re-parents to.
            uint32_t m_width = 1;
            uint32_t m_height = 1;
            AZStd::string m_projectPath;           //!< project.godot directory to edit.
            AZStd::string m_scenePath;             //!< Optional scene (res://...) to open on start.
            bool m_running = false;                //!< True once the GodotInstance is started.
            bool m_started = false;                //!< Guards one-shot start() after create.
            bool m_reparented = false;             //!< Guards one-shot Win32 reparent of Godot's window.
            bool m_vsyncDisabled = false;          //!< Guards one-shot vsync-off (avoids present blocking Tick).

            // ---- GDExtension access (resolved in the init callback, godot_migration.md §2) ----
            GodotApi m_api;                        //!< Thin GDExtension C-API facade.
            GDExtensionObjectPtr m_sceneTree = nullptr; //!< SceneTree MainLoop (Engine.get_main_loop).
            GDExtensionObjectPtr m_editorCamera = nullptr; //!< Camera3D we own to view the scene.
            GDExtensionObjectPtr m_overlayMesh = nullptr;  //!< MeshInstance3D holding the ArrayMesh.
            GDExtensionObjectPtr m_overlayArrayMesh = nullptr; //!< ArrayMesh: gizmo/grid geometry, bulk-submitted per frame.
            GDExtensionObjectPtr m_overlayMaterial = nullptr; //!< StandardMaterial3D: unshaded + albedo-from-vertex-color.
        };

    private:
        //! Load libgodot.dll and resolve libgodot_create/destroy. Returns false if not found;
        //! the backend then degrades gracefully (viewport shows nothing, editor logic runs).
        bool LoadLibGodot();
        void UnloadLibGodot();

        // ----- ISceneRenderer -------------------------------------------------------------
        class GodotSceneRenderer final : public ISceneRenderer
        {
        public:
            explicit GodotSceneRenderer(EngineState& state) : m_state(state) {}

            void OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height) override;
            void OnSurfaceResized(uint32_t width, uint32_t height) override;
            void OnSurfaceAboutToBeDestroyed() override;
            bool IsSurfaceReady() const override { return m_state.m_running; }

            void BeginOverlayFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) override;
            void SubmitLines(AZStd::span<const DebugVertex> vertices) override;
            void SubmitTriangles(AZStd::span<const DebugVertex> vertices) override;
            void SetDepthTest(bool enabled) override;
            void EndOverlayFrame() override;

        private:
            //! Lazily create the editor camera + overlay MeshInstance3D under the scene root.
            void EnsureOverlayNodes();

            //! Append one ArrayMesh surface of the given primitive from `vertices` in a single bulk
            //! add_surface_from_arrays call (Godot editor-gizmo path). Falls back to SurfaceTool if
            //! the GDExtension ABI lacks a required bulk entry point.
            void SubmitSurface(int64_t primitive, AZStd::span<const DebugVertex> vertices);
            void SubmitSurfaceViaSurfaceTool(int64_t primitive, AZStd::span<const DebugVertex> vertices);

            EngineState& m_state;
            bool m_depthTest = true;
            bool m_frameOpen = false;
            //! Reused per-frame scratch so the bulk submit does not reallocate: positions xyz,
            //! colors rgba, contiguous, memcpy'd straight into the packed arrays.
            AZStd::vector<float> m_posScratch;
            AZStd::vector<float> m_colorScratch;
        };

        // ----- IEntityMirror --------------------------------------------------------------
        class GodotEntityMirror final : public IEntityMirror
        {
        public:
            explicit GodotEntityMirror(EngineState& state) : m_state(state) {}

            void SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities) override;
            void FinishSync() override;
            void EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& out) override;
            void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) override;
            void OnEditorPropertyChanged(AZ::EntityId entityId) override;
            AZ::EntityId CreateObject(const ObjectSpec& spec) override;
            void DestroyObject(AZ::EntityId entityId) override;
            bool SaveScene(const AZStd::string& path) override;
            AZ::Aabb GetWorldBounds(AZ::EntityId entityId) const override;
            bool RaycastScene(
                const AZ::Vector3& rayOrigin,
                const AZ::Vector3& rayDirection,
                AZ::Vector3& outHitPoint,
                AZ::Vector3& outHitNormal) const override;
            bool CreatePrefabFromNodes(
                const AZStd::vector<AZ::EntityId>& entityIds,
                const AZStd::string& path) override;
            bool AssignMaterial(AZ::EntityId entityId, const AZStd::string& assetPath, int slot) override;
            bool AssignAnimation(AZ::EntityId entityId, const AZStd::string& assetPath) override;
            AZStd::vector<AZ::u8> SerializeNodes(const AZStd::vector<AZ::EntityId>& entityIds) override;
            bool PasteNodes(const AZStd::vector<AZ::u8>& data, AZ::EntityId parentId) override;

            // Migration 批次 2 精简 (rbfx_migration.md §3.1).
            bool SaveResource(
                const AZStd::string& type, const AZStd::string& path) override;
            bool ReadResourceProperties(
                const AZStd::string& type, const AZStd::string& path, PropertyBag& out) override;
            bool WriteResourceProperties(
                const AZStd::string& type, const AZStd::string& path, const PropertyBag& bag) override;

        private:
            //! Mirror one Godot Node into an AZ::Entity and recurse over its children.
            void MirrorNodeRecursive(
                GDExtensionObjectPtr node, AZ::EntityId parentId, AZStd::vector<AZ::Entity*>& outEntities);
            //! Read a node's editable properties into a typed PropertyBag (Plan §B4).
            void ReadProperties(GDExtensionObjectPtr node, PropertyBag& outBag) const;
            //! Resolve the Godot Node for a mirror entity via the live EngineNodeComponent's
            //! reflected ObjectID handle, or null (warns on miss). Single resolution path for every
            //! post-sync operation: the editor entity context can re-home the mirror entity (and
            //! change its AZ::EntityId), whereas the reflected handle survives that re-home.
            GDExtensionObjectPtr ResolveNode(AZ::EntityId entityId) const;

            EngineState& m_state;
            //! Mirror entity id -> Godot ObjectID, populated during SyncToEditor and consumed only by
            //! the synchronous FinishSync transform seeding in the same batch. NOT a resolution path
            //! for later edits - use ResolveNode for those.
            AZStd::unordered_map<AZ::EntityId, GodotObjectId> m_entityToNode;
            //! Child mirror entity -> parent mirror entity, applied in FinishSync via TransformBus.
            AZStd::unordered_map<AZ::EntityId, AZ::EntityId> m_pendingParent;
        };

        // ----- IAssetSource ---------------------------------------------------------------
        class GodotAssetSource final : public IAssetSource
        {
        public:
            explicit GodotAssetSource(EngineState& state) : m_state(state) {}

            void EnumerateRoot(AZStd::vector<AssetEntryInfo>& out) override;
            void EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out) override;

        private:
            EngineState& m_state;
        };

        EngineState m_state;
        GodotSceneRenderer m_sceneRenderer{ m_state };
        GodotEntityMirror m_entityMirror{ m_state };
        GodotAssetSource m_assetSource{ m_state };
    };
} // namespace CrossEngineEditor
