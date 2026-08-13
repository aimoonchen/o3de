/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Backends/RbfxBackend.h>
#include <Framework/EngineNodeComponent.h>

#include <Profiling/CrossEngineProfiler.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Math/Color.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/MathUtils.h>

#include <cmath>
#include <limits>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
AZ_POP_DISABLE_WARNING

#if defined(_MSC_VER)
#    pragma warning(push, 0)
#    pragma warning(disable : 5054)  // rapidjson: operator '|' between different enum types (/we5054)
#endif
AZ_PUSH_DISABLE_WARNING(4251 4244 4245 4267 4100 4263 4264 4265 4266, "-Wunknown-warning-option")
#include <Urho3D/Core/Variant.h>
#include <Urho3D/Core/Attribute.h>
#include <Urho3D/Engine/EngineDefs.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/DebugRenderer.h>
#include <Urho3D/Graphics/Drawable.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/OctreeQuery.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Viewport.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/Math/Color.h>
#include <Urho3D/Math/Quaternion.h>
#include <Urho3D/Math/Ray.h>
#include <Urho3D/Math/Vector3.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Scene/Serializable.h>
AZ_POP_DISABLE_WARNING
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

namespace CrossEngineEditor
{
    namespace
    {
        constexpr EngineSpace k_space = EngineSpace::Rbfx;

        //! O3DE Vector3 -> rbfx Vector3, converting the coordinate basis at the edge.
        Urho3D::Vector3 ToRbfx(const AZ::Vector3& v)
        {
            const AZ::Vector3 e = EngineTransformConverter::PositionToEngine(k_space, v);
            return Urho3D::Vector3(e.GetX(), e.GetY(), e.GetZ());
        }

        //! rbfx Vector3 -> O3DE Vector3 (inverse basis conversion).
        AZ::Vector3 FromRbfx(const Urho3D::Vector3& v)
        {
            return EngineTransformConverter::PositionFromEngine(k_space, AZ::Vector3(v.x_, v.y_, v.z_));
        }

        //! O3DE color -> rbfx color (straight channel copy, no basis change).
        Urho3D::Color ToRbfxColor(const AZ::Color& c)
        {
            return Urho3D::Color(c.GetR(), c.GetG(), c.GetB(), c.GetA());
        }

        //! Attributes that duplicate what the editor already owns via the standard
        //! TransformComponent (world transform) or the AZ::Entity name, or the enabled flag the
        //! Outliner manages. Mirroring them into the property bag would create a second, unsynced
        //! editing path that fights the gizmo/Outliner, so they are skipped (plan §3.2: the
        //! transform is edited via the gizmo and must not be duplicated).
        bool IsShadowedByEditor(const ea::string& attrName)
        {
            return attrName == "Position" || attrName == "Rotation" || attrName == "Scale" ||
                attrName == "Name" || attrName == "Is Enabled";
        }

        //! Map an rbfx attribute value to a typed EngineProperty (plan section 3.2).
        //! Returns nullptr for types the editor grid has no stock control for (skipped).
        EngineProperty* MakeProperty(const Urho3D::AttributeInfo& info, const Urho3D::Variant& value)
        {
            const AZStd::string name(info.name_.c_str());
            // AM_READONLY / AM_NOEDIT attributes are shown but not editable (plan §3.2 m_readOnly),
            // matching the Godot backend which honours PROPERTY_USAGE_READ_ONLY.
            const bool readOnly = (info.mode_ & Urho3D::AM_READONLY) || (info.mode_ & Urho3D::AM_NOEDIT);

            EngineProperty* result = nullptr;
            switch (info.type_)
            {
            case Urho3D::VAR_BOOL:
                {
                    auto* p = aznew EnginePropertyBool();
                    p->m_value = value.GetBool();
                    result = p;
                    break;
                }
            case Urho3D::VAR_INT:
                {
                    auto* p = aznew EnginePropertyInt();
                    p->m_value = static_cast<AZ::s64>(value.GetInt());
                    // Enum attributes are VAR_INT with a non-empty enumNames_ -> ComboBox.
                    for (const auto& enumName : info.enumNames_)
                    {
                        p->m_enumNames.emplace_back(enumName.c_str());
                    }
                    result = p;
                    break;
                }
            case Urho3D::VAR_INT64:
                {
                    auto* p = aznew EnginePropertyInt();
                    p->m_value = static_cast<AZ::s64>(value.GetInt64());
                    result = p;
                    break;
                }
            case Urho3D::VAR_FLOAT:
                {
                    auto* p = aznew EnginePropertyDouble();
                    p->m_value = static_cast<double>(value.GetFloat());
                    result = p;
                    break;
                }
            case Urho3D::VAR_DOUBLE:
                {
                    auto* p = aznew EnginePropertyDouble();
                    p->m_value = value.GetDouble();
                    result = p;
                    break;
                }
            case Urho3D::VAR_STRING:
                {
                    auto* p = aznew EnginePropertyString();
                    p->m_value = AZStd::string(value.GetString().c_str());
                    result = p;
                    break;
                }
            case Urho3D::VAR_VECTOR3:
                {
                    auto* p = aznew EnginePropertyVector3();
                    const Urho3D::Vector3& v = value.GetVector3();
                    // Vector3-typed attributes are engine data (e.g. a size), not a world
                    // position, so they are shown raw (no coordinate-basis conversion).
                    p->m_value = AZ::Vector3(v.x_, v.y_, v.z_);
                    result = p;
                    break;
                }
            case Urho3D::VAR_COLOR:
                {
                    auto* p = aznew EnginePropertyColor();
                    const Urho3D::Color& c = value.GetColor();
                    p->m_value = AZ::Color(c.r_, c.g_, c.b_, c.a_);
                    result = p;
                    break;
                }
            default:
                return nullptr; // Type without a stock grid control; skipped in v1.
            }

            result->m_name = name;
            result->m_readOnly = readOnly;
            return result;
        }

