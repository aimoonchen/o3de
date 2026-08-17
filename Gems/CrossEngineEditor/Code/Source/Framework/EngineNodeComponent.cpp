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
        // Listen for this entity's transform changes to invalidate the world-bounds cache (fix 3).
        AZ::TransformNotificationBus::Handler::BusConnect(GetEntityId());
    }

    void EngineNodeComponent::Deactivate()
    {
        AZ::TransformNotificationBus::Handler::BusDisconnect();
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
        // so it registers in the visibility system and its editor icon has a world position. Fall
        // back to a small pivot box when the engine node has no renderable geometry.
        const AZ::Aabb visual = GetBackendVisualBounds();
        if (visual.IsValid())
        {
            return visual;
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
        // ray test, or it would steal clicks from real meshes behind it. Non-visual nodes stay
        // selectable via their editor icon and the Outliner. Computed on demand (no cache).
        return GetBackendVisualBounds();
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
