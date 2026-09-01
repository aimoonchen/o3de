/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/CeePreferences.h>

#include <AzCore/IO/FileIO.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryImpl.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/Utils/Utils.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QCoreApplication>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    namespace
    {
        constexpr AZStd::string_view k_registryRoot = "/CEE/Preferences";

        AZStd::string JoinKey(AZStd::string_view field)
        {
            return AZStd::string::format("%.*s/%.*s",
                aznumeric_cast<int>(k_registryRoot.size()), k_registryRoot.data(),
                aznumeric_cast<int>(field.size()), field.data());
        }
    } // namespace

    void CeePreferences::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<CeePreferences>()
                ->Version(1)
                ->Field("autosaveEnabled", &CeePreferences::m_autosaveEnabled)
                ->Field("autosaveIntervalMinutes", &CeePreferences::m_autosaveIntervalMinutes)
                ->Field("showViewportHelpers", &CeePreferences::m_showViewportHelpers)
                ->Field("defaultGizmoStyle", &CeePreferences::m_defaultGizmoStyle);

            if (AZ::EditContext* edit = serialize->GetEditContext())
            {
                edit->Class<CeePreferences>("Preferences", "Cross-Engine Editor preferences")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(AZ::Edit::UIHandlers::Default, &CeePreferences::m_autosaveEnabled, "Autosave enabled",
                        "Periodically save the engine scene when it has unsaved changes")
                    ->DataElement(AZ::Edit::UIHandlers::Slider, &CeePreferences::m_autosaveIntervalMinutes, "Autosave interval (minutes)",
                        "How often to autosave (1..120)")
                    ->Attribute(AZ::Edit::Attributes::Min, 1)
                    ->Attribute(AZ::Edit::Attributes::Max, 120)
                    ->Attribute(AZ::Edit::Attributes::Step, 1)
                    ->DataElement(AZ::Edit::UIHandlers::CheckBox, &CeePreferences::m_showViewportHelpers, "Show viewport helpers",
                        "Wireframe gizmos for lights / cameras / empty nodes and other editor helpers in the viewport")
                    ->DataElement(AZ::Edit::UIHandlers::ComboBox, &CeePreferences::m_defaultGizmoStyle, "Default gizmo style",
                        "Transform gizmo look used on startup")
                    ->EnumAttribute(CeePreferences::GizmoStylePreference::Blender, "Blender")
                    ->EnumAttribute(CeePreferences::GizmoStylePreference::Unreal, "Unreal");
            }
        }
    }

    AZStd::string CeePreferences::SettingsFilePath()
    {
        return AZStd::string(QCoreApplication::applicationDirPath().toUtf8().constData()) + "/cee_preferences.setreg";
    }

    void CeePreferences::Load()
    {
        AZ::SettingsRegistryInterface* registry = AZ::SettingsRegistry::Get();
        if (registry == nullptr)
        {
            return;
        }

        // Merge the persisted file first (if present), then read each field; absent fields keep
        // their in-place defaults. The ANCHOR is load-bearing: DumpSettingsRegistryToStream
        // writes the subtree members WITHOUT the /CEE/Preferences path prefix (the export
        // visitor skips the root key name), so merging with the default (empty) anchor would
        // land them at the registry root where the reads below never look (review round 1,
        // review_editor_polish_deepseek.md R1).
        const AZStd::string file = SettingsFilePath();
        if (AZ::IO::FileIOBase::GetInstance() != nullptr && AZ::IO::FileIOBase::GetInstance()->Exists(file.c_str()))
        {
            registry->MergeSettingsFile(file, AZ::SettingsRegistryInterface::Format::JsonMergePatch, k_registryRoot);
        }

        auto readBool = [registry](const char* field, bool& value)
        {
            bool tmp = false;
            if (registry->Get(tmp, JoinKey(field).c_str()))
            {
                value = tmp;
            }
        };
        auto readInt = [registry](const char* field, int& value)
        {
            AZ::s64 tmp = 0;
            if (registry->Get(tmp, JoinKey(field).c_str()))
            {
                value = static_cast<int>(tmp);
            }
        };

        readBool("autosaveEnabled", m_autosaveEnabled);
        readInt("autosaveIntervalMinutes", m_autosaveIntervalMinutes);
        readBool("showViewportHelpers", m_showViewportHelpers);

        int style = static_cast<int>(m_defaultGizmoStyle);
        readInt("defaultGizmoStyle", style);
        style = AZStd::clamp(style, 0, 1);
        m_defaultGizmoStyle = static_cast<GizmoStylePreference>(style);

        m_autosaveIntervalMinutes = AZStd::clamp(m_autosaveIntervalMinutes, 1, 120);
    }

    void CeePreferences::Save() const
    {
        AZ::SettingsRegistryInterface* registry = AZ::SettingsRegistry::Get();
        if (registry == nullptr)
        {
            return;
        }

        registry->Set(JoinKey("autosaveEnabled").c_str(), m_autosaveEnabled);
        registry->Set(JoinKey("autosaveIntervalMinutes").c_str(), static_cast<AZ::s64>(m_autosaveIntervalMinutes));
        registry->Set(JoinKey("showViewportHelpers").c_str(), m_showViewportHelpers);
        registry->Set(
            JoinKey("defaultGizmoStyle").c_str(), static_cast<AZ::s64>(static_cast<int>(m_defaultGizmoStyle)));

        // Persist the /CEE/Preferences subtree to the settings file so the next launch merges it
        // back (anchored MergeSettingsFile in Load). The rest of the registry is not touched.
        const AZStd::string path = SettingsFilePath();
        AZ::IO::FileIOStream stream(path.c_str(), AZ::IO::OpenMode::ModeWrite);
        if (stream.IsOpen())
        {
            AZ::SettingsRegistryMergeUtils::DumperSettings dumper;
            dumper.m_prettifyOutput = true;
            AZ::SettingsRegistryMergeUtils::DumpSettingsRegistryToStream(
                *registry, k_registryRoot, stream, dumper);
        }
        else
        {
            AZ_Warning("CrossEngineEditor", false, "Preferences: cannot open %s for write.",
                path.c_str());
        }
    }
} // namespace CrossEngineEditor
