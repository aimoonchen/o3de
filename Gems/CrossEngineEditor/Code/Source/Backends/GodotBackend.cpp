/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Backends/GodotBackend.h>
#include <Framework/EngineNodeComponent.h>

#include <Profiling/CrossEngineProfiler.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Matrix3x3.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/std/string/conversions.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
AZ_POP_DISABLE_WARNING

#if defined(AZ_PLATFORM_WINDOWS)
#include <AzCore/PlatformIncl.h>  // Windows.h (LoadLibrary/GetProcAddress) via O3DE wrapper.
#endif

namespace CrossEngineEditor
{
    namespace
    {
        constexpr EngineSpace k_space = EngineSpace::Godot;

        // Godot Mesh.PrimitiveType (RenderingServer order): POINTS=0, LINES=1, TRIANGLES=3.
        constexpr int64_t k_primitiveLines = 1;
        constexpr int64_t k_primitiveTriangles = 3;

        //! libgodot C-API entry points, resolved at runtime from the libgodot DLL.
        using PFN_libgodot_create =
            GDExtensionObjectPtr (*)(int argc, char* argv[], GDExtensionInitializationFunction init_func);
        using PFN_libgodot_destroy = void (*)(GDExtensionObjectPtr instance);

        constexpr const char* kLibGodotNames[] = {
            "godot.windows.editor.dev.x86_64.dll",
            "godot.windows.editor.x86_64.dll",
            "libgodot.windows.editor.dev.x86_64.dll",
            "libgodot.windows.editor.x86_64.dll",
            "libgodot.dll",
        };

        //! The GDExtension init callback libgodot invokes during create. It hands us the
        //! GDExtensionInterfaceGetProcAddress we need to talk to the engine. We stash the resolved
        //! api into the EngineState being initialised. libgodot allows only one instance at a time,
        //! so a single pending pointer is safe (godot_migration.md §2).
        GodotBackend::EngineState* g_initTarget = nullptr;

#if defined(AZ_PLATFORM_WINDOWS)
        // Godot 4.8's "--wid <HWND>" does NOT truly embed into the host client area: it only sets
        // the host as the Win32 *owner* of a still-independent top-level, borderless window (no
        // WS_CHILD) - so the editor viewport would stay blank. The fix (below) is to reparent
        // Godot's real main window into our viewport HWND once it exists.
        //
        // Returns the Godot main-window HWND, or null if the DisplayServer/native handle is not
        // yet available (caller retries next frame).
        HWND ResolveGodotMainWindow(GodotBackend::EngineState& state)
        {
            GDExtensionObjectPtr ds = state.m_api.GetSingleton("DisplayServer");
            if (ds == nullptr)
            {
                return nullptr;
            }
            // window_get_native_handle(HandleType::WINDOW_HANDLE=1, MAIN_WINDOW_ID=0) -> (int64)HWND.
            GodotVariant args[2] = { state.m_api.MakeInt(1), state.m_api.MakeInt(0) };
            GodotVariant ret = state.m_api.Call(ds, "window_get_native_handle", args, 2);
            const int64_t handle = state.m_api.AsInt(ret);
            return reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
        }

        // Win32 reparent Godot's main window into the editor viewport (plan fix). Idempotent-safe:
        // caller gates via m_reparented. Godot renders here but must NOT take input: the editor (Qt)
        // owns the cursor, picking, camera and gizmos. The reliable way to make a *child* HWND both
        // render and stay input-transparent is WS_DISABLED - a disabled child receives no mouse or
        // keyboard messages, so Windows routes cursor / mouse / keyboard for that region to the
        // parent (the Qt viewport) while the window still paints normally. (WS_EX_TRANSPARENT is only
        // dependable for layered top-level windows, not an ordinary WS_CHILD, which is why the cursor
        // vanished and input died over the Godot child - it swallowed WM_SETCURSOR / mouse messages
        // instead of letting them fall through. WS_DISABLED fixes both.)
        bool EmbedGodotWindow(GodotBackend::EngineState& state)
        {
            HWND godotHwnd = ResolveGodotMainWindow(state);
            HWND hostHwnd = static_cast<HWND>(state.m_hostWindow);
            if (godotHwnd == nullptr || hostHwnd == nullptr)
            {
                return false;
            }

            LONG_PTR style = ::GetWindowLongPtrW(godotHwnd, GWL_STYLE);
            style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW);
            // WS_DISABLED: keep the child render-only so all input (cursor incl.) goes to the Qt parent.
            style |= WS_CHILD | WS_VISIBLE | WS_DISABLED;
            ::SetWindowLongPtrW(godotHwnd, GWL_STYLE, style);

            LONG_PTR exStyle = ::GetWindowLongPtrW(godotHwnd, GWL_EXSTYLE);
            // NOACTIVATE so it never steals focus; drop WS_EX_TRANSPARENT (ineffective for a child and
            // it was contributing to the swallowed-input symptom).
            exStyle &= ~WS_EX_TRANSPARENT;
            exStyle |= WS_EX_NOACTIVATE;
            ::SetWindowLongPtrW(godotHwnd, GWL_EXSTYLE, exStyle);

            ::SetParent(godotHwnd, hostHwnd);
            ::MoveWindow(godotHwnd, 0, 0, static_cast<int>(state.m_width), static_cast<int>(state.m_height), TRUE);
            ::ShowWindow(godotHwnd, SW_SHOWNOACTIVATE);
            return true;
        }
#endif

        void GodotInitializeLevel(void* /*userdata*/, GDExtensionInitializationLevel /*level*/) {}
        void GodotDeinitializeLevel(void* /*userdata*/, GDExtensionInitializationLevel /*level*/) {}

        GDExtensionBool GodotHostInit(
            GDExtensionInterfaceGetProcAddress getProc,
            GDExtensionClassLibraryPtr /*library*/,
            GDExtensionInitialization* init)
        {
            if (g_initTarget)
            {
                g_initTarget->m_api.Bind(getProc);
            }
            if (init)
            {
                init->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
                init->userdata = nullptr;
                init->initialize = &GodotInitializeLevel;
                init->deinitialize = &GodotDeinitializeLevel;
            }
            return 1;
        }

        // ---- Coordinate conversion (O3DE Z-up <-> Godot Y-up, Plan §B6) ---------------------
        // All coordinate math lives in EngineTransformConverter (Plan §B6, one place). These thin
        // wrappers just bind the Godot EngineSpace so the call sites read cleanly.
        AZ::Vector3 GodotPosFromO3de(const AZ::Vector3& v)
        {
            return EngineTransformConverter::PositionToEngine(k_space, v);
        }
        AZ::Vector3 GodotPosToO3de(const AZ::Vector3& v)
        {
            return EngineTransformConverter::PositionFromEngine(k_space, v);
        }

        //! Compose a Godot Transform3D raw (12 floats) from an O3DE world transform.
        void O3deTransformToGodotRaw(const AZ::Transform& tm, float out12[12])
        {
            EngineTransformConverter::TransformToEngineRaw12(k_space, tm, out12);
        }

        //! Decompose a Godot Transform3D raw (12 floats) into an O3DE world transform.
        AZ::Transform GodotRawToO3deTransform(const float in12[12])
        {
            return EngineTransformConverter::TransformFromEngineRaw12(k_space, in12);
        }

        //! Godot project asset extensions surfaced in the browser (Plan §B9).
        bool IsGodotAsset(const QString& suffix)
        {
            static const QStringList kExts = {
                "tscn", "scn", "res", "tres", "gd", "gdshader", "shader",
                "png", "jpg", "jpeg", "webp", "svg", "ogg", "wav", "gltf", "glb", "obj",
                "material", "mesh", "ttf", "otf", "godot",
            };
            return kExts.contains(suffix.toLower());
        }