        //! Push an edited EngineProperty value back into an rbfx attribute by index, preserving the
        //! engine's declared Variant type. The attribute's declared type (not the O3DE grid's
        //! widened type) drives the constructed Variant so 64-bit ints / doubles are not truncated
        //! to int / float on write-back.
        void WriteProperty(
            Urho3D::Serializable* node, unsigned index, const Urho3D::AttributeInfo& info, const EngineProperty* prop)
        {
            if (auto* b = azrtti_cast<const EnginePropertyBool*>(prop))
            {
                node->SetAttribute(index, Urho3D::Variant(b->m_value));
            }
            else if (auto* i = azrtti_cast<const EnginePropertyInt*>(prop))
            {
                // VAR_INT is 32-bit in rbfx; VAR_INT64 keeps full width.
                if (info.type_ == Urho3D::VAR_INT64)
                {
                    node->SetAttribute(index, Urho3D::Variant(static_cast<long long>(i->m_value)));
                }
                else
                {
                    node->SetAttribute(index, Urho3D::Variant(static_cast<int>(i->m_value)));
                }
            }
            else if (auto* d = azrtti_cast<const EnginePropertyDouble*>(prop))
            {
                if (info.type_ == Urho3D::VAR_DOUBLE)
                {
                    node->SetAttribute(index, Urho3D::Variant(d->m_value));
                }
                else
                {
                    node->SetAttribute(index, Urho3D::Variant(static_cast<float>(d->m_value)));
                }
            }
            else if (auto* s = azrtti_cast<const EnginePropertyString*>(prop))
            {
                node->SetAttribute(index, Urho3D::Variant(ea::string(s->m_value.c_str())));
            }
            else if (auto* v = azrtti_cast<const EnginePropertyVector3*>(prop))
            {
                node->SetAttribute(
                    index, Urho3D::Variant(Urho3D::Vector3(v->m_value.GetX(), v->m_value.GetY(), v->m_value.GetZ())));
            }
            else if (auto* c = azrtti_cast<const EnginePropertyColor*>(prop))
            {
                node->SetAttribute(index, Urho3D::Variant(ToRbfxColor(c->m_value)));
            }
        }
    } // namespace

    // ------------------------------------------------------------------ RbfxBackend

    RbfxBackend::RbfxBackend() = default;
    RbfxBackend::~RbfxBackend() = default;

    std::expected<void, BackendError> RbfxBackend::Initialize(const BackendInitParams& params)
    {
        // The engine is brought up lazily on first surface expose (OnSurfaceCreated), since
        // EP_EXTERNAL_WINDOW needs the editor HWND. Here we only stash the project path.
        m_state.m_projectPath = params.m_projectPath;
        m_state.m_scenePath = params.m_scenePath;
        return {};
    }

    void RbfxBackend::Shutdown()
    {
        if (m_state.m_engine)
        {
            m_state.m_scene.Reset();
            m_state.m_engine.Reset();
            m_state.m_context.Reset();
            m_state.m_initialized = false;
            m_state.m_camera = nullptr;
            m_state.m_cameraNode = nullptr;
            m_state.m_debug = nullptr;
        }
    }

