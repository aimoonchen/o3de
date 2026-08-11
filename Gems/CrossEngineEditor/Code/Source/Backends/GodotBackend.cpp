/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Backends/GodotBackend.h>
#include <Framework/EngineNodeComponent.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Matrix3x3.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/std/string/conversions.h>

#include <cmath>
#include <cstdio>

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
        //! so a single pending pointer is safe (plan §3.2).
        GodotBackend::EngineState* g_initTarget = nullptr;

#if defined(AZ_PLATFORM_WINDOWS)
        // Godot 4.8's "--wid <HWND>" does NOT truly embed into the host client area: it only sets
        // the host as the Win32 *owner* of a still-independent top-level, borderless window (no
        // WS_CHILD) - so the editor viewport stays blank (see PROGRESS.md "Godot 视口嵌入待修").
        // The fix is to reparent Godot's real main window into our viewport HWND once it exists.
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
        // caller gates via m_reparented. Makes the window a hit-test-transparent, non-activating
        // WS_CHILD so Qt keeps camera/gizmo input and keyboard focus (plan §2.2).
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
            style |= WS_CHILD | WS_VISIBLE;
            ::SetWindowLongPtrW(godotHwnd, GWL_STYLE, style);

            LONG_PTR exStyle = ::GetWindowLongPtrW(godotHwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
            ::SetWindowLongPtrW(godotHwnd, GWL_EXSTYLE, exStyle);

            ::SetParent(godotHwnd, hostHwnd);
            ::MoveWindow(godotHwnd, 0, 0, static_cast<int>(state.m_width), static_cast<int>(state.m_height), TRUE);
            ::ShowWindow(godotHwnd, SW_SHOW);
            ::BringWindowToTop(godotHwnd);
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

        // ---- Coordinate conversion (O3DE Z-up <-> Godot Y-up, plan §5) ---------------------
        // All coordinate math lives in EngineTransformConverter (plan §5, one place). These thin
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

        //! Godot project asset extensions surfaced in the browser (plan §4).
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
                if (!entry.m_isFolder)
                {
                    entry.m_extension = info.suffix().toUtf8().constData();
                }
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
#if defined(AZ_PLATFORM_WINDOWS)
        if (m_state.m_instance == nullptr || !m_state.m_api.IsValid())
        {
            return;
        }

        // One-shot start() after creation: brings the main loop up and loads the project's main
        // scene (or the requested --scene). Done here (not in OnSurfaceCreated) so the first
        // iteration follows immediately (plan §2.4/§C7).
        if (!m_state.m_started)
        {
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
        GodotVariant quit = m_state.m_api.Call(m_state.m_instance, "iteration");
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
#endif
    }

    // =====================================================================================
    // GodotSceneRenderer  (G1.1 render bring-up + G1.2 overlay)
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

        // "--wid <hwnd>" reparents Godot's window to the editor HWND at creation (plan §3.1);
        // "--path <project>" opens the project. We drive editor camera/overlay ourselves, so we
        // do NOT pass --editor (which would bring up Godot's own editor UI).
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
            // renders the scene through it (plan §2.4).
            m_state.m_editorCamera = api.ConstructObject("Camera3D");
            if (m_state.m_editorCamera)
            {
                GodotVariant cam = api.MakeObject(m_state.m_editorCamera);
                api.Call(root, "add_child", &cam, 1);
                api.Call(m_state.m_editorCamera, "make_current");
            }

            // Overlay: a MeshInstance3D holding an ImmediateMesh, rebuilt every frame with the
            // gizmo/grid 3 primitives (plan §2.3).
            m_state.m_overlayMesh = api.ConstructObject("MeshInstance3D");
            m_state.m_overlayImmediate = api.ConstructObject("ImmediateMesh");
            if (m_state.m_overlayMesh && m_state.m_overlayImmediate)
            {
                GodotVariant immV = api.MakeObject(m_state.m_overlayImmediate);
                api.Call(m_state.m_overlayMesh, "set_mesh", &immV, 1);
                GodotVariant meshV = api.MakeObject(m_state.m_overlayMesh);
                api.Call(root, "add_child", &meshV, 1);
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

        // Drive the editor camera from the editor's view matrix (plan §2.4): camera world = inverse
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

        // Start a fresh overlay: clear last frame's surfaces and open a LINES surface.
        if (m_state.m_overlayImmediate)
        {
            api.Call(m_state.m_overlayImmediate, "clear_surfaces");
            m_frameOpen = true;
        }
    }

    void GodotBackend::GodotSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        if (!m_frameOpen || !m_state.m_overlayImmediate)
        {
            return;
        }
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr im = m_state.m_overlayImmediate;

        GodotVariant prim = api.MakeInt(k_primitiveLines);
        api.Call(im, "surface_begin", &prim, 1);
        for (const DebugVertex& v : vertices)
        {
            GodotVariant color = api.MakeColor(v.m_color);
            api.Call(im, "surface_set_color", &color, 1);
            GodotVariant pos = api.MakeVector3(GodotPosFromO3de(v.m_position));
            api.Call(im, "surface_add_vertex", &pos, 1);
        }
        api.Call(im, "surface_end");
    }

    void GodotBackend::GodotSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        if (!m_frameOpen || !m_state.m_overlayImmediate)
        {
            return;
        }
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr im = m_state.m_overlayImmediate;

        GodotVariant prim = api.MakeInt(k_primitiveTriangles);
        api.Call(im, "surface_begin", &prim, 1);
        for (const DebugVertex& v : vertices)
        {
            GodotVariant color = api.MakeColor(v.m_color);
            api.Call(im, "surface_set_color", &color, 1);
            GodotVariant pos = api.MakeVector3(GodotPosFromO3de(v.m_position));
            api.Call(im, "surface_add_vertex", &pos, 1);
        }
        api.Call(im, "surface_end");
    }

    void GodotBackend::GodotSceneRenderer::SetDepthTest(bool enabled)
    {
        m_depthTest = enabled;
        // v1: overlay uses the ImmediateMesh's default material. A depth-test-disabled material
        // for the SetDepthTest(false) pass is a later polish item (plan §2.3); the visual result
        // matches rbfx's depth-tested overlay for the common case.
    }

    void GodotBackend::GodotSceneRenderer::EndOverlayFrame()
    {
        // Surfaces are closed per-batch in SubmitLines/Triangles; Godot presents in Tick().
        m_frameOpen = false;
    }

    // =====================================================================================
    // GodotEntityMirror  (G1.3 tree mirror / G1.4 properties / G1.5 create-delete)
    // =====================================================================================
    GDExtensionObjectPtr GodotBackend::GodotEntityMirror::FindNode(AZ::EntityId entityId) const
    {
        auto it = m_entityToNode.find(entityId);
        if (it == m_entityToNode.end() || !m_state.m_api.IsValid())
        {
            return nullptr;
        }
        return m_state.m_api.ObjectFromId(it->second);
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

        // Full property reflection (plan §3.2/§3.4): walk get_property_list() which returns a
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
            if (!((usage & kUsageEditor) && (usage & kUsageStorage)) || propName == "name")
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
        m_nodeToEntity[objectId] = entityId;
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
        m_nodeToEntity.clear();
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
        GDExtensionObjectPtr node = FindNode(entityId);
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

    void GodotBackend::GodotEntityMirror::OnEditorPropertyChanged(AZ::EntityId entityId, const PropertyChange& /*change*/)
    {
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr node = FindNode(entityId);
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
        // property to its Godot object by name (plan §3.4). "name" is special (set_name); all
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

        // The engine node now exists; its mirror AZ::Entity appears on the next full SyncToEditor
        // (v1 re-mirrors the whole scene). NOTE: the shell does not yet trigger that re-sync from a
        // create/delete (the create/delete UI is not wired in v1), matching the rbfx backend.
        return AZ::EntityId();
    }

    void GodotBackend::GodotEntityMirror::DestroyObject(AZ::EntityId entityId)
    {
        GodotApi& api = m_state.m_api;
        GDExtensionObjectPtr node = FindNode(entityId);
        if (!node || !api.IsValid())
        {
            return;
        }
        api.Call(node, "queue_free");
        m_entityToNode.erase(entityId);
    }

    bool GodotBackend::GodotEntityMirror::SaveScene(const AZStd::string& path)
    {
        // Plan §3.7 + G1.5: the Godot native scene is the single source of truth. Persist it by
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

        GodotVariant args[3];
        args[0] = api.MakeObject(packed);       // resource
        args[1] = api.MakeString(targetPath.c_str()); // path
        args[2] = api.MakeInt(0);               // flags (default)
        const int64_t saveErr = api.AsInt(api.Call(saver, "save", args, 3));
        if (saveErr != 0)
        {
            AZ_Warning("CrossEngineEditor", false, "Godot SaveScene: ResourceSaver.save failed (Error %lld) for %s.",
                static_cast<long long>(saveErr), targetPath.c_str());
            return false;
        }

        AZ_Printf("CrossEngineEditor", "Godot SaveScene: wrote %s\n", targetPath.c_str());
        return true;
    }

    // =====================================================================================
    // GodotAssetSource  (G1.6)
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

    QIcon GodotBackend::GodotAssetSource::GetThumbnail(const AssetEntryInfo& /*entry*/)
    {
        return QIcon();
    }
} // namespace CrossEngineEditor