        void EnumerateGodotDirectory(const AZStd::string& dirPath, AZStd::vector<AssetEntryInfo>& out)
        {
            if (dirPath.empty())
            {
                return;
            }
            QDir dir(QString::fromUtf8(dirPath.c_str()));
            if (!dir.exists())
            {
                return;
            }
            const QFileInfoList entries =
                dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
            for (const QFileInfo& info : entries)
            {
                if (info.isDir() && (info.fileName() == QStringLiteral(".godot") || info.fileName().startsWith('.')))
                {
                    continue;
                }
                if (!info.isDir() && !IsGodotAsset(info.suffix()))
                {
                    continue;
                }
                AssetEntryInfo entry;
                entry.m_path = info.absoluteFilePath().toUtf8().constData();
                entry.m_displayName = info.fileName().toUtf8().constData();
                entry.m_isFolder = info.isDir();
                out.push_back(AZStd::move(entry));
            }
        }

        //! Split a Godot PROPERTY_HINT_ENUM hint_string ("Val1,Val2,Val3") into names. Godot may
        //! also encode explicit values as "Name:2"; we keep only the display name (the ComboBox
        //! index maps to the stored int, matching the common contiguous-enum case).
        void SplitEnumNames(const AZStd::string& hintStr, AZStd::vector<AZStd::string>& out)
        {
            size_t start = 0;
            while (start <= hintStr.size())
            {
                const size_t comma = hintStr.find(',', start);
                const size_t end = (comma == AZStd::string::npos) ? hintStr.size() : comma;
                AZStd::string token = hintStr.substr(start, end - start);
                const size_t colon = token.find(':');
                if (colon != AZStd::string::npos)
                {
                    token = token.substr(0, colon);
                }
                if (!token.empty())
                {
                    out.push_back(token);
                }
                if (comma == AZStd::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
        }
    } // namespace

    // =====================================================================================
    // GodotBackend
    // =====================================================================================
    GodotBackend::GodotBackend() = default;

    GodotBackend::~GodotBackend()
    {
        Shutdown();
    }

    bool GodotBackend::LoadLibGodot()
    {
#if defined(AZ_PLATFORM_WINDOWS)
        for (const char* name : kLibGodotNames)
        {
            HMODULE module = ::LoadLibraryA(name);
            if (module != nullptr)
            {
                m_state.m_libgodotModule = module;
                return true;
            }
        }
        AZ_Warning("CrossEngineEditor", false,
            "Godot backend: no libgodot DLL found (looked for godot.windows.editor.*.dll / "
            "libgodot.dll on PATH / next to the executable). Build it with: "
            "scons platform=windows target=editor library_type=shared_library. "
            "Running with no Godot instance (editor logic still works).");
#endif
        return false;
    }

    void GodotBackend::UnloadLibGodot()
    {
#if defined(AZ_PLATFORM_WINDOWS)
        if (m_state.m_instance != nullptr && m_state.m_libgodotModule != nullptr)
        {
            // Release interned StringNames while the engine is still live (they hold engine-side
            // ref-counted heap; freeing after libgodot_destroy would be use-after-teardown).
            m_state.m_api.ReleaseCaches();
            auto destroy = reinterpret_cast<PFN_libgodot_destroy>(
                ::GetProcAddress(static_cast<HMODULE>(m_state.m_libgodotModule), "libgodot_destroy_godot_instance"));
            if (destroy != nullptr)
            {
                destroy(m_state.m_instance);
            }
            m_state.m_instance = nullptr;
        }
        if (m_state.m_libgodotModule != nullptr)
        {
            ::FreeLibrary(static_cast<HMODULE>(m_state.m_libgodotModule));
            m_state.m_libgodotModule = nullptr;
        }
#endif
    }

    std::expected<void, BackendError> GodotBackend::Initialize(const BackendInitParams& params)
    {
        m_state.m_projectPath = params.m_projectPath;
        m_state.m_scenePath = params.m_scenePath;

        // Load is only pre-flighted here; the instance is created on OnSurfaceCreated because
        // Godot must be created with "--wid <HWND>" so DisplayServerWindows parents to it.
        LoadLibGodot(); // Non-fatal: editor shell runs on empty contracts if absent.
        return {};
    }

    void GodotBackend::Shutdown()
    {
        m_state.m_running = false;
        UnloadLibGodot();
    }

    void GodotBackend::Tick([[maybe_unused]] float deltaSeconds)
    {
        CEE_PROFILE_FUNCTION();
#if defined(AZ_PLATFORM_WINDOWS)
        if (m_state.m_instance == nullptr || !m_state.m_api.IsValid())
        {
            return;
        }

        // One-shot start() after creation: brings the main loop up and loads the project's main
        // scene (or the requested --scene). Done here (not in OnSurfaceCreated) so the first
        // iteration follows immediately (Plan §B3 single-loop).
        if (!m_state.m_started)
        {
#if defined(CEE_GODOT_HEADER_VERSION_MAJOR) && defined(CEE_GODOT_HEADER_VERSION_MINOR)
            // Header/runtime handshake: the GDExtension headers come from the CEE_GODOT_ROOT
            // checkout while the engine comes from its bin/ libgodot DLL, and the two can drift
            // apart (checkout updated without rebuilding the DLL -> silently mixed ABI). Compare
            // major.minor once at startup; the constants are injected by the CMake facade from
            // the checkout's git tag.
            if (GDExtensionObjectPtr engine = m_state.m_api.GetSingleton("Engine"))
            {
                const GodotVariant versionInfo = m_state.m_api.Call(engine, "get_version_info");
                if (m_state.m_api.TypeOf(versionInfo) == GDEXTENSION_VARIANT_TYPE_DICTIONARY)
                {
                    const AZStd::string runtimeVersion = m_state.m_api.VariantStringToAz(
                        m_state.m_api.DictGet(versionInfo, "string"));
                    const size_t dot = runtimeVersion.find('.');
                    if (dot != AZStd::string::npos)
                    {
                        const int runtimeMajor = atoi(runtimeVersion.substr(0, dot).c_str());
                        const int runtimeMinor = atoi(runtimeVersion.substr(dot + 1).c_str());
                        if (runtimeMajor != CEE_GODOT_HEADER_VERSION_MAJOR ||
                            runtimeMinor != CEE_GODOT_HEADER_VERSION_MINOR)
                        {
                            AZ_Warning("CrossEngineEditor", false,
                                "Godot backend: runtime libgodot is '%s' but the headers were built from "
                                "Godot %d.%d. Rebuild the DLL (scons platform=windows target=editor "
                                "library_type=shared_library) or point -DCEE_GODOT_ROOT at the matching checkout.",
                                runtimeVersion.c_str(),
                                CEE_GODOT_HEADER_VERSION_MAJOR, CEE_GODOT_HEADER_VERSION_MINOR);
                        }
                    }
                }
            }
#endif
            m_state.m_api.Call(m_state.m_instance, "start");
            m_state.m_started = true;

            // Resolve the SceneTree main loop via the Engine singleton, then optionally switch to
            // the requested scene. change_scene_to_file is DEFERRED (the swap happens next frame),
            // so the current scene is not yet available here - m_running is flipped below once
            // get_current_scene resolves.
            if (GDExtensionObjectPtr engine = m_state.m_api.GetSingleton("Engine"))
            {
                GodotVariant mainLoop = m_state.m_api.Call(engine, "get_main_loop");
                m_state.m_sceneTree = m_state.m_api.AsObject(mainLoop);
            }
            if (m_state.m_sceneTree && !m_state.m_scenePath.empty())
            {
                GodotVariant path = m_state.m_api.MakeString(m_state.m_scenePath.c_str());
                m_state.m_api.Call(m_state.m_sceneTree, "change_scene_to_file", &path, 1);
            }
        }

        // Pump exactly one Godot frame; iteration() returns true when a quit is requested. This
        // also applies the deferred scene change, after which get_current_scene resolves and the
        // surface is reported ready so the shell mirrors the scene into the Outliner (one-shot).
        // NOTE: iteration() runs Godot's whole frame incl. present, the prime suspect for the
        // camera-roam / post-selection stutter (Progress.md fix-11/fix-13) - keep it in its own zone.
        GodotVariant quit;
        {
            CEE_PROFILE_SCOPE("Godot.iteration(present)");
            quit = m_state.m_api.Call(m_state.m_instance, "iteration");
        }
        if (m_state.m_api.TypeOf(quit) == GDEXTENSION_VARIANT_TYPE_BOOL && m_state.m_api.AsBool(quit))
        {
            m_state.m_running = false;
            return;
        }

        // Mark the surface ready for mirroring only once a scene root actually exists. When no
        // --scene / main scene is set, fall back to the raw root so the editor still renders.
        if (!m_state.m_running && m_state.m_sceneTree)
        {
            GodotVariant sceneV = m_state.m_api.Call(m_state.m_sceneTree, "get_current_scene");
            const bool haveScene = (m_state.m_api.AsObject(sceneV) != nullptr);
            m_state.m_running = haveScene || m_state.m_scenePath.empty();
        }

        // One-shot Win32 reparent of Godot's main window into the editor viewport. Must run AFTER
        // start()+iteration() (the window only exists then); retried each frame until it resolves.
        if (m_state.m_started && !m_state.m_reparented)
        {
            m_state.m_reparented = EmbedGodotWindow(m_state);
        }

        // One-shot: disable Godot's VSync. We drive Godot synchronously - one iteration() per editor
        // Tick on this thread - and iteration() presents the frame. With Godot's default
        // vsync_mode = ENABLED that present blocks on VBLANK (~16 ms), stalling the editor's own
        // event loop and making camera movement stutter. Turning VSync off lets present return
        // immediately (Vulkan IMMEDIATE / D3D12 tearing), so the host owns pacing. This is the
        // supported knob (DisplayServer.window_set_vsync_mode, VSYNC_DISABLED == 0); done once after
        // start() when the DisplayServer/main window exists (same readiness the reparent relies on).
        if (m_state.m_started && !m_state.m_vsyncDisabled)
        {
            if (GDExtensionObjectPtr ds = m_state.m_api.GetSingleton("DisplayServer"))
            {
                GodotVariant args[2] = { m_state.m_api.MakeInt(0 /*VSYNC_DISABLED*/), m_state.m_api.MakeInt(0 /*MAIN_WINDOW_ID*/) };
                m_state.m_api.Call(ds, "window_set_vsync_mode", args, 2);
                m_state.m_vsyncDisabled = true;
            }
        }
#endif
    }

    // =====================================================================================
    // GodotSceneRenderer  (godot_migration.md §2: viewport embedding + ArrayMesh overlay)
    // =====================================================================================
    void GodotBackend::GodotSceneRenderer::OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height)
    {
        m_state.m_hostWindow = nativeWindowHandle;
        m_state.m_width = width;
        m_state.m_height = height;

#if defined(AZ_PLATFORM_WINDOWS)
        if (m_state.m_libgodotModule == nullptr || m_state.m_instance != nullptr)
        {
            return;
        }

        auto create = reinterpret_cast<PFN_libgodot_create>(
            ::GetProcAddress(static_cast<HMODULE>(m_state.m_libgodotModule), "libgodot_create_godot_instance"));
        if (create == nullptr)
        {
            AZ_Warning("CrossEngineEditor", false,
                "Godot backend: libgodot_create_godot_instance not exported by the loaded DLL.");
            return;
        }

        // "--wid <hwnd>" sets the editor HWND as Godot's Win32 owner at creation (the real embed
        // is EmbedGodotWindow's SetParent reparent, Plan §B2); "--path <project>" opens the
        // project. We drive editor camera/overlay ourselves, so we do NOT pass --editor (which
        // would bring up Godot's own editor UI).
        const auto hwndValue = reinterpret_cast<uintptr_t>(nativeWindowHandle);
        AZStd::string widArg = AZStd::string::format("%llu", static_cast<unsigned long long>(hwndValue));

        AZStd::vector<AZStd::string> argStrings;
        argStrings.push_back("godot");
        argStrings.push_back("--wid");
        argStrings.push_back(widArg);
        if (!m_state.m_projectPath.empty())
        {
            argStrings.push_back("--path");
            argStrings.push_back(m_state.m_projectPath);
        }

        AZStd::vector<char*> argv;
        argv.reserve(argStrings.size());
        for (auto& s : argStrings)
        {
            argv.push_back(s.data());
        }

        // Publish the state so our GDExtension init callback can stash the proc-address table.
        g_initTarget = &m_state;
        m_state.m_instance = create(static_cast<int>(argv.size()), argv.data(), &GodotHostInit);
        g_initTarget = nullptr;

        AZ_Warning("CrossEngineEditor", m_state.m_instance != nullptr,
            "Godot backend: libgodot_create_godot_instance returned null; viewport will be empty.");
        AZ_Warning("CrossEngineEditor", m_state.m_api.IsValid(),
            "Godot backend: GDExtension interface not resolved; mirror/overlay disabled.");
        // m_running flips true after start() in the first Tick.
#endif
    }