    void RbfxBackend::Tick(float /*deltaSeconds*/)
    {
        // Intentionally does NOT drive RunFrame here. The engine frame is pumped once per overlay
        // cycle from RbfxSceneRenderer::EndOverlayFrame instead, so the scene and the just-submitted
        // DebugRenderer overlay always present together in the same frame. Driving RunFrame from
        // this (much higher-rate) idle tick presented ~15 of every 16 frames with an EMPTY
        // DebugRenderer (it auto-clears at each frame end), which made the grid/gizmo overlay
        // flicker. Presenting from EndOverlayFrame matches the DiligentBackend precedent and rbfx's
        // "submit-each-frame, cleared-at-frame-end" DebugRenderer model.
    }

    // ----------------------------------------------------------- RbfxSceneRenderer

    void RbfxBackend::RbfxSceneRenderer::OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height)
    {
        if (m_state.m_initialized || nativeWindowHandle == nullptr)
        {
            return;
        }

        m_state.m_context = Urho3D::MakeShared<Urho3D::Context>();
        m_state.m_engine = Urho3D::MakeShared<Urho3D::Engine>(m_state.m_context);

        // Render model A: hand rbfx the editor HWND via EP_EXTERNAL_WINDOW so SDL creates
        // its window from our surface and presents straight into the viewport.
        Urho3D::StringVariantMap params;
        params[Urho3D::EP_EXTERNAL_WINDOW] = static_cast<void*>(nativeWindowHandle);
        params[Urho3D::EP_WINDOW_WIDTH] = static_cast<int>(width);
        params[Urho3D::EP_WINDOW_HEIGHT] = static_cast<int>(height);
        params[Urho3D::EP_HEADLESS] = false;
        params[Urho3D::EP_FULL_SCREEN] = false;
        if (!m_state.m_projectPath.empty())
        {
            params[Urho3D::EP_RESOURCE_PREFIX_PATHS] = ea::string(m_state.m_projectPath.c_str());
        }

        if (!m_state.m_engine->Initialize(params, {}))
        {
            m_state.m_engine.Reset();
            m_state.m_context.Reset();
            return;
        }

        // Minimal scene: root gets a DebugRenderer (overlay), plus an editor camera node that
        // is driven each frame from the editor CameraState. The viewport binds them so rbfx
        // renders the scene through the editor camera.
        m_state.m_scene = Urho3D::MakeShared<Urho3D::Scene>(m_state.m_context);
        m_state.m_scene->CreateComponent<Urho3D::Octree>();
        m_state.m_debug = m_state.m_scene->CreateComponent<Urho3D::DebugRenderer>();

        m_state.m_cameraNode = m_state.m_scene->CreateChild("__EditorCamera");
        m_state.m_camera = m_state.m_cameraNode->CreateComponent<Urho3D::Camera>();

        if (auto* renderer = m_state.m_context->GetSubsystem<Urho3D::Renderer>())
        {
            Urho3D::SharedPtr<Urho3D::Viewport> viewport(
                new Urho3D::Viewport(m_state.m_context, m_state.m_scene, m_state.m_camera));
            renderer->SetViewport(0, viewport);
        }

        // Open the requested scene (plan section 3.2). LoadFile takes a resource-relative name
        // (e.g. "Scenes/RenderingShowcase_0.xml") resolved through EP_RESOURCE_PREFIX_PATHS +
        // the default "CoreData;Cache;Data" resource paths, and handles both .xml and .scene.
        // The editor camera child is recreated afterwards because LoadFile clears the scene.
        if (!m_state.m_scenePath.empty())
        {
            const bool loaded = m_state.m_scene->LoadFile(ea::string(m_state.m_scenePath.c_str()));
            AZ_Warning("CrossEngineEditor", loaded, "rbfx: failed to load scene '%s'.", m_state.m_scenePath.c_str());

            // LoadFile replaces the scene contents, dropping our Octree/DebugRenderer/camera.
            // Re-ensure the editor-owned components so overlay + camera rendering still work.
            if (!m_state.m_scene->GetComponent<Urho3D::Octree>())
            {
                m_state.m_scene->CreateComponent<Urho3D::Octree>();
            }
            m_state.m_debug = m_state.m_scene->GetOrCreateComponent<Urho3D::DebugRenderer>();

            m_state.m_cameraNode = m_state.m_scene->CreateChild("__EditorCamera");
            m_state.m_camera = m_state.m_cameraNode->CreateComponent<Urho3D::Camera>();
            if (auto* renderer = m_state.m_context->GetSubsystem<Urho3D::Renderer>())
            {
                Urho3D::SharedPtr<Urho3D::Viewport> viewport(
                    new Urho3D::Viewport(m_state.m_context, m_state.m_scene, m_state.m_camera));
                renderer->SetViewport(0, viewport);
            }
        }

        m_state.m_initialized = true;
    }

    void RbfxBackend::RbfxSceneRenderer::OnSurfaceResized(uint32_t /*width*/, uint32_t /*height*/)
    {
        // rbfx tracks the external window size itself via SDL; nothing to do in v1. A future
        // pass can call graphics->SetMode(...) if the swapchain needs an explicit resize.
    }

    void RbfxBackend::RbfxSceneRenderer::OnSurfaceAboutToBeDestroyed()
    {
        // The editor surface is going away: drop the scene/engine synchronously so rbfx does
        // not present into a dead HWND.
        m_state.m_scene.Reset();
        m_state.m_engine.Reset();
        m_state.m_context.Reset();
        m_state.m_initialized = false;
        m_state.m_camera = nullptr;
        m_state.m_cameraNode = nullptr;
        m_state.m_debug = nullptr;
    }

    void RbfxBackend::RbfxSceneRenderer::BeginOverlayFrame(
        const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip)
    {
        m_frameOpen = m_state.m_initialized && m_state.m_debug != nullptr;
        m_depthTest = true;

        // Drive the rbfx editor camera from the editor's view matrix (plan section 2.4). The
        // camera world transform is the inverse of world->view; convert it to rbfx space and
        // set the camera node so rbfx renders the scene through the editor camera. The
        // projection (FOV/near/far) is taken from view->clip.
        if (!m_state.m_camera || !m_state.m_cameraNode)
        {
            return;
        }

        const AZ::Matrix4x4 viewToWorld = worldToView.GetInverseTransform();
        const AZ::Vector3 camPos = viewToWorld.GetTranslation();
        const AZ::Quaternion camRot = AZ::Quaternion::CreateFromMatrix4x4(viewToWorld);

        // O3DE cameras look down local +Y; rbfx cameras look down local +Z. Rx(-90) maps the O3DE
        // forward (+Y) onto rbfx's +Z and keeps the up vector upright, but leaves the forward
        // pointing 180 deg away from the target; Rz(180) - a yaw about O3DE's local up (+Z) - then
        // flips the forward onto the target without disturbing the (already correct) up.
        // (Verified against the PROGRESS.md baseline: FWD points at the origin, UP.y > 0.)
        const AZ::Quaternion camForwardFix =
            AZ::Quaternion::CreateRotationX(-AZ::Constants::HalfPi) *
            AZ::Quaternion::CreateRotationZ(AZ::Constants::Pi);
        const AZ::Quaternion camRotFixed = camRot * camForwardFix;

        AZ::Vector3 pos;
        AZ::Quaternion rot;
        EngineTransformConverter::TransformToEngine(
            k_space, AZ::Transform::CreateFromQuaternionAndTranslation(camRotFixed, camPos), pos, rot);
        m_state.m_cameraNode->SetWorldPosition(Urho3D::Vector3(pos.GetX(), pos.GetY(), pos.GetZ()));
        m_state.m_cameraNode->SetWorldRotation(Urho3D::Quaternion(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()));

        // Recover a vertical FOV from the perspective projection matrix element [1][1] =
        // 1/tan(fovY/2); rbfx Camera takes FOV in degrees.
        const float clip11 = viewToClip.GetElement(1, 1);
        if (clip11 > 0.0001f)
        {
            const float fovYRadians = 2.0f * std::atan(1.0f / clip11);
            m_state.m_camera->SetFov(AZ::RadToDeg(fovYRadians));
        }
    }

    void RbfxBackend::RbfxSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        CEE_PROFILE_FUNCTION();
        if (!m_frameOpen)
        {
            return;
        }
        // Line list: consecutive pairs. Convert each endpoint O3DE -> rbfx at the edge.
        for (size_t i = 0; i + 1 < vertices.size(); i += 2)
        {
            m_state.m_debug->AddLine(
                ToRbfx(vertices[i].m_position), ToRbfx(vertices[i + 1].m_position),
                ToRbfxColor(vertices[i].m_color), m_depthTest);
        }
    }

    void RbfxBackend::RbfxSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        CEE_PROFILE_FUNCTION();
        if (!m_frameOpen)
        {
            return;
        }
        // Triangle list: consecutive triples.
        for (size_t i = 0; i + 2 < vertices.size(); i += 3)
        {
            m_state.m_debug->AddTriangle(
                ToRbfx(vertices[i].m_position), ToRbfx(vertices[i + 1].m_position), ToRbfx(vertices[i + 2].m_position),
                ToRbfxColor(vertices[i].m_color), m_depthTest);
        }
    }

    void RbfxBackend::RbfxSceneRenderer::SetDepthTest(bool enabled)
    {
        m_depthTest = enabled;
    }

    void RbfxBackend::RbfxSceneRenderer::EndOverlayFrame()
    {
        CEE_PROFILE_FUNCTION();
        // Pump exactly one rbfx frame now, at the close of the overlay cycle. The DebugRenderer
        // geometry submitted this cycle (SubmitLines/Triangles) is drawn by this RunFrame and then
        // auto-cleared at frame end, so scene + overlay always present coherently in lock-step
        // (one present per overlay submission) - this is what stops the overlay flickering. See the
        // note in RbfxBackend::Tick.
        if (m_state.m_initialized && m_state.m_engine && !m_state.m_engine->IsExiting())
        {
            CEE_PROFILE_SCOPE("Rbfx.RunFrame(present)");
            m_state.m_engine->RunFrame();
        }
        m_frameOpen = false;
    }

    // ------------------------------------------------------------ RbfxEntityMirror

    Urho3D::Node* RbfxBackend::RbfxEntityMirror::ResolveNode(AZ::EntityId entityId) const
    {
        // Single resolution path: read the rbfx node id off the live EngineNodeComponent's
        // reflected handle. The handle survives the editor entity context re-homing the mirror
        // entity into its prefab (which can change the AZ::EntityId), so this never silently
        // no-ops the way an entity-id -> node-id map would.
        if (!m_state.m_scene)
        {
            return nullptr;
        }
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationBus::Events::FindEntity, entityId);
        auto* comp = entity ? entity->FindComponent<EngineNodeComponent>() : nullptr;
        if (!comp)
        {
            AZ_Warning("CrossEngineEditor", false,
                "rbfx ResolveNode: no EngineNodeComponent for entity %s.", entityId.ToString().c_str());
            return nullptr;
        }
        Urho3D::Node* node = m_state.m_scene->GetNode(static_cast<unsigned>(comp->GetNodeHandle()));
        AZ_Warning("CrossEngineEditor", node != nullptr,
            "rbfx ResolveNode: node handle %llu no longer in scene for entity %s.",
            static_cast<AZ::u64>(comp->GetNodeHandle()), entityId.ToString().c_str());
        return node;
    }

    AZ::Aabb RbfxBackend::RbfxEntityMirror::GetWorldBounds(AZ::EntityId entityId) const
    {
        CEE_PROFILE_FUNCTION();
        Urho3D::Node* node = ResolveNode(entityId);
        if (!node)
        {
            return AZ::Aabb::CreateNull();
        }

        // Union the world bounding boxes of the Drawables ON THIS NODE ONLY (SelfDerived, not the
        // whole sub-tree) - see the per-node rationale in IEntityMirror::GetWorldBounds.
        Urho3D::BoundingBox engineBox; // default-constructed = undefined; Merge grows it.
        ea::vector<Urho3D::Drawable*> drawables;
        node->FindComponents<Urho3D::Drawable>(drawables, Urho3D::ComponentSearchFlag::SelfDerived);
        for (Urho3D::Drawable* drawable : drawables)
        {
            // Skip environment / non-geometry Drawables that editors never ray-pick: Skybox
            // (background, follows the camera with a huge world box that always wins the ray test),
            // Zone (lighting / fog / reflection volume - invisible bounds), and Light (selectable
            // via its icon, not its Drawable bounds). Matches Godot / Unity / Unreal, where sky and
            // volumes are excluded from viewport picking. IsInstanceOf catches subclasses too.
            if (drawable &&
                !drawable->IsInstanceOf(Urho3D::StringHash("Skybox")) &&
                !drawable->IsInstanceOf(Urho3D::StringHash("Zone")) &&
                !drawable->IsInstanceOf(Urho3D::StringHash("Light")))
            {
                engineBox.Merge(drawable->GetWorldBoundingBox());
            }
        }

        if (!engineBox.Defined())
        {
            // Non-visual node (no Drawable): NOT ray-pickable - return null so PickEntity skips it
            // (see IEntityMirror::GetWorldBounds). Still selectable via its editor icon / the Outliner.
            return AZ::Aabb::CreateNull();
        }

        const AZ::Aabb engineAabb = AZ::Aabb::CreateFromMinMax(
            AZ::Vector3(engineBox.min_.x_, engineBox.min_.y_, engineBox.min_.z_),
            AZ::Vector3(engineBox.max_.x_, engineBox.max_.y_, engineBox.max_.z_));
        return EngineTransformConverter::ConvertAabb(k_space, engineAabb);
    }

    bool RbfxBackend::RbfxEntityMirror::RaycastNode(
        AZ::EntityId entityId,
        const AZ::Vector3& rayOrigin,
        const AZ::Vector3& rayDirection,
        bool& outHit,
        float& outDistance) const
    {
        // Precise triangle-level pick for THIS node only (pick_final_plan §8 seam). Fixes the
        // rotated-mesh problem where a big model's world-axis-aligned AABB inflates to cover empty
        // space and steals clicks from smaller objects behind it (e.g. "Geometry 100", a scale-100
        // rotated teapot). We test the ray against the node's own drawables' triangles instead of
        // trusting the coarse AABB. Returns true (= "a precise test ran"), with outHit telling the
        // caller whether any triangle was actually hit; false means "no precise path, keep AABB".
        outHit = false;

        Urho3D::Node* node = ResolveNode(entityId);
        if (!node)
        {
            return false;
        }

        // Convert the O3DE-space ray to rbfx space. PositionToEngine is a pure linear basis remap
        // (no translation), so it is correct for both the origin (a point) and the direction (a
        // vector). Normalise the direction so RayQueryResult::distance_ is in world metres.
        const Urho3D::Vector3 rbfxOrigin = ToRbfx(rayOrigin);
        Urho3D::Vector3 rbfxDir = ToRbfx(rayDirection);
        const float dirLen = rbfxDir.Length();
        if (dirLen <= 0.0f)
        {
            return false;
        }
        rbfxDir /= dirLen;
        const Urho3D::Ray rbfxRay(rbfxOrigin, rbfxDir);

        // Gather the node's own drawables, applying the same environment exclusions as GetWorldBounds
        // (Skybox / Zone / Light are never ray-picked; they are selected via their editor icon).
        ea::vector<Urho3D::Drawable*> drawables;
        node->FindComponents<Urho3D::Drawable>(drawables, Urho3D::ComponentSearchFlag::SelfDerived);

        // Drive each drawable's own triangle test directly (Drawable::ProcessRayQuery) rather than an
        // Octree query: it targets exactly this node's geometry and does not depend on the octree
        // being populated (the editor app does not drive the game entity context that fills it).
        ea::vector<Urho3D::RayQueryResult> results;
        Urho3D::RayOctreeQuery query(results, rbfxRay, Urho3D::RAY_TRIANGLE);

        float nearest = std::numeric_limits<float>::max();
        for (Urho3D::Drawable* drawable : drawables)
        {
            if (!drawable ||
                drawable->IsInstanceOf(Urho3D::StringHash("Skybox")) ||
                drawable->IsInstanceOf(Urho3D::StringHash("Zone")) ||
                drawable->IsInstanceOf(Urho3D::StringHash("Light")))
            {
                continue;
            }

            results.clear();
            drawable->ProcessRayQuery(query, results);
            for (const Urho3D::RayQueryResult& r : results)
            {
                if (r.distance_ >= 0.0f && r.distance_ < nearest)
                {
                    nearest = r.distance_;
                }
            }
        }

        if (nearest < std::numeric_limits<float>::max())
        {
            // rbfx distance is along the unit ray in world metres. O3DE's AabbIntersectRay parametrises
            // distance along the ORIGINAL (possibly non-unit) direction, so divide back by dirLen to
            // return the same parameter units the caller compares across entities.
            outHit = true;
            outDistance = nearest / dirLen;
        }
        return true; // A precise test ran (hit or miss); its verdict is authoritative.
    }

    void RbfxBackend::RbfxEntityMirror::ReadProperties(Urho3D::Node* node, PropertyBag& outBag) const
    {
        const ea::vector<Urho3D::AttributeInfo>* attrs = node->GetAttributes();
        if (!attrs)
        {
            return;
        }
        for (unsigned i = 0; i < attrs->size(); ++i)
        {
            const Urho3D::AttributeInfo& info = attrs->at(i);
            // Skip attributes the editor already owns (world transform via the gizmo, entity name),
            // so there is no second, unsynced editing path (plan §3.2).
            if (IsShadowedByEditor(info.name_))
            {
                continue;
            }
            if (EngineProperty* prop = MakeProperty(info, node->GetAttribute(i)))
            {
                prop->m_category = node->GetTypeName().c_str();
                outBag.m_items.push_back(prop);
            }
        }
    }

    void RbfxBackend::RbfxEntityMirror::MirrorNodeRecursive(
        Urho3D::Node* node, AZ::EntityId parentId, AZStd::vector<AZ::Entity*>& outEntities)
    {
        if (!node)
        {
            return;
        }

        // One rbfx node = one AZ::Entity carrying an EngineNodeComponent (plan section 3.1).
        // The required editor components (incl. TransformComponent) are added by the shell.
        AZ::Entity* entity = aznew AZ::Entity(node->GetName().empty() ? "Node" : node->GetName().c_str());

        auto* nodeComponent = entity->CreateComponent<EngineNodeComponent>();
        PropertyBag bag;
        ReadProperties(node, bag);
        nodeComponent->SetMirrorData(node->GetTypeName().c_str(), AZStd::move(bag));
        nodeComponent->SetNodeHandle(node->GetID());

        const AZ::EntityId entityId = entity->GetId();
        m_entityToNode[entityId] = node->GetID();
        if (parentId.IsValid())
        {
            m_pendingParent[entityId] = parentId;
        }

        outEntities.push_back(entity);

        // Pre-order: parent precedes children in the output list.
        for (const Urho3D::SharedPtr<Urho3D::Node>& child : node->GetChildren())
        {
            MirrorNodeRecursive(child, entityId, outEntities);
        }
    }

    void RbfxBackend::RbfxEntityMirror::SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities)
    {
        m_entityToNode.clear();
        m_pendingParent.clear();

        if (!m_state.m_scene)
        {
            return;
        }

        // Mirror every child of the scene root (the root itself is the level container).
        for (const Urho3D::SharedPtr<Urho3D::Node>& child : m_state.m_scene->GetChildren())
        {
            // Skip the internal editor camera node.
            if (child->GetName() == "__EditorCamera")
            {
                continue;
            }
            MirrorNodeRecursive(child, AZ::EntityId(), outEntities);
        }
    }

    void RbfxBackend::RbfxEntityMirror::FinishSync()
    {
        // Entities are now activated; wire the parent-child links so the Outliner tree forms.
        for (const auto& [childId, parentId] : m_pendingParent)
        {
            AZ::TransformBus::Event(childId, &AZ::TransformBus::Events::SetParent, parentId);
        }
        m_pendingParent.clear();

        // Seed each mirror entity's world transform from its rbfx node (converted rbfx ->
        // O3DE) so the gizmo / Outliner show it in place. Done after activation so the editor
        // TransformComponent exists and TransformBus is live.
        for (const auto& [entityId, nodeId] : m_entityToNode)
        {
            Urho3D::Node* node = m_state.m_scene ? m_state.m_scene->GetNode(nodeId) : nullptr;
            if (!node)
            {
                continue;
            }
            const Urho3D::Vector3 wp = node->GetWorldPosition();
            const Urho3D::Quaternion wr = node->GetWorldRotation();
            const AZ::Vector3 pos = FromRbfx(wp);
            const AZ::Quaternion rot = EngineTransformConverter::RotationFromEngine(
                k_space, AZ::Quaternion(wr.x_, wr.y_, wr.z_, wr.w_));
            AZ::TransformBus::Event(
                entityId, &AZ::TransformBus::Events::SetWorldTM,
                AZ::Transform::CreateFromQuaternionAndTranslation(rot, pos));
        }
    }

    void RbfxBackend::RbfxEntityMirror::OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm)
    {
        Urho3D::Node* node = ResolveNode(entityId);
        if (!node)
        {
            return;
        }
        // Editor edits arrive in O3DE space; convert to rbfx at the edge (plan section 5).
        AZ::Vector3 pos;
        AZ::Quaternion rot;
        EngineTransformConverter::TransformToEngine(k_space, worldTm, pos, rot);
        node->SetWorldPosition(Urho3D::Vector3(pos.GetX(), pos.GetY(), pos.GetZ()));
        node->SetWorldRotation(Urho3D::Quaternion(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()));
    }

    void RbfxBackend::RbfxEntityMirror::OnEditorPropertyChanged(AZ::EntityId entityId, const PropertyChange& /*change*/)
    {
        Urho3D::Node* node = ResolveNode(entityId);
        if (!node)
        {
            return;
        }

        // Empty path (the property bus only identifies the component): re-push every mirrored
        // property to its rbfx attribute by matching name (plan section 3.4).
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

        const ea::vector<Urho3D::AttributeInfo>* attrs = node->GetAttributes();
        if (!attrs)
        {
            return;
        }
        for (unsigned i = 0; i < attrs->size(); ++i)
        {
            const Urho3D::AttributeInfo& info = attrs->at(i);
            if (IsShadowedByEditor(info.name_))
            {
                continue;
            }
            const AZStd::string name(info.name_.c_str());
            if (EngineProperty* prop = nodeComponent->FindProperty(name))
            {
                if (prop->m_readOnly)
                {
                    continue; // never push read-only attributes back to the engine.
                }
                WriteProperty(node, i, info, prop);
            }
        }
    }

    AZ::EntityId RbfxBackend::RbfxEntityMirror::CreateObject(const ObjectSpec& spec)
    {
        if (!m_state.m_scene)
        {
            return AZ::EntityId();
        }

        Urho3D::Node* node = m_state.m_scene->CreateChild(
            spec.m_typeId.empty() ? "Node" : ea::string(spec.m_typeId.c_str()));

        // Place it using the requested O3DE transform, converted to rbfx.
        AZ::Vector3 pos;
        AZ::Quaternion rot;
        EngineTransformConverter::TransformToEngine(k_space, spec.m_transform, pos, rot);
        node->SetWorldPosition(Urho3D::Vector3(pos.GetX(), pos.GetY(), pos.GetZ()));
        node->SetWorldRotation(Urho3D::Quaternion(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()));

        // The engine node now exists; its mirror AZ::Entity is created on the next full
        // SyncToEditor (v1 re-mirrors the whole scene rather than incrementally adding one
        // entity). NOTE: the shell does not yet trigger that re-sync from a create/delete (the
        // create/delete UI is not wired in v1); returning an invalid id reflects that. A targeted
        // single-entity add can be layered on later without changing the contract.
        return AZ::EntityId();
    }

    void RbfxBackend::RbfxEntityMirror::DestroyObject(AZ::EntityId entityId)
    {
        if (Urho3D::Node* node = ResolveNode(entityId))
        {
            node->Remove();
            m_entityToNode.erase(entityId);
        }
    }

    bool RbfxBackend::RbfxEntityMirror::SaveScene(const AZStd::string& path)
    {
        // Plan §3.7 + G1.5 parity: the rbfx native scene is the single source of truth. Save it
        // as XML to an absolute file (Scene::SaveXML(Serializer&) via a FILE_WRITE File). The
        // target is the caller path, else <projectPath>/<scenePath> (the file it was loaded from).
        if (!m_state.m_scene || !m_state.m_context)
        {
            return false;
        }

        AZStd::string targetPath = path;
        if (targetPath.empty())
        {
            if (m_state.m_scenePath.empty())
            {
                AZ_Warning("CrossEngineEditor", false, "rbfx SaveScene: no path and no loaded scene file.");
                return false;
            }
            targetPath = m_state.m_projectPath;
            if (!targetPath.empty() && targetPath.back() != '/' && targetPath.back() != '\\')
            {
                targetPath += '/';
            }
            targetPath += m_state.m_scenePath;
        }

        Urho3D::File file(m_state.m_context, ea::string(targetPath.c_str()), Urho3D::FILE_WRITE);
        if (!file.IsOpen())
        {
            AZ_Warning("CrossEngineEditor", false, "rbfx SaveScene: could not open %s for write.", targetPath.c_str());
            return false;
        }
        const bool ok = m_state.m_scene->SaveXML(file);
        AZ_Warning("CrossEngineEditor", ok, "rbfx SaveScene: SaveXML failed for %s.", targetPath.c_str());
        if (ok)
        {
            AZ_Printf("CrossEngineEditor", "rbfx SaveScene: wrote %s\n", targetPath.c_str());
        }
        return ok;
    }

    // ------------------------------------------------------------- RbfxAssetSource

    void RbfxBackend::RbfxAssetSource::EnumerateRoot(AZStd::vector<AssetEntryInfo>& out)
    {
        // Enumerate the top level of the rbfx project directory (plan section 4). The project
        // path is the resource root; each engine scene / material / resource is an asset.
        EnumerateDirectory(m_state.m_projectPath, out);
    }

    void RbfxBackend::RbfxAssetSource::EnumerateChildren(
        const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out)
    {
        if (parent.m_isFolder)
        {
            EnumerateDirectory(parent.m_path, out);
        }
    }

    void RbfxBackend::RbfxAssetSource::EnumerateDirectory(
        const AZStd::string& dirPath, AZStd::vector<AssetEntryInfo>& out) const
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

    QIcon RbfxBackend::RbfxAssetSource::GetThumbnail(const AssetEntryInfo& /*entry*/)
    {
        // v1: extension icons are supplied by the AssetBrowser panel's QFileIconProvider.
        return QIcon();
    }
} // namespace CrossEngineEditor
