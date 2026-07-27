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

#include <QFileIconProvider>
#include <QFileInfo>

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
    // The Null backend has no engine scene, so "rendering" is just clearing the viewport
    // surface. The overlay batches (gizmos/grid) coming from GenericDebugDisplay are drawn
    // through the shared GL context via the small GLDebugRenderer. All GL calls run while
    // the viewport's QOpenGLWidget context is current (during its paintGL).

    bool NullBackend::NullSceneRenderer::EnsureInitialized()
    {
        return m_debugRenderer.Initialize();
    }

    void NullBackend::NullSceneRenderer::AttachToWindow(void* /*nativeWindowHandle*/, uint32_t width, uint32_t height)
    {
        m_width = AZStd::max(1u, width);
        m_height = AZStd::max(1u, height);
        EnsureInitialized();
    }

    void NullBackend::NullSceneRenderer::Resize(uint32_t width, uint32_t height)
    {
        m_width = AZStd::max(1u, width);
        m_height = AZStd::max(1u, height);
    }

    void NullBackend::NullSceneRenderer::RenderFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip)
    {
        if (!EnsureInitialized())
        {
            return;
        }
        m_debugRenderer.SetViewProjection(viewToClip * worldToView);
    }

    void NullBackend::NullSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        m_debugRenderer.SubmitLines(vertices);
    }

    void NullBackend::NullSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        m_debugRenderer.SubmitTriangles(vertices);
    }

    void NullBackend::NullSceneRenderer::SetDepthTest(bool enabled)
    {
        m_debugRenderer.SetDepthTest(enabled);
    }

    void NullBackend::NullSceneRenderer::ReleaseGraphics()
    {
        m_debugRenderer.Shutdown();
    }

    void NullBackend::NullSceneRenderer::InvalidateGraphics()
    {
        m_debugRenderer.InvalidateContext();
    }

    // --- NullEntityMirror ---

    void NullBackend::NullEntityMirror::SyncToEditor(AZStd::vector<AZ::Entity*>& /*outEntities*/)
    {
    }

    void NullBackend::NullEntityMirror::OnEditorTransformChanged(AZ::EntityId /*entityId*/, const AZ::Transform& /*worldTm*/)
    {
    }

    void NullBackend::NullEntityMirror::OnEditorPropertyChanged(AZ::EntityId /*entityId*/, const PropertyChange& /*change*/)
    {
    }

    AZ::EntityId NullBackend::NullEntityMirror::CreateObject(const ObjectSpec& /*spec*/)
    {
        return AZ::EntityId();
    }

    void NullBackend::NullEntityMirror::DestroyObject(AZ::EntityId /*entityId*/)
    {
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
                    if (isFile)
                    {
                        entry.m_extension = childPath.Extension().String();
                    }
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

    QIcon NullBackend::NullAssetSource::GetThumbnail(const AssetEntryInfo& entry)
    {
        // File-type icons from the platform (KISS); rendered previews are a real-backend job.
        QFileIconProvider provider;
        if (entry.m_isFolder)
        {
            return provider.icon(QFileIconProvider::Folder);
        }
        return provider.icon(QFileInfo(QString::fromUtf8(entry.m_path.c_str())));
    }
} // namespace CrossEngineEditor