    void GodotBackend::GodotSceneRenderer::OnSurfaceResized(uint32_t width, uint32_t height)
    {
        m_state.m_width = width;
        m_state.m_height = height;
#if defined(AZ_PLATFORM_WINDOWS)
        // Keep the embedded Godot window flush with the viewport client area.
        if (m_state.m_reparented)
        {
            if (HWND godotHwnd = ResolveGodotMainWindow(m_state))
            {
                ::MoveWindow(godotHwnd, 0, 0, static_cast<int>(width), static_cast<int>(height), TRUE);
            }
        }
#endif
    }

    void GodotBackend::GodotSceneRenderer::OnSurfaceAboutToBeDestroyed()
    {
        m_state.m_running = false;
        // The GodotInstance is torn down in GodotBackend::Shutdown via libgodot_destroy.
    }

    void GodotBackend::GodotSceneRenderer::EnsureOverlayNodes()
    {
        GodotApi& api = m_state.m_api;
        if (!api.IsValid() || !m_state.m_sceneTree)
        {
            return;
        }

        // The scene tree root (a Window/Viewport) is the mount point for our editor-owned nodes.
        if (!m_state.m_editorCamera)
        {
            GodotVariant rootV = api.Call(m_state.m_sceneTree, "get_root");
            GDExtensionObjectPtr root = api.AsObject(rootV);
            if (!root)
            {
                return;
            }

            // Editor camera: a Camera3D we drive from the editor view and make current so Godot
            // renders the scene through it (godot_migration.md §2).
            m_state.m_editorCamera = api.ConstructObject("Camera3D");
            if (m_state.m_editorCamera)
            {
                GodotVariant cam = api.MakeObject(m_state.m_editorCamera);
                api.Call(root, "add_child", &cam, 1);
                api.Call(m_state.m_editorCamera, "make_current");
            }

            // Overlay: a MeshInstance3D holding an ArrayMesh, rebuilt every frame with the
            // gizmo/grid primitives. ArrayMesh + add_surface_from_arrays is Godot's own editor
            // gizmo path (EditorNode3DGizmo::add_vertices) and lets us submit each surface in a
            // single GDExtension call instead of the per-vertex ImmediateMesh churn that
            // Progress.md fix-13 measured as the pick-then-orbit stall (plan-aligned fix 1).
            m_state.m_overlayMesh = api.ConstructObject("MeshInstance3D");
            m_state.m_overlayArrayMesh = api.ConstructObject("ArrayMesh");
            if (m_state.m_overlayMesh && m_state.m_overlayArrayMesh)
            {
                GodotVariant immV = api.MakeObject(m_state.m_overlayArrayMesh);
                api.Call(m_state.m_overlayMesh, "set_mesh", &immV, 1);
                GodotVariant meshV = api.MakeObject(m_state.m_overlayMesh);
                api.Call(root, "add_child", &meshV, 1);
            }

            // Overlay material: an unshaded StandardMaterial3D that uses per-vertex colours as
            // albedo. add_surface_from_arrays surfaces default to a shaded material that IGNORES the
            // vertex colour channel (gizmos would render flat grey); this matches Godot's own gizmo
            // material setup (node_3d_editor_gizmos.cpp:928-936: UNSHADED + ALBEDO_FROM_VERTEX_COLOR
            // + SRGB_VERTEX_COLOR).
            //
            // CRITICAL (RefCounted lifetime): a freshly classdb_construct'd Resource has refcount 1
            // but is NOT owned by anything on our side (a bare Variant(OBJECT) does not hold a Ref).
            // It gets collected almost immediately - which is why the earlier attempt read back
            // class=<null> and all flags 0. Fix: hand it to the MeshInstance3D via
            // set_material_override FIRST (the node keeps a Ref<Material>, keeping it alive), THEN
            // configure it. material_override also applies to every surface, so no per-surface bind.
            m_state.m_overlayMaterial = api.ConstructObject("StandardMaterial3D");
            if (m_state.m_overlayMaterial && m_state.m_overlayMesh)
            {
                GDExtensionObjectPtr mat = m_state.m_overlayMaterial;

                // 1) Keep it alive: MeshInstance3D takes a Ref via material_override.
                GodotVariant matV = api.MakeObject(mat);
                api.Call(m_state.m_overlayMesh, "set_material_override", &matV, 1);

                // 2) Configure (now safely owned). set_shading_mode(SHADING_MODE_UNSHADED=0).
                GodotVariant shadeArg = api.MakeInt(0);
                api.Call(mat, "set_shading_mode", &shadeArg, 1);
                // set_flag(FLAG_ALBEDO_FROM_VERTEX_COLOR=1, true).
                {
                    GodotVariant flagV = api.MakeInt(1);
                    GodotVariant enV = api.MakeBool(true);
                    const GodotVariant a[2] = { AZStd::move(flagV), AZStd::move(enV) };
                    api.Call(mat, "set_flag", a, 2);
                }
                // set_flag(FLAG_SRGB_VERTEX_COLOR=2, true).
                {
                    GodotVariant flagV = api.MakeInt(2);
                    GodotVariant enV = api.MakeBool(true);
                    const GodotVariant a[2] = { AZStd::move(flagV), AZStd::move(enV) };
                    api.Call(mat, "set_flag", a, 2);
                }
            }

            // Depth-off material: same as above but with FLAG_DISABLE_DEPTH_TEST so overlay
            // geometry (gizmo handles, selection outlines) draws over scene geometry.
            // FLAG_DISABLE_DEPTH_TEST = 0 (material.h Flags enum first member).
            m_state.m_overlayMaterialDepthOff = api.ConstructObject("StandardMaterial3D");
            if (m_state.m_overlayMaterialDepthOff)
            {
                GDExtensionObjectPtr matOff = m_state.m_overlayMaterialDepthOff;
                GodotVariant shadeArg = api.MakeInt(0); // SHADING_MODE_UNSHADED
                api.Call(matOff, "set_shading_mode", &shadeArg, 1);
                {
                    GodotVariant flagV = api.MakeInt(0); // FLAG_DISABLE_DEPTH_TEST
                    GodotVariant enV = api.MakeBool(true);
                    const GodotVariant a[2] = { AZStd::move(flagV), AZStd::move(enV) };
                    api.Call(matOff, "set_flag", a, 2);
                }
                {
                    GodotVariant flagV = api.MakeInt(1); // FLAG_ALBEDO_FROM_VERTEX_COLOR
                    GodotVariant enV = api.MakeBool(true);
                    const GodotVariant a[2] = { AZStd::move(flagV), AZStd::move(enV) };
                    api.Call(matOff, "set_flag", a, 2);
                }
                {
                    GodotVariant flagV = api.MakeInt(2); // FLAG_SRGB_VERTEX_COLOR
                    GodotVariant enV = api.MakeBool(true);
                    const GodotVariant a[2] = { AZStd::move(flagV), AZStd::move(enV) };
                    api.Call(matOff, "set_flag", a, 2);
                }
            }
        }
    }

