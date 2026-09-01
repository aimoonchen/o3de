/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Framework/EngineNodeComponent.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>

#include <AzCore/Component/TransformBus.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>

#include <Profiling/CrossEngineProfiler.h>

namespace CrossEngineEditor
{
    namespace
    {
        // Wireframe sizes (editor_polish.md P0-6), metres. Forward is the entity's local +Y:
        // both coordinate converters map mirror +Y to the engine forward axis (rbfx +Z /
        // Godot -Z, Plan §B6) - one convention serves every backend.
        constexpr float k_lightBodyRadius = 0.35f;    // Point/Directional light sphere.
        constexpr float k_lightRayInner = 0.45f;      // Point light ray start.
        constexpr float k_lightRayOuter = 0.75f;      // Point light ray end (also the pick extent).
        constexpr float k_sunArrowLength = 2.0f;      // Directional light forward arrow.
        constexpr float k_spotRadius = 0.7f;          // Spot light cone base radius.
        constexpr float k_spotLength = 1.5f;          // Spot light cone length.
        constexpr float k_camDepth = 1.2f;            // Camera frustum pyramid depth.
        constexpr float k_camHalfWidth = 0.4f;        // Camera far-rect half width (local X).
        constexpr float k_camHalfHeight = 0.28f;      // Camera far-rect half height (local Z).
        constexpr float k_emptyHalfExtent = 0.2f;     // Empty node box half extent.

        // Kind colors: light = warm yellow, camera = cyan, empty = gray. (const, not
        // constexpr - AZ::Color's constructor template is not constexpr.)
        const AZ::Color k_lightColor{ 0.95f, 0.85f, 0.35f, 1.0f };
        const AZ::Color k_cameraColor{ 0.35f, 0.8f, 0.85f, 1.0f };
        const AZ::Color k_emptyColor{ 0.65f, 0.65f, 0.65f, 1.0f };
    } // namespace

