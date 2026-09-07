/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Backends/NullBackend.h>

#include <AzCore/IO/Path/Path.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/sort.h>

namespace CrossEngineEditor
{
    std::expected<void, BackendError> NullBackend::Initialize(const BackendInitParams& /*params*/)
    {
        return {};
    }

    void NullBackend::Shutdown()
    {
    }

    void NullBackend::Tick(float /*deltaSeconds*/)
    {
    }

    // --- NullSceneRenderer ---
    //
    // The Null backend has no engine scene and no RHI/swapchain. It is the fall-back used
    // only when the Diligent backend is not compiled in. It never reports a ready surface,
    // so the viewport controller skips all present/overlay work - the editor still runs its
    // camera/picking/manipulator logic against the camera state.

    void NullBackend::NullSceneRenderer::OnSurfaceCreated(void* /*nativeWindowHandle*/, uint32_t width, uint32_t height)
    {
        m_width = AZStd::max(1u, width);
        m_height = AZStd::max(1u, height);
    }

    void NullBackend::NullSceneRenderer::OnSurfaceResized(uint32_t width, uint32_t height)
    {
        m_width = AZStd::max(1u, width);
        m_height = AZStd::max(1u, height);
    }

    void NullBackend::NullSceneRenderer::OnSurfaceAboutToBeDestroyed()
    {
    }

    void NullBackend::NullSceneRenderer::BeginOverlayFrame(
        const AZ::Matrix4x4& /*worldToView*/, const AZ::Matrix4x4& /*viewToClip*/)
    {
    }

    void NullBackend::NullSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> /*vertices*/)
    {
    }

    void NullBackend::NullSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> /*vertices*/)
    {
    }

    void NullBackend::NullSceneRenderer::SetDepthTest(bool /*enabled*/)
    {
    }

    void NullBackend::NullSceneRenderer::EndOverlayFrame()
    {
    }

    // --- NullEntityMirror ---

    void NullBackend::NullEntityMirror::SyncToEditor(AZStd::vector<AZ::Entity*>& /*outEntities*/)
    {
    }

    void NullBackend::NullEntityMirror::EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& /*out*/)
    {
    }

    void NullBackend::NullEntityMirror::OnEditorTransformChanged(AZ::EntityId /*entityId*/, const AZ::Transform& /*worldTm*/)
    {
    }

    void NullBackend::NullEntityMirror::OnEditorPropertyChanged(AZ::EntityId /*entityId*/)
    {
    }

    AZ::EntityId NullBackend::NullEntityMirror::CreateObject(const ObjectSpec& /*spec*/)
    {
        return AZ::EntityId();
    }

    void NullBackend::NullEntityMirror::DestroyObject(AZ::EntityId /*entityId*/)
    {
    }

    // --- migration 批次 1 stubs (rbfx-first; Null backend has no engine scene to operate on) ---

    bool NullBackend::NullEntityMirror::RaycastScene(
        const AZ::Vector3& /*rayOrigin*/,
        const AZ::Vector3& /*rayDirection*/,
        AZ::Vector3& /*outHitPoint*/,
        AZ::Vector3& /*outHitNormal*/) const
    {
        return false;
    }

    bool NullBackend::NullEntityMirror::CreatePrefabFromNodes(
        const AZStd::vector<AZ::EntityId>& /*entityIds*/,
        const AZStd::string& /*path*/)
    {
        return false;
    }

    bool NullBackend::NullEntityMirror::AssignMaterial(
        AZ::EntityId /*entityId*/, const AZStd::string& /*assetPath*/, int /*slot*/)
    {
        return false;
    }

    bool NullBackend::NullEntityMirror::AssignAnimation(
        AZ::EntityId /*entityId*/, const AZStd::string& /*assetPath*/)
    {
        return false;
    }

    AZStd::vector<AZ::u8> NullBackend::NullEntityMirror::SerializeNodes(
        const AZStd::vector<AZ::EntityId>& /*entityIds*/)
    {
        return {};
    }

    bool NullBackend::NullEntityMirror::PasteNodes(
        const AZStd::vector<AZ::u8>& /*data*/, AZ::EntityId /*parentId*/)
    {
        return false;
    }

    // --- NullAssetSource ---
    //
    // With no engine attached the "asset source" simply exposes the editor project's
    // folder tree on disk, so the reused AssetBrowser panel has real content to browse.
    // A real backend overrides EnumerateRoot/Children with its own catalog.

    namespace
    {
        AZ::IO::FixedMaxPath ProjectRootPath()
        {
            AZ::IO::FixedMaxPath projectRoot;
            if (auto* registry = AZ::SettingsRegistry::Get())
            {
                registry->Get(projectRoot.Native(), AZ::SettingsRegistryMergeUtils::FilePathKey_ProjectPath);
            }
            return projectRoot;
        }

        void EnumerateFolder(AZStd::string_view folderPath, AZStd::vector<AssetEntryInfo>& out)
        {
            AZ::IO::FixedMaxPath filter{ folderPath };
            filter /= "*";
            AZ::IO::SystemFile::FindFiles(
                filter.c_str(),
                [&out, folderPath](const char* name, bool isFile) -> bool
                {
                    if (azstricmp(name, ".") == 0 || azstricmp(name, "..") == 0)
                    {
                        return true;
                    }
                    AZ::IO::FixedMaxPath childPath{ folderPath };
                    childPath /= name;

                    AssetEntryInfo entry;
                    entry.m_path = childPath.String();
                    entry.m_displayName = name;
                    entry.m_isFolder = !isFile;
                    out.push_back(AZStd::move(entry));
                    return true;
                });

            AZStd::sort(
                out.begin(), out.end(),
                [](const AssetEntryInfo& lhs, const AssetEntryInfo& rhs)
                {
                    if (lhs.m_isFolder != rhs.m_isFolder)
                    {
                        return lhs.m_isFolder; // folders first
                    }
                    return azstricmp(lhs.m_displayName.c_str(), rhs.m_displayName.c_str()) < 0;
                });
        }
    } // namespace

    void NullBackend::NullAssetSource::EnumerateRoot(AZStd::vector<AssetEntryInfo>& out)
    {
        const AZ::IO::FixedMaxPath projectRoot = ProjectRootPath();
        if (projectRoot.empty())
        {
            return;
        }
        EnumerateFolder(projectRoot.Native(), out);
    }

    void NullBackend::NullAssetSource::EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out)
    {
        if (!parent.m_isFolder)
        {
            return;
        }
        EnumerateFolder(parent.m_path, out);
    }
} // namespace CrossEngineEditor