    void GodotBackend::GodotSceneRenderer::BeginOverlayFrame(
        const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip)
    {
        m_frameOpen = false;
        GodotApi& api = m_state.m_api;
        if (!m_state.m_running || !api.IsValid())
        {
            return;
        }

        EnsureOverlayNodes();

        // Drive the editor camera from the editor's view matrix (godot_migration.md §2): camera world = inverse
        // of world->view, converted O3DE -> Godot, applied as the Camera3D global transform.
        if (m_state.m_editorCamera)
        {
            const AZ::Matrix4x4 viewToWorld = worldToView.GetInverseTransform();
            const AZ::Vector3 camPos = viewToWorld.GetTranslation();
            const AZ::Quaternion camRot = AZ::Quaternion::CreateFromMatrix4x4(viewToWorld);
            // O3DE cameras look down local +Y; Godot cameras look down local -Z. Rx(+90) aligns the
            // O3DE forward (+Y) onto Godot's view direction (FORWARD probe confirms it aims at the
            // scene); Ry(180) - a roll about O3DE's local forward (+Y) - then flips the image
            // upright without disturbing the forward. (Verified against the PROGRESS.md baseline.)
            const AZ::Quaternion camForwardFix =
                AZ::Quaternion::CreateRotationX(AZ::Constants::HalfPi) *
                AZ::Quaternion::CreateRotationY(AZ::Constants::Pi);
            const AZ::Transform camWorld =
                AZ::Transform::CreateFromQuaternionAndTranslation(camRot, camPos) *
                AZ::Transform::CreateFromQuaternion(camForwardFix);
            float raw[12];
            O3deTransformToGodotRaw(camWorld, raw);
            GodotVariant tm = api.MakeTransform3D(raw);
            api.Call(m_state.m_editorCamera, "set_global_transform", &tm, 1);
            // Re-assert current each frame: the scene swap / surface rebuild can steal it.
            api.Call(m_state.m_editorCamera, "make_current");

            const float clip11 = viewToClip.GetElement(1, 1);
            if (clip11 > 0.0001f)
            {
                const float fovYRadians = 2.0f * std::atan(1.0f / clip11);
                GodotVariant fov = api.MakeFloat(AZ::RadToDeg(fovYRadians));
                api.Call(m_state.m_editorCamera, "set_fov", &fov, 1);
            }
        }

        // Start a fresh overlay: drop last frame's surfaces. Each Submit* adds one surface via the
        // bulk ArrayMesh path below.
        if (m_state.m_overlayArrayMesh)
        {
            api.Call(m_state.m_overlayArrayMesh, "clear_surfaces");
            m_frameOpen = true;
        }
    }