    void EngineNodeComponent::Reflect(AZ::ReflectContext* context)
    {
        PropertyBag::Reflect(context);

        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EngineNodeComponent, AzToolsFramework::Components::EditorComponentBase>()
                ->Version(2)
                ->Field("className", &EngineNodeComponent::m_className)
                ->Field("properties", &EngineNodeComponent::m_properties)
                // The engine-side node handle IS reflected so serialized state capture keeps it:
                // undo/redo restore and the level's root-instance overlay takeover re-construct
                // the component from serialized state, and without the field the handle would
                // reset to 0 and the backend could no longer resolve the node for bounds /
                // picking / write-back. It is never persisted to disk because the engine's native
                // scene is the source of truth and the mirror prefab is not saved (Plan §B4).
                ->Field("nodeHandle", &EngineNodeComponent::m_nodeHandle);

            if (AZ::EditContext* editContext = serialize->GetEditContext())
            {
                editContext->Class<EngineNodeComponent>("Engine Node", "A mirrored engine scene node")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "Engine")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &EngineNodeComponent::m_className, "Class", "Engine class name")
                    ->Attribute(AZ::Edit::Attributes::ReadOnly, true)
                    // The property set is shown through a dynamic edit-data provider, the
                    // same mechanism ScriptEditorComponent uses: each mirrored property gets
                    // its name / UI handler / enum / read-only supplied at runtime. No custom
                    // PropertyHandler is registered.
                    ->DataElement(nullptr, &EngineNodeComponent::m_properties, "Properties", "Engine node properties")
                    ->SetDynamicEditDataProvider(&EngineNodeComponent::GetPropertyEditData)
                    ->Attribute(AZ::Edit::Attributes::Visibility, AZ::Edit::PropertyVisibility::ShowChildrenOnly);
            }
        }
    }

    EngineNodeComponent::~EngineNodeComponent()
    {
        for (auto& [element, editData] : m_editDataByElement)
        {
            editData.ClearAttributes();
        }
        for (EngineProperty* prop : m_properties.m_items)
        {
            delete prop;
        }
    }

    void EngineNodeComponent::SetMirrorData(AZStd::string className, PropertyBag&& bag)
    {
        m_className = AZStd::move(className);

        // Release any property objects we currently own before taking the new set.
        for (EngineProperty* prop : m_properties.m_items)
        {
            delete prop;
        }
        m_properties = AZStd::move(bag);
        m_boundsCacheValid = false; // geometry may have changed with the new mirror data.

        RebuildEditData();
    }

    EngineProperty* EngineNodeComponent::FindProperty(AZStd::string_view name) const
    {
        for (EngineProperty* prop : m_properties.m_items)
        {
            if (prop && prop->m_name == name)
            {
                return prop;
            }
        }
        return nullptr;
    }

    void EngineNodeComponent::RebuildEditData()
    {
        // ElementData owns the attributes it allocates (enum entries), so free them first.
        for (auto& [element, editData] : m_editDataByElement)
        {
            editData.ClearAttributes();
        }
        m_editDataByElement.clear();

        for (EngineProperty* prop : m_properties.m_items)
        {
            if (!prop)
            {
                continue;
            }

            AZ::Edit::ElementData editData;
            // m_name / m_category own their storage and outlive this edit data (rebuilt on
            // every property-set change), so their c_str() is a stable label pointer.
            editData.m_name = prop->m_name.c_str();
            editData.m_description = prop->m_category.c_str();
            editData.m_elementId = prop->GetUIHandler();

            if (prop->m_readOnly)
            {
                editData.m_attributes.push_back(AZ::Edit::AttributePair(
                    AZ::Edit::Attributes::ReadOnly, aznew AZ::Edit::AttributeData<bool>(true)));
            }

            // Integer property with enum names -> ComboBox entries (one EnumValue attribute
            // per name), exactly as ScriptEditorComponent builds its enum attributes.
            if (auto* intProp = azrtti_cast<EnginePropertyInt*>(prop); intProp && !intProp->m_enumNames.empty())
            {
                using EnumPair = AZStd::pair<AZ::s64, AZStd::string>;
                for (AZ::s64 i = 0; i < static_cast<AZ::s64>(intProp->m_enumNames.size()); ++i)
                {
                    editData.m_attributes.push_back(AZ::Edit::AttributePair(
                        AZ::Edit::InternalAttributes::EnumValue,
                        aznew AZ::Edit::AttributeData<EnumPair>(EnumPair(i, intProp->m_enumNames[static_cast<size_t>(i)]))));
                }
            }

            // Key on the concrete value field address: that is the reflected editable leaf
            // the property grid passes to the dynamic provider. ElementData owns its enum /
            // read-only attributes, so it is stored exactly once to avoid a double free.
            m_editDataByElement.emplace(prop->GetValueAddress(), AZStd::move(editData));
        }
    }

    const AZ::Edit::ElementData* EngineNodeComponent::GetPropertyEditData(
        const void* handlerPtr, const void* elementPtr, const AZ::Uuid& /*elementType*/)
    {
        auto* self = const_cast<EngineNodeComponent*>(reinterpret_cast<const EngineNodeComponent*>(handlerPtr));
        auto it = self->m_editDataByElement.find(elementPtr);
        if (it == self->m_editDataByElement.end())
        {
            // A cache miss means the property objects were replaced out from under us - the only
            // path that does this is an Undo/Redo, which restores m_properties by deleting the old
            // EngineProperty objects and cloning new ones at fresh addresses (so both the cache
            // keys and the m_name label pointers are stale). Rebuild once from the live property
            // set and retry; without this the value row would vanish from the Inspector after the
            // first Ctrl+Z, and the stale c_str() labels would be a use-after-free read.
            self->RebuildEditData();
            it = self->m_editDataByElement.find(elementPtr);
        }
        return (it != self->m_editDataByElement.end()) ? &it->second : nullptr;
    }

    void EngineNodeComponent::Init()
    {
        EditorComponentBase::Init();
        // Edit data is built lazily by the dynamic provider (rebuild-on-miss), so no eager
        // build here: the property set may still be replaced by Undo before first display.
    }

    void EngineNodeComponent::Activate()
    {
        EditorComponentBase::Activate();
        // Two selection-facing buses, each with a distinct job:
        //  * EditorComponentSelectionRequestsBus -> the ONLY bus the viewport picker needs: O3DE's
        //    PickEntity requires a handler here to return valid bounds AND pass the precise ray test
        //    to register the click (the picker iterates FindVisibleEntities, not the visibility tree).
        //  * BoundsRequestBus -> registers the entity in the EntityVisibilityBoundsUnionSystem octree.
        //    NOT required for picking; no consumer reads that tree today. Implemented for O3DE parity
        //    (every selectable entity provides bounds) and to seed future box-select / frustum-cull.
        //    PRECONDITION: bounds queries must stay on the main thread - they reach the backend scene
        //    (GDExtension / rbfx), which is not thread-safe, and share the mutable bounds cache.
        AzFramework::BoundsRequestBus::Handler::BusConnect(GetEntityId());
        AzToolsFramework::EditorComponentSelectionRequestsBus::Handler::BusConnect(GetEntityId());
        // EntityDebugDisplayEventBus -> the wireframe gizmo for non-renderable nodes (P0-6),
        // drawn through the same per-entity display dispatch EditorHelpers already runs.
        AzFramework::EntityDebugDisplayEventBus::Handler::BusConnect(GetEntityId());
        // Listen for this entity's transform changes to invalidate the world-bounds cache (fix 3).
        AZ::TransformNotificationBus::Handler::BusConnect(GetEntityId());
    }

    void EngineNodeComponent::Deactivate()
    {
        AZ::TransformNotificationBus::Handler::BusDisconnect();
        AzFramework::EntityDebugDisplayEventBus::Handler::BusDisconnect();
        AzToolsFramework::EditorComponentSelectionRequestsBus::Handler::BusDisconnect();
        AzFramework::BoundsRequestBus::Handler::BusDisconnect();
        EditorComponentBase::Deactivate();
    }

    void EngineNodeComponent::OnTransformChanged(const AZ::Transform& /*local*/, const AZ::Transform& /*world*/)
    {
        // Transform moved (gizmo drag / parent move / sync) -> the cached world AABB is stale.
        m_boundsCacheValid = false;
    }

    AZ::Aabb EngineNodeComponent::GetBackendVisualBounds() const
    {
        // Transform-invalidated cache: return the memoised world AABB unless a transform change has
        // marked it dirty. During camera orbit over a static selection this hits every frame, so the
        // per-frame selection-outline + pick queries stop crossing the GDExtension boundary to
        // recompute an unchanged box (Progress.md 修复13 fix 3; consistent with Plan §B5b - nothing stale
        // is held because only a transform change alters a world AABB, and that invalidates here).
        if (m_boundsCacheValid)
        {
            return m_cachedVisualBounds;
        }
        // Raw engine-side bounds: the world AABB of THIS node's own renderable geometry, or a null
        // AABB for a non-visual node (light / empty / logic). Null here is meaningful - see the two
        // callers below, which treat "no visual" differently. Result is memoised until the next
        // transform change (including the null result, so non-visual nodes don't re-query either).
        AZ::Aabb bounds = AZ::Aabb::CreateNull();
        if (auto* backend = AZ::Interface<IEngineBackend>::Get())
        {
            bounds = backend->GetEntityMirror().GetWorldBounds(GetEntityId());
        }
        m_cachedVisualBounds = bounds;
        m_boundsCacheValid = true;
        return bounds;
    }

    AZ::Aabb EngineNodeComponent::GetWorldBounds() const
    {
        // Visibility-system bounds (BoundsRequestBus): a non-visual node still needs a valid bound
        // so it registers in the visibility system and its editor icon has a world position. For
        // wireframe kinds the wire extent is that bound (it matches what is on screen); other
        // non-visual nodes fall back to a small pivot box.
        const AZ::Aabb visual = GetBackendVisualBounds();
        if (visual.IsValid())
        {
            return visual;
        }
        if (GetWireKind() != WireKind::None)
        {
            AZ::Transform worldTm = AZ::Transform::CreateIdentity();
            AZ::TransformBus::EventResult(worldTm, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
            return GetLocalWireBounds().GetTransformedAabb(worldTm);
        }
        AZ::Vector3 worldPos = AZ::Vector3::CreateZero();
        AZ::TransformBus::EventResult(worldPos, GetEntityId(), &AZ::TransformBus::Events::GetWorldTranslation);
        return AZ::Aabb::CreateCenterRadius(worldPos, 0.25f);
    }

    AZ::Aabb EngineNodeComponent::GetLocalBounds() const
    {
        // The visibility system multiplies local bounds by the entity's world transform, so return
        // the world bounds expressed in local space (inverse world transform applied).
        AZ::Transform worldTm = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(worldTm, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
        const AZ::Aabb worldBounds = GetWorldBounds();
        return worldBounds.GetTransformedAabb(worldTm.GetInverse());
    }

    AZ::Aabb EngineNodeComponent::GetEditorSelectionBoundsViewport(const AzFramework::ViewportInfo& /*viewportInfo*/)
    {
        // Selection / ray-pick bounds: use the RAW visual bounds (null for a non-visual node), NOT
        // the visibility fallback box. A light / empty must not expose an invisible pivot box to the
        // ray test, or it would steal clicks from real meshes behind it. The ONE exception since
        // the wireframe gizmo (editor_polish.md P0-6): nodes with a wireframe representation expose
        // the wire's exact extent - the box is fully visible on screen, so clicking the light /
        // camera / empty selects it, while empty space around it still passes through.
        const AZ::Aabb visual = GetBackendVisualBounds();
        if (visual.IsValid() || GetWireKind() == WireKind::None)
        {
            return visual;
        }
        AZ::Transform worldTm = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(worldTm, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
        return GetLocalWireBounds().GetTransformedAabb(worldTm);
    }

    EngineNodeComponent::WireKind EngineNodeComponent::GetWireKind() const
    {
        // Exact-name classification against the known engine classes. rbfx has ONE Light
        // class (Light.h:183 - PointLight/SpotLight variants do not exist there; light type
        // is a property), Godot splits DirectionalLight3D / OmniLight3D / SpotLight3D.
        // Exact matching keeps user-defined class names containing these substrings out
        // (review round 1, R8). Unknown classes render nothing (WireKind::None) and keep the
        // pre-wireframe pick behaviour.
        if (m_className == "SpotLight3D")
        {
            return WireKind::SpotLight;
        }
        if (m_className == "DirectionalLight3D")
        {
            return WireKind::DirectionalLight;
        }
        if (m_className == "Light" || m_className == "OmniLight3D")
        {
            return WireKind::PointLight; // rbfx single Light class / Godot point light.
        }
        if (m_className == "Camera" || m_className == "Camera3D")
        {
            return WireKind::Camera;
        }
        if (m_className == "Node" || m_className == "Node3D")
        {
            return WireKind::EmptyNode;
        }
        return WireKind::None;
    }

    AZ::Aabb EngineNodeComponent::GetLocalWireBounds() const
    {
        // Local-space extent of the wireframe. The directional light's pick box covers only the
        // body sphere - its long forward arrow is decoration, and a 2 m pick box along it would
        // steal clicks from anything the sun ray visually crosses.
        switch (GetWireKind())
        {
        case WireKind::PointLight:
            return AZ::Aabb::CreateCenterRadius(AZ::Vector3::CreateZero(), k_lightRayOuter);
        case WireKind::DirectionalLight:
            return AZ::Aabb::CreateCenterRadius(AZ::Vector3::CreateZero(), k_lightBodyRadius);
        case WireKind::SpotLight:
            return AZ::Aabb::CreateFromMinMax(
                AZ::Vector3(-k_spotRadius, 0.0f, -k_spotRadius), AZ::Vector3(k_spotRadius, k_spotLength, k_spotRadius));
        case WireKind::Camera:
            return AZ::Aabb::CreateFromMinMax(
                AZ::Vector3(-k_camHalfWidth, 0.0f, -k_camHalfHeight),
                AZ::Vector3(k_camHalfWidth, k_camDepth, k_camHalfHeight));
        case WireKind::EmptyNode:
            return AZ::Aabb::CreateFromMinMax(
                AZ::Vector3(-k_emptyHalfExtent), AZ::Vector3(k_emptyHalfExtent));
        case WireKind::None:
            break;
        }
        return AZ::Aabb::CreateNull();
    }

    void EngineNodeComponent::DisplayEntityViewport(
        const AzFramework::ViewportInfo& /*viewportInfo*/, AzFramework::DebugDisplayRequests& debugDisplay)
    {
        // Wireframe gizmo for non-renderable nodes (editor_polish.md P0-6 / S2): lights /
        // cameras / empty nodes have no engine geometry, and viewport billboard icons are
        // structurally unavailable in CEE, so without this they are invisible and unpickable in
        // the viewport. Drawn in LOCAL space under the entity transform; forward = local +Y (see
        // the converter note above the sizing constants). Renderable nodes return above - their
        // engine geometry is the representation.
        const WireKind kind = GetWireKind();
        if (kind == WireKind::None)
        {
            return;
        }

        AZ::Transform worldTm = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(worldTm, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);

        debugDisplay.PushMatrix(worldTm);
        debugDisplay.SetColor(kind == WireKind::Camera
                ? k_cameraColor
                : (kind == WireKind::EmptyNode ? k_emptyColor : k_lightColor));
        debugDisplay.DepthTestOn();

        switch (kind)
        {
        case WireKind::PointLight:
            {
                // Blender point light: wire sphere + six short rays along the local axes.
                debugDisplay.DrawWireSphere(AZ::Vector3::CreateZero(), k_lightBodyRadius);
                for (int axis = 0; axis < 3; ++axis)
                {
                    AZ::Vector3 dir = AZ::Vector3::CreateZero();
                    dir.SetElement(axis, 1.0f);
                    debugDisplay.DrawArrow(dir * k_lightRayInner, dir * k_lightRayOuter, 0.5f, false);
                    debugDisplay.DrawArrow(-dir * k_lightRayInner, -dir * k_lightRayOuter, 0.5f, false);
                }
                break;
            }
        case WireKind::DirectionalLight:
            {
                // Sun: wire sphere + one long unidirectional arrow along local +Y (forward).
                debugDisplay.DrawWireSphere(AZ::Vector3::CreateZero(), k_lightBodyRadius);
                debugDisplay.DrawArrow(
                    AZ::Vector3::CreateZero(), AZ::Vector3::CreateAxisY(k_sunArrowLength), 0.15f, false);
                break;
            }
        case WireKind::SpotLight:
            {
                // Spot: cone opening along local +Y. DrawWireCone takes the BASE center and
                // direction towards the apex, so base forward, axis back.
                debugDisplay.DrawWireCone(
                    AZ::Vector3::CreateAxisY(k_spotLength), -AZ::Vector3::CreateAxisY(), k_spotRadius, k_spotLength);
                break;
            }
        case WireKind::Camera:
            {
                // Camera: frustum pyramid - apex at the origin, far rectangle at +Y depth,
                // plus a small "up" wedge above the far rectangle (Godot/Blender style).
                const AZ::Vector3 c0(-k_camHalfWidth, k_camDepth, -k_camHalfHeight);
                const AZ::Vector3 c1(k_camHalfWidth, k_camDepth, -k_camHalfHeight);
                const AZ::Vector3 c2(k_camHalfWidth, k_camDepth, k_camHalfHeight);
                const AZ::Vector3 c3(-k_camHalfWidth, k_camDepth, k_camHalfHeight);
                debugDisplay.DrawLine(AZ::Vector3::CreateZero(), c0);
                debugDisplay.DrawLine(AZ::Vector3::CreateZero(), c1);
                debugDisplay.DrawLine(AZ::Vector3::CreateZero(), c2);
                debugDisplay.DrawLine(AZ::Vector3::CreateZero(), c3);
                debugDisplay.DrawLine(c0, c1);
                debugDisplay.DrawLine(c1, c2);
                debugDisplay.DrawLine(c2, c3);
                debugDisplay.DrawLine(c3, c0);
                // Up wedge: marks the camera's +Z (O3DE up) on the far rect.
                const AZ::Vector3 wedgeTop = c3 + AZ::Vector3::CreateAxisZ(k_camHalfHeight * 0.6f);
                debugDisplay.DrawLine(c3, wedgeTop);
                debugDisplay.DrawLine(wedgeTop, c3 + AZ::Vector3(k_camHalfWidth * 0.4f, 0.0f, k_camHalfHeight * 0.6f));
                break;
            }
        case WireKind::EmptyNode:
            {
                // Plain scene node: small wire box at the transform origin (Blender empty).
                debugDisplay.DrawWireBox(AZ::Vector3(-k_emptyHalfExtent), AZ::Vector3(k_emptyHalfExtent));
                break;
            }
        case WireKind::None:
            break;
        }

        debugDisplay.PopMatrix();
    }

    bool EngineNodeComponent::SupportsEditorRayIntersectViewport(const AzFramework::ViewportInfo& /*viewportInfo*/)
    {
        return true;
    }

    bool EngineNodeComponent::EditorSelectionIntersectRayViewport(
        const AzFramework::ViewportInfo& viewportInfo,
        const AZ::Vector3& rayOrigin,
        const AZ::Vector3& rayDirection,
        float& distance)
    {
        CEE_PROFILE_FUNCTION();
        // Coarse cull first: the world AABB. Cheap reject for the common miss, and for non-visual
        // nodes (null bounds) it skips the precise test entirely so they never steal a click.
        const AZ::Aabb bounds = GetEditorSelectionBoundsViewport(viewportInfo);
        if (!bounds.IsValid() || !AzToolsFramework::AabbIntersectRay(rayOrigin, rayDirection, bounds, distance))
        {
            return false;
        }

        // Precise (triangle-level) test through the backend seam (Plan §B5b). This fixes
        // the rotated-mesh problem: a large model's world-axis-aligned AABB inflates to cover empty
        // space and wins the ray test over smaller objects behind it (rbfx "Geometry 100", a
        // scale-100 rotated teapot). When the backend supports a precise test, the triangle result
        // is authoritative - an AABB hit that misses every triangle is rejected. Backends without a
        // precise path leave the AABB decision (already in `distance`) in effect.
        if (auto* backend = AZ::Interface<IEngineBackend>::Get())
        {
            bool preciseHit = false;
            float preciseDistance = distance;
            if (backend->GetEntityMirror().RaycastNode(
                    GetEntityId(), rayOrigin, rayDirection, preciseHit, preciseDistance))
            {
                // Backend ran a precise test: its verdict wins over the coarse AABB.
                if (preciseHit)
                {
                    distance = preciseDistance;
                }
                return preciseHit;
            }
        }

        // No precise backend path available: keep the AABB-level decision (both engines' native
        // editor selection is AABB-level anyway).
        return true;
    }
} // namespace CrossEngineEditor