    void GodotBackend::GodotSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        CEE_PROFILE_FUNCTION();
        SubmitSurface(k_primitiveLines, vertices);
    }

    void GodotBackend::GodotSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        CEE_PROFILE_FUNCTION();
        SubmitSurface(k_primitiveTriangles, vertices);
    }

    void GodotBackend::GodotSceneRenderer::SubmitSurface(
        int64_t primitive, AZStd::span<const DebugVertex> vertices)
    {
        GodotApi& api = m_state.m_api;

        if (!m_frameOpen || !m_state.m_overlayArrayMesh || vertices.empty())
        {
            return;
        }
        GDExtensionObjectPtr mesh = m_state.m_overlayArrayMesh;
        const int64_t count = static_cast<int64_t>(vertices.size());

        if (api.BulkOverlayReady())
        {
            // Bulk path (Godot editor-gizmo pattern): pack all positions/colors into contiguous
            // float buffers, hand them to PackedVector3Array/PackedColorArray in one shot each, then
            // one add_surface_from_arrays call. O(1) GDExtension calls per surface instead of O(N).
            m_posScratch.resize(static_cast<size_t>(count) * 3);
            m_colorScratch.resize(static_cast<size_t>(count) * 4);
            for (int64_t i = 0; i < count; ++i)
            {
                const AZ::Vector3 p = GodotPosFromO3de(vertices[i].m_position);
                m_posScratch[i * 3 + 0] = p.GetX();
                m_posScratch[i * 3 + 1] = p.GetY();
                m_posScratch[i * 3 + 2] = p.GetZ();
                const AZ::Color& c = vertices[i].m_color;
                m_colorScratch[i * 4 + 0] = c.GetR();
                m_colorScratch[i * 4 + 1] = c.GetG();
                m_colorScratch[i * 4 + 2] = c.GetB();
                m_colorScratch[i * 4 + 3] = c.GetA();
            }
            GodotVariant arrays = api.BuildSurfaceArrays(m_posScratch.data(), m_colorScratch.data(), count);
            GodotVariant primV = api.MakeInt(primitive);
            const GodotVariant args[2] = { AZStd::move(primV), AZStd::move(arrays) };
            api.Call(mesh, "add_surface_from_arrays", args, 2);
            // Vertex colour comes from the MeshInstance3D material_override set up in
            // EnsureOverlayNodes (applies to every surface) - no per-surface material bind needed.
            return;
        }

        // Fallback (older GDExtension ABI missing a bulk entry point): SurfaceTool builds an
        // ArrayMesh the same way, still one commit call rather than per-vertex ImmediateMesh churn.
        SubmitSurfaceViaSurfaceTool(primitive, vertices);
    }

    void GodotBackend::GodotSceneRenderer::SubmitSurfaceViaSurfaceTool(
        int64_t primitive, AZStd::span<const DebugVertex> vertices)
    {
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr mesh = m_state.m_overlayArrayMesh;

        // SurfaceTool.begin(primitive); per-vertex set_color/add_vertex; commit(existing_mesh).
        // This still crosses the DLL per vertex, but is the ABI-portable fallback and only runs if
        // the bulk packed-array path is unavailable. Kept minimal.
        // HAZARD (see the material RefCounted note in EnsureOverlayNodes): ConstructObject makes a
        // bare RefCounted with no Ref holder, so `st` could be freed early (UAF) or never freed
        // (leak). Only reached when the bulk ABI entry is missing; left as-is pending a dedicated
        // Ref-lifetime fix if this path ever activates.
        GDExtensionObjectPtr st = api.ConstructObject("SurfaceTool");
        if (!st)
        {
            return;
        }
        GodotVariant primV = api.MakeInt(primitive);
        api.Call(st, "begin", &primV, 1);
        for (const DebugVertex& v : vertices)
        {
            GodotVariant color = api.MakeColor(v.m_color);
            api.Call(st, "set_color", &color, 1);
            GodotVariant pos = api.MakeVector3(GodotPosFromO3de(v.m_position));
            api.Call(st, "add_vertex", &pos, 1);
        }
        GodotVariant meshV = api.MakeObject(mesh);
        api.Call(st, "commit", &meshV, 1); // append a surface onto our ArrayMesh
        // Vertex colour comes from the MeshInstance3D material_override (see EnsureOverlayNodes).
    }

    void GodotBackend::GodotSceneRenderer::SetDepthTest(bool enabled)
    {
        m_depthTest = enabled;
        // Switch the overlay mesh's material between depth-on and depth-off variants so
        // overlay geometry (gizmo handles, selection outlines) draws over scene geometry
        // when depth test is disabled. This matches rbfx's DebugRenderer depthTest parameter.
        GodotApi& api = m_state.m_api;
        if (!m_state.m_overlayMesh || !api.IsValid())
        {
            return;
        }
        GDExtensionObjectPtr mat = enabled ? m_state.m_overlayMaterial : m_state.m_overlayMaterialDepthOff;
        if (mat)
        {
            GodotVariant matV = api.MakeObject(mat);
            api.Call(m_state.m_overlayMesh, "set_material_override", &matV, 1);
        }
    }

    void GodotBackend::GodotSceneRenderer::EndOverlayFrame()
    {
        // Surfaces are closed per-batch in SubmitLines/Triangles; Godot presents in Tick().
        m_frameOpen = false;
    }

    // =====================================================================================
    // GodotEntityMirror  (godot_migration.md §2: mirror / properties / create-destroy)
    // =====================================================================================
    GDExtensionObjectPtr GodotBackend::GodotEntityMirror::ResolveNode(AZ::EntityId entityId) const
    {
        // Single resolution path: read the Godot ObjectID off the live EngineNodeComponent's
        // reflected handle. The handle survives the editor entity context re-homing the mirror
        // entity into its prefab (which can change the AZ::EntityId), so this never silently
        // no-ops the way an entity-id -> ObjectID map would.
        if (!m_state.m_api.IsValid())
        {
            return nullptr;
        }
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationBus::Events::FindEntity, entityId);
        auto* comp = entity ? entity->FindComponent<EngineNodeComponent>() : nullptr;
        if (!comp)
        {
            AZ_Warning("CrossEngineEditor", false,
                "godot ResolveNode: no EngineNodeComponent for entity %s.", entityId.ToString().c_str());
            return nullptr;
        }
        GDExtensionObjectPtr node = m_state.m_api.ObjectFromId(static_cast<GodotObjectId>(comp->GetNodeHandle()));
        AZ_Warning("CrossEngineEditor", node != nullptr,
            "godot ResolveNode: ObjectID %llu no longer valid for entity %s.",
            static_cast<AZ::u64>(comp->GetNodeHandle()), entityId.ToString().c_str());
        return node;
    }

    AZ::Aabb GodotBackend::GodotEntityMirror::GetWorldBounds(AZ::EntityId entityId) const
    {
        CEE_PROFILE_FUNCTION();
        const GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr root = ResolveNode(entityId);
        if (!root)
        {
            return AZ::Aabb::CreateNull();
        }

        // Use the local AABB of THIS NODE ONLY (not the sub-tree) - see the per-node rationale in
        // IEntityMirror::GetWorldBounds. Matches Godot's own per-instance gizmo AABB.
        AZ::Aabb godotWorld = AZ::Aabb::CreateNull(); // accumulated in Godot space; converted once below.

        // Fetch the global transform once (reused by both the visual box and the pivot fallback).
        // Only a Node3D answers get_global_transform; guard with is_class so a plain (non-spatial)
        // node skips the call. A missing method silently returns nil in Godot 4.8 (no console spam),
        // but the guard avoids a pointless GDExtension round-trip and mirrors the get_aabb guard.
        float raw[12] = {};
        bool haveTransform = false;
        {
            const GodotVariant node3dArg = api.MakeString("Node3D");
            const GodotVariant isNode3d = api.Call(root, "is_class", &node3dArg, 1);
            if (api.TypeOf(isNode3d) == GDEXTENSION_VARIANT_TYPE_BOOL && api.AsBool(isNode3d))
            {
                GodotVariant tmV = api.Call(root, "get_global_transform");
                if (api.TypeOf(tmV) == GDEXTENSION_VARIANT_TYPE_TRANSFORM3D)
                {
                    api.AsTransform3D(tmV, raw); // basis rows (row-major 3x3) then origin.
                    haveTransform = true;
                }
            }
        }

        // get_aabb only exists on GeometryInstance3D; pre-guard with is_class so non-visual nodes
        // skip the call (a missing method silently returns nil in Godot 4.8, godot_migration.md §2).
        const GodotVariant classArg = api.MakeString("GeometryInstance3D");
        const GodotVariant isGeom = api.Call(root, "is_class", &classArg, 1);
        const bool isGeometryInstance =
            api.TypeOf(isGeom) == GDEXTENSION_VARIANT_TYPE_BOOL && api.AsBool(isGeom);

        if (haveTransform && isGeometryInstance)
        {
            GodotVariant aabbV = api.Call(root, "get_aabb");
            if (api.TypeOf(aabbV) == GDEXTENSION_VARIANT_TYPE_AABB)
            {
                float pos[3] = {};
                float size[3] = {};
                api.AsAabb(aabbV, pos, size);

                // Transform the 8 local-AABB corners by the node's global Transform3D and grow the
                // world box (a rotated AABB is no longer axis-aligned, so all corners are needed).
                for (int corner = 0; corner < 8; ++corner)
                {
                    const float lx = pos[0] + ((corner & 1) ? size[0] : 0.0f);
                    const float ly = pos[1] + ((corner & 2) ? size[1] : 0.0f);
                    const float lz = pos[2] + ((corner & 4) ? size[2] : 0.0f);
                    const AZ::Vector3 worldPt(
                        raw[0] * lx + raw[1] * ly + raw[2] * lz + raw[9],
                        raw[3] * lx + raw[4] * ly + raw[5] * lz + raw[10],
                        raw[6] * lx + raw[7] * ly + raw[8] * lz + raw[11]);
                    godotWorld.AddPoint(worldPt);
                }
            }
        }

        if (!godotWorld.IsValid())
        {
            // Non-visual node (not a GeometryInstance3D): NOT ray-pickable - return null so PickEntity
            // skips it (see IEntityMirror::GetWorldBounds). Still selectable via its editor icon / the
            // Outliner.
            return AZ::Aabb::CreateNull();
        }

        return EngineTransformConverter::ConvertAabb(k_space, godotWorld);
    }

    void GodotBackend::GodotEntityMirror::ReadProperties(GDExtensionObjectPtr node, PropertyBag& outBag) const
    {
        const GodotApi& api = m_state.m_api;

        GodotVariant className = api.Call(node, "get_class");
        AZStd::string classStr = api.VariantStringToAz(className);

        // The node name is always editable (Godot exposes it, but not via get_property_list),
        // so surface it first as a LineEdit under the class category.
        auto* nameProp = aznew EnginePropertyString();
        nameProp->m_name = "name";
        nameProp->m_category = classStr;
        GodotVariant nameV = api.Call(node, "get_name");
        nameProp->m_value = api.VariantStringToAz(nameV);
        outBag.m_items.push_back(nameProp);

        // Full property reflection (Plan §B4): walk get_property_list() which returns a
        // TypedArray<Dictionary> (name/class_name/type/hint/hint_string/usage). We surface only
        // properties that are both editor-visible and storable, and only the primitive Variant
        // types O3DE has stock property-grid editors for (bool/int/float/String/Vector3/Color).
        // Category/group rows (usage CATEGORY/GROUP/SUBGROUP) and unsupported types are skipped;
        // the transform is edited via the gizmo, so it is not duplicated here.
        // These usage/hint constants mirror Godot core/object/property_info.h (grep-confirmed).
        constexpr int64_t kUsageStorage = 1 << 1;
        constexpr int64_t kUsageEditor = 1 << 2;
        constexpr int64_t kUsageGroup = 1 << 6;
        constexpr int64_t kUsageCategory = 1 << 7;
        constexpr int64_t kUsageSubgroup = 1 << 8;
        constexpr int64_t kUsageInternal = 1 << 10;
        constexpr int64_t kUsageReadOnly = 1 << 28;
        constexpr int64_t kHintEnum = 2;

        GodotVariant listV = api.Call(node, "get_property_list");
        if (api.TypeOf(listV) != GDEXTENSION_VARIANT_TYPE_ARRAY)
        {
            return;
        }

        AZStd::string currentGroup; // last GROUP/SUBGROUP row, prefixed onto category for clarity
        const int64_t count = api.ArraySize(listV);
        for (int64_t i = 0; i < count; ++i)
        {
            GodotVariant entry = api.ArrayGet(listV, i);
            if (api.TypeOf(entry) != GDEXTENSION_VARIANT_TYPE_DICTIONARY)
            {
                continue;
            }

            const AZStd::string propName = api.VariantStringToAz(api.DictGet(entry, "name"));
            const int64_t type = api.AsInt(api.DictGet(entry, "type"));
            const int64_t usage = api.AsInt(api.DictGet(entry, "usage"));
            const int64_t hint = api.AsInt(api.DictGet(entry, "hint"));

            // Track grouping rows so the Inspector category reflects Godot's grouping, then skip
            // them (they carry no editable value).
            if (usage & (kUsageGroup | kUsageSubgroup))
            {
                currentGroup = propName;
                continue;
            }
            if (usage & kUsageCategory)
            {
                currentGroup.clear();
                continue;
            }
            // Must be both editor-visible and storable, and never the "name" (already added).
            // Skip INTERNAL properties (Godot engine internals not meant for user editing).
            if (usage & kUsageInternal || propName == "name")
            {
                continue;
            }
            if (!((usage & kUsageEditor) && (usage & kUsageStorage)))
            {
                continue;
            }

            const AZStd::string category =
                currentGroup.empty() ? classStr : (classStr + "/" + currentGroup);
            const bool readOnly = (usage & kUsageReadOnly) != 0;

            EngineProperty* prop = nullptr;
            switch (type)
            {
            case GDEXTENSION_VARIANT_TYPE_BOOL:
                {
                    auto* p = aznew EnginePropertyBool();
                    p->m_value = api.AsBool(api.GetProperty(node, propName.c_str()));
                    prop = p;
                }
                break;
            case GDEXTENSION_VARIANT_TYPE_INT:
                {
                    auto* p = aznew EnginePropertyInt();
                    p->m_value = api.AsInt(api.GetProperty(node, propName.c_str()));
                    if (hint == kHintEnum)
                    {
                        // hint_string is "Val1,Val2,..."; split into ComboBox entries so the
                        // selected index maps to the int value (matches EnginePropertyInt).
                        const AZStd::string hintStr = api.VariantStringToAz(api.DictGet(entry, "hint_string"));
                        SplitEnumNames(hintStr, p->m_enumNames);
                    }
                    prop = p;
                }
                break;
            case GDEXTENSION_VARIANT_TYPE_FLOAT:
                {
                    auto* p = aznew EnginePropertyDouble();
                    p->m_value = api.AsFloat(api.GetProperty(node, propName.c_str()));
                    prop = p;
                }
                break;
            case GDEXTENSION_VARIANT_TYPE_STRING:
            case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
            case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
                {
                    auto* p = aznew EnginePropertyString();
                    p->m_value = api.VariantStringToAz(api.GetProperty(node, propName.c_str()));
                    prop = p;
                }
                break;
            case GDEXTENSION_VARIANT_TYPE_VECTOR3:
                {
                    auto* p = aznew EnginePropertyVector3();
                    p->m_value = api.AsVector3(api.GetProperty(node, propName.c_str()));
                    prop = p;
                }
                break;
            case GDEXTENSION_VARIANT_TYPE_COLOR:
                {
                    auto* p = aznew EnginePropertyColor();
                    p->m_value = api.AsColor(api.GetProperty(node, propName.c_str()));
                    prop = p;
                }
                break;
            default:
                // Unsupported Variant type (Object/Array/Resource/...): not editable in v1.
                break;
            }

            if (prop)
            {
                prop->m_name = propName;
                prop->m_category = category;
                prop->m_readOnly = readOnly;
                outBag.m_items.push_back(prop);
            }
        }
    }

    void GodotBackend::GodotEntityMirror::MirrorNodeRecursive(
        GDExtensionObjectPtr node, AZ::EntityId parentId, AZStd::vector<AZ::Entity*>& outEntities)
    {
        if (!node || !m_state.m_api.IsValid())
        {
            return;
        }
        GodotApi& api = m_state.m_api;

        GodotVariant nameV = api.Call(node, "get_name");
        AZStd::string nodeName = api.VariantStringToAz(nameV);
        GodotVariant classV = api.Call(node, "get_class");
        AZStd::string className = api.VariantStringToAz(classV);

        AZ::Entity* entity = aznew AZ::Entity(nodeName.empty() ? "Node" : nodeName.c_str());
        auto* nodeComponent = entity->CreateComponent<EngineNodeComponent>();
        PropertyBag bag;
        ReadProperties(node, bag);
        nodeComponent->SetMirrorData(className, AZStd::move(bag));

        const uint64_t objectId = api.InstanceId(node);
        nodeComponent->SetNodeHandle(objectId);

        const AZ::EntityId entityId = entity->GetId();
        m_entityToNode[entityId] = objectId;
        if (parentId.IsValid())
        {
            m_pendingParent[entityId] = parentId;
        }
        outEntities.push_back(entity);

        // Recurse over children (get_child_count / get_child(i)), pre-order.
        GodotVariant countV = api.Call(node, "get_child_count");
        const int64_t count = api.AsInt(countV);
        for (int64_t i = 0; i < count; ++i)
        {
            GodotVariant idx = api.MakeInt(i);
            GodotVariant childV = api.Call(node, "get_child", &idx, 1);
            GDExtensionObjectPtr child = api.AsObject(childV);
            MirrorNodeRecursive(child, entityId, outEntities);
        }
    }

    void GodotBackend::GodotEntityMirror::SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities)
    {
        m_entityToNode.clear();
        m_pendingParent.clear();

        GodotApi& api = m_state.m_api;
        if (!api.IsValid() || !m_state.m_sceneTree)
        {
            return;
        }

        // Mirror the current scene (the level the user opened), not the whole root Window/Viewport
        // (which also holds our editor camera + overlay). get_current_scene is the loaded scene's
        // root node; fall back to nothing if no scene is active yet.
        GodotVariant sceneV = api.Call(m_state.m_sceneTree, "get_current_scene");
        GDExtensionObjectPtr sceneRoot = api.AsObject(sceneV);
        if (!sceneRoot)
        {
            return;
        }
        MirrorNodeRecursive(sceneRoot, AZ::EntityId(), outEntities);
        AZ_TracePrintf("CrossEngineEditor", "Godot mirror: %zu entities mirrored from scene.\n",
            outEntities.size());
    }

    void GodotBackend::GodotEntityMirror::FinishSync()
    {
        GodotApi& api = m_state.m_api;

        // Wire parent-child links so the Outliner tree forms (entities are now activated).
        for (const auto& [childId, parentId] : m_pendingParent)
        {
            AZ::TransformBus::Event(childId, &AZ::TransformBus::Events::SetParent, parentId);
        }
        m_pendingParent.clear();

        if (!api.IsValid())
        {
            return;
        }

        // Seed each mirror entity's world transform from its Godot node (Node3D only; 2D/other
        // nodes stay at identity). Converted Godot -> O3DE at the edge.
        for (const auto& [entityId, objectId] : m_entityToNode)
        {
            GDExtensionObjectPtr node = api.ObjectFromId(objectId);
            if (!node)
            {
                continue;
            }
            // Only Node3D responds to get_global_transform; guard by checking the returned type.
            GodotVariant tmV = api.Call(node, "get_global_transform");
            if (api.TypeOf(tmV) != GDEXTENSION_VARIANT_TYPE_TRANSFORM3D)
            {
                continue;
            }
            float raw[12] = {};
            api.AsTransform3D(tmV, raw);
            AZ::TransformBus::Event(
                entityId, &AZ::TransformBus::Events::SetWorldTM, GodotRawToO3deTransform(raw));
        }
    }

    void GodotBackend::GodotEntityMirror::OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm)
    {
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr node = ResolveNode(entityId);
        if (!node || !api.IsValid())
        {
            return;
        }
        float raw[12];
        O3deTransformToGodotRaw(worldTm, raw);
        GodotVariant tm = api.MakeTransform3D(raw);
        // Only Node3D has set_global_transform; a no-op for other node types (call is ignored).
        api.Call(node, "set_global_transform", &tm, 1);
    }

    void GodotBackend::GodotEntityMirror::OnEditorPropertyChanged(AZ::EntityId entityId)
    {
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr node = ResolveNode(entityId);
        if (!node || !api.IsValid())
        {
            return;
        }

        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationBus::Events::FindEntity, entityId);
        if (!entity)
        {
            return;
        }
        auto* nodeComponent = entity->FindComponent<EngineNodeComponent>();
        if (!nodeComponent)
        {
            return;
        }

        // Empty path (the property bus only identifies the component): re-push every mirrored
        // property to its Godot object by name (Plan §B4). "name" is special (set_name); all
        // other typed values go through the generic set(name, Variant). Read-only props are
        // skipped (the engine would reject them / they can't be edited in the grid anyway).
        for (EngineProperty* prop : nodeComponent->GetProperties().m_items)
        {
            if (prop->m_readOnly)
            {
                continue;
            }

            if (prop->m_name == "name")
            {
                if (auto* s = azrtti_cast<EnginePropertyString*>(prop))
                {
                    GodotVariant nameV = api.MakeString(s->m_value.c_str());
                    api.Call(node, "set_name", &nameV, 1);
                }
                continue;
            }

            GodotVariant value;
            bool haveValue = true;
            if (auto* b = azrtti_cast<EnginePropertyBool*>(prop))
            {
                value = api.MakeBool(b->m_value);
            }
            else if (auto* i = azrtti_cast<EnginePropertyInt*>(prop))
            {
                value = api.MakeInt(i->m_value);
            }
            else if (auto* d = azrtti_cast<EnginePropertyDouble*>(prop))
            {
                value = api.MakeFloat(d->m_value);
            }
            else if (auto* s = azrtti_cast<EnginePropertyString*>(prop))
            {
                value = api.MakeString(s->m_value.c_str());
            }
            else if (auto* v = azrtti_cast<EnginePropertyVector3*>(prop))
            {
                value = api.MakeVector3(v->m_value);
            }
            else if (auto* c = azrtti_cast<EnginePropertyColor*>(prop))
            {
                value = api.MakeColor(c->m_value);
            }
            else
            {
                haveValue = false;
            }

            if (haveValue)
            {
                api.SetProperty(node, prop->m_name.c_str(), value);
            }
        }
    }

    AZ::EntityId GodotBackend::GodotEntityMirror::CreateObject(const ObjectSpec& spec)
    {
        GodotApi& api = m_state.m_api;
        if (!api.IsValid() || !m_state.m_sceneTree)
        {
            return AZ::EntityId();
        }

        GodotVariant sceneV = api.Call(m_state.m_sceneTree, "get_current_scene");
        GDExtensionObjectPtr sceneRoot = api.AsObject(sceneV);
        if (!sceneRoot)
        {
            return AZ::EntityId();
        }

        const char* className = spec.m_typeId.empty() ? "Node3D" : spec.m_typeId.c_str();
        GDExtensionObjectPtr node = api.ConstructObject(className);
        if (!node)
        {
            return AZ::EntityId();
        }
        GodotVariant nodeV = api.MakeObject(node);
        api.Call(sceneRoot, "add_child", &nodeV, 1);

        // Position it (Node3D only) using the requested O3DE transform, converted to Godot.
        float raw[12];
        O3deTransformToGodotRaw(spec.m_transform, raw);
        GodotVariant tm = api.MakeTransform3D(raw);
        api.Call(node, "set_global_transform", &tm, 1);

        // The engine node now exists; its mirror AZ::Entity appears on the next SyncToEditor
        // (the shell calls RefreshFromEngine right after create/delete, and v1 re-mirrors the
        // whole scene).
        return AZ::EntityId();
    }

    void GodotBackend::GodotEntityMirror::DestroyObject(AZ::EntityId entityId)
    {
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr node = ResolveNode(entityId);
        if (!node || !api.IsValid())
        {
            return;
        }

        // Synchronous delete: detach from the parent, then free immediately. queue_free defers
        // to end-of-frame, so the follow-up RefreshFromEngine would re-mirror the condemned node
        // as a ghost entity before it dies. Editor UI actions run outside Main::iteration, so
        // sync free is safe here (the Godot editor itself memdeletes scene-tree nodes).
        GodotVariant nodeV = api.MakeObject(node);
        GDExtensionObjectPtr parent = api.AsObject(api.Call(node, "get_parent"));
        if (parent)
        {
            api.Call(parent, "remove_child", &nodeV, 1);
        }
        api.Call(node, "free");
        m_entityToNode.erase(entityId);
    }

    bool GodotBackend::GodotEntityMirror::SaveScene(const AZStd::string& path)
    {
        // Plan §B4 + godot_migration.md §2: the Godot native scene is the single source of truth. Persist it by
        // packing the current scene root into a PackedScene and writing it via ResourceSaver.
        //   PackedScene.pack(node) -> Error(0=OK); ResourceSaver.save(res, path, flags) -> Error.
        // (Both APIs grep-confirmed in the Godot checkout.)
        GodotApi& api = m_state.m_api;
        if (!api.IsValid() || !m_state.m_sceneTree)
        {
            return false;
        }

        GDExtensionObjectPtr sceneRoot = api.AsObject(api.Call(m_state.m_sceneTree, "get_current_scene"));
        if (!sceneRoot)
        {
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: no current scene to save.");
            return false;
        }

        // Resolve the target path: caller-provided, else the scene's own file path.
        AZStd::string targetPath = path;
        if (targetPath.empty())
        {
            targetPath = api.VariantStringToAz(api.Call(sceneRoot, "get_scene_file_path"));
        }
        if (targetPath.empty())
        {
            AZ_Warning("CrossEngineEditor", false,
                "Godot SaveScene: no path given and the scene has no file path.");
            return false;
        }

        GDExtensionObjectPtr packed = api.ConstructObject("PackedScene");
        if (!packed)
        {
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: failed to construct PackedScene.");
            return false;
        }

        GodotVariant rootV = api.MakeObject(sceneRoot);
        const int64_t packErr = api.AsInt(api.Call(packed, "pack", &rootV, 1));
        if (packErr != 0)
        {
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: pack() failed (Error %lld).",
                static_cast<long long>(packErr));
            return false;
        }

        GDExtensionObjectPtr saver = api.GetSingleton("ResourceSaver");
        if (!saver)
        {
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: ResourceSaver singleton missing.");
            return false;
        }

        // Crash-safe save (editor_polish.md P0-8 / M2, same dance as the rbfx backend): write to
        // a temp path, then atomically promote it over the target with the previous version kept
        // as .bak. A crash mid-save can only lose the temp file, never corrupt the scene file.
        // IMPORTANT: Godot's ResourceSaver matches format by file extension (.tscn/.scn/.res/.tres),
        // so the temp file MUST preserve the original extension. Insert ".cee_tmp" before it.
        const size_t dotPos = targetPath.rfind('.');
        AZStd::string tmpPath;
        if (dotPos != AZStd::string::npos)
        {
            tmpPath = targetPath.substr(0, dotPos) + ".cee_tmp" + targetPath.substr(dotPos);
        }
        else
        {
            tmpPath = targetPath + ".cee_tmp";
        }
        const AZStd::string bakPath = targetPath + ".bak";

        GodotVariant args[3];
        args[0] = api.MakeObject(packed);            // resource
        args[1] = api.MakeString(tmpPath.c_str());   // path
        args[2] = api.MakeInt(0);                    // flags (default)
        const int64_t saveErr = api.AsInt(api.Call(saver, "save", args, 3));
        if (saveErr != 0)
        {
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: ResourceSaver.save failed (Error %lld) for %s.",
                static_cast<long long>(saveErr), tmpPath.c_str());
            AZ::IO::SystemFile::Delete(tmpPath.c_str()); // do not leave a failed partial file.
            return false;
        }

        if (AZ::IO::SystemFile::Exists(targetPath.c_str()))
        {
            if (!AZ::IO::SystemFile::Rename(targetPath.c_str(), bakPath.c_str(), /*overwrite=*/true))
            {
                AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: could not back up %s.", targetPath.c_str());
                AZ::IO::SystemFile::Delete(tmpPath.c_str());
                return false;
            }
        }
        if (!AZ::IO::SystemFile::Rename(tmpPath.c_str(), targetPath.c_str(), /*overwrite=*/true))
        {
            // Promote failed and the original was already moved away: restore it. If even the
            // restore fails the only complete copy is stranded in the .bak - say so loudly.
            if (!AZ::IO::SystemFile::Rename(bakPath.c_str(), targetPath.c_str(), /*overwrite=*/true))
            {
                AZ_Warning("CrossEngineEditor", false,
                    "Godot SaveScene: promote AND restore failed; the intact scene is stranded at %s.",
                    bakPath.c_str());
            }
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: could not promote %s.", targetPath.c_str());
            AZ::IO::SystemFile::Delete(tmpPath.c_str());
            return false;
        }

        AZ_Printf("CrossEngineEditor", "Godot SaveScene: wrote %s (backup: %s)\n", targetPath.c_str(), bakPath.c_str());
        return true;
    }

    // ------------------------------------------------- Migration batch 1 stubs (rbfx-first)
    // Contract rule C4: every contract change stubs ALL backends in the same change. These are
    // pure virtual on IEntityMirror so they must exist to compile; they return "unsupported"
    // until the Godot backend gets its own migration pass (rbfx_migration.md §3.1).

    void GodotBackend::GodotEntityMirror::EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& /*out*/)
    {
        // TODO rbfx-first stub. Godot side would walk the ClassDB for Node types.
    }

    bool GodotBackend::GodotEntityMirror::RaycastScene(
        const AZ::Vector3& /*rayOrigin*/,
        const AZ::Vector3& /*rayDirection*/,
        AZ::Vector3& /*outHitPoint*/,
        AZ::Vector3& /*outHitNormal*/) const
    {
        // TODO rbfx-first stub. Godot side would use PhysicsDirectSpaceState3D::intersect_ray.
        return false;
    }

    bool GodotBackend::GodotEntityMirror::CreatePrefabFromNodes(
        const AZStd::vector<AZ::EntityId>& /*entityIds*/,
        const AZStd::string& /*path*/)
    {
        // TODO rbfx-first stub. Godot side would pack a PackedScene like SaveScene.
        return false;
    }

    bool GodotBackend::GodotEntityMirror::AssignMaterial(
        AZ::EntityId /*entityId*/, const AZStd::string& /*assetPath*/, int /*slot*/)
    {
        // TODO rbfx-first stub. Godot side would set surface_material_override on the MeshInstance3D.
        return false;
    }

    bool GodotBackend::GodotEntityMirror::AssignAnimation(
        AZ::EntityId /*entityId*/, const AZStd::string& /*assetPath*/)
    {
        // TODO rbfx-first stub. Godot side would add an AnimationPlayer and assign the
        // library (no auto-play - same contract rule as rbfx).
        return false;
    }

    AZStd::vector<AZ::u8> GodotBackend::GodotEntityMirror::SerializeNodes(
        const AZStd::vector<AZ::EntityId>& /*entityIds*/)
    {
        // TODO rbfx-first stub. Godot side would duplicate() nodes in memory instead of bytes.
        return {};
    }

    bool GodotBackend::GodotEntityMirror::PasteNodes(
        const AZStd::vector<AZ::u8>& /*data*/, AZ::EntityId /*parentId*/)
    {
        // TODO rbfx-first stub.
        return false;
    }

    // =====================================================================================
    // GodotAssetSource  (godot_migration.md §2)
    // =====================================================================================
    void GodotBackend::GodotAssetSource::EnumerateRoot(AZStd::vector<AssetEntryInfo>& out)
    {
        EnumerateGodotDirectory(m_state.m_projectPath, out);
    }

    void GodotBackend::GodotAssetSource::EnumerateChildren(
        const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out)
    {
        if (parent.m_isFolder)
        {
            EnumerateGodotDirectory(parent.m_path, out);
        }
    }
} // namespace CrossEngineEditor
