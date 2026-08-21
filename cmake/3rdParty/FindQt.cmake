#
# Copyright (c) Contributors to the Open 3D Engine Project.
# For complete copyright and license terms please see the LICENSE at the root of this distribution.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
#
# Qt 6.11.1 resolution - "system Qt" fallback.
#
# This is the same script shipped inside the qt-6.11.1-rev1-windows 3rdParty
# package (LY_PACKAGE_UNPACK_LOCATION/qt-6.11.1-rev1-windows/FindQt.cmake).
# The package copy resolves the SDK from "<package dir>/qt"; this repo-local
# copy serves machines WITHOUT that package (it is not hosted on the public
# O3DE package server), resolving a Qt 6.11.1 msvc2022_64 SDK installed on the
# machine instead, via -DQT_PATH=<path>. cmake/3rdPartyPackages.cmake chooses
# between the two: package when present, this fallback otherwise. A Qt
# online-installer SDK layout works unchanged - it is what the vendored
# package wraps.
#
# AzQtComponents legitimately uses Qt private headers (style/docking/hidpi);
# Qt6 still ships them and all symbols O3DE calls are exported, so we link the
# *Private modules to resolve the private include paths. Component library
# logic is unchanged.

if(TARGET 3rdParty::Qt::Core) # Check we are not called multiple times
    return()
endif()

# Root of the installed Qt SDK: the folder that contains bin/ and
# lib/cmake/Qt6/Qt6Config.cmake. Configure with
# -DQT_PATH=C:/Qt/6.11.1/msvc2022_64.
set(QT_PATH "" CACHE PATH "Root of the installed Qt 6.11.1 msvc2022_64 SDK")
mark_as_advanced(QT_PATH)
if(NOT QT_PATH OR NOT EXISTS "${QT_PATH}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR
        "No usable Qt SDK found: the qt-6.11.1 3rdParty package is missing "
        "(expected under ${LY_PACKAGE_UNPACK_LOCATION}/qt-6.11.1-rev1-windows) and "
        "QT_PATH does not point at a Qt 6.11.1 msvc2022_64 SDK. "
        "Pass -DQT_PATH=C:/Qt/6.11.1/msvc2022_64 (or restore the package).")
endif()

# Suppress Qt's "using private module headers" warning: AzQtComponents
# intentionally uses Qt private headers; this is a known, accepted design.
set(QT_NO_PRIVATE_MODULE_WARNING ON)

# find_package creates imported targets scoped to the calling directory/function
# by default, so Qt's own targets (Qt6::Core, Qt6::lrelease, ...) would not be
# visible where the versionless qt_* macros later run. Promote all imported
# targets created by find_package to GLOBAL so they are visible engine-wide
# (matches how the package-mode flow behaved).
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

# Qt6's config sets QT_DEFAULT_MAJOR_VERSION, but scoped to the calling
# directory/function it would not propagate to the directories that later call
# the versionless qt_* macros (e.g. qt_add_lrelease in ly_add_translations).
# Publish it to the cache so the versionless Qt macros know they are on Qt6
# everywhere.
set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Default Qt major version for versionless qt_* macros" FORCE)
mark_as_advanced(QT_DEFAULT_MAJOR_VERSION)

# Point find_package(Qt6) at the resolved SDK instead of whatever Qt a machine
# might have on its default prefix paths. This is the crux of the "no local
# dependency" change.
set(Qt6_DIR "${QT_PATH}/lib/cmake/Qt6" CACHE PATH "Qt6 CMake config dir (from QT_PATH)" FORCE)
mark_as_advanced(Qt6_DIR)
list(APPEND CMAKE_PREFIX_PATH "${QT_PATH}" "${QT_PATH}/lib/cmake")

# Private components required (see header comment). Link the *Private modules so
# include paths resolve; component library logic unchanged.
find_package(Qt6 REQUIRED
    COMPONENTS
        Core Gui Widgets Svg SvgWidgets Xml Network Concurrent Test
        OpenGLWidgets OpenGL LinguistTools
        GuiPrivate WidgetsPrivate CorePrivate
    NO_CMAKE_PACKAGE_REGISTRY
)

# Create 3rdParty::Qt::* alias targets expected by the rest of the engine.
# NOTE: we cannot use ly_create_alias(NAME Qt::Core ...) because it creates an
# internal *non-namespaced* backing target literally named "Qt::Core", which
# collides with Qt6's own version-less "Qt::Core" alias created by
# find_package(Qt6). So we create the 3rdParty::Qt::* targets directly with
# uniquely-named backing libraries.
# Core/Gui/Widgets back onto the *Private targets (which INTERFACE-link the
# public ones + add private include dirs) so existing private #includes compile
# untouched. Svg folds in SvgWidgets: QSvgWidget moved to QtSvgWidgets in Qt6
# and .ui files (AUTOUIC) need its include dir.
function(o3de_qt_alias alias_suffix)
    set(backing "o3de_3rdParty_Qt_${alias_suffix}")
    add_library(${backing} INTERFACE IMPORTED GLOBAL)
    set_target_properties(${backing} PROPERTIES GEM_MODULE TRUE)
    target_link_libraries(${backing} INTERFACE ${ARGN})
    add_library(3rdParty::Qt::${alias_suffix} ALIAS ${backing})
endfunction()
o3de_qt_alias(Core        Qt6::CorePrivate)
o3de_qt_alias(Gui         Qt6::GuiPrivate)
o3de_qt_alias(Widgets     Qt6::WidgetsPrivate)
o3de_qt_alias(Svg         Qt6::Svg Qt6::SvgWidgets)
o3de_qt_alias(Xml         Qt6::Xml)
o3de_qt_alias(Network     Qt6::Network)
o3de_qt_alias(Concurrent  Qt6::Concurrent)
o3de_qt_alias(Test        Qt6::Test)
o3de_qt_alias(OpenGL      Qt6::OpenGLWidgets)

# Zero Qt5 leftovers: turn pre-6.0 deprecated APIs into compile errors.
add_compile_definitions(QT_DISABLE_DEPRECATED_UP_TO=0x060000)

# Qt tools from the resolved SDK (used by AUTOUIC/lrelease/windeployqt below).
unset(QT_UIC_EXECUTABLE CACHE)
find_program(QT_UIC_EXECUTABLE uic HINTS "${QT_PATH}/bin")
mark_as_advanced(QT_UIC_EXECUTABLE)

# lrelease compiles .ts -> .qm. We invoke it directly from ly_add_translations
# (rather than the versionless qt_add_lrelease, whose generator expressions rely
# on Qt namespace variables that are not visible outside this package's function
# scope). Export the executable path via the cache so it is globally reachable.
unset(QT_LRELEASE_EXECUTABLE CACHE)
find_program(QT_LRELEASE_EXECUTABLE lrelease HINTS "${QT_PATH}/bin")
mark_as_advanced(QT_LRELEASE_EXECUTABLE)
if(NOT QT_LRELEASE_EXECUTABLE)
    message(FATAL_ERROR "Qt's lrelease executable not found under ${QT_PATH}/bin")
endif()

find_program(QT_WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_PATH}/bin")
mark_as_advanced(QT_WINDEPLOYQT_EXECUTABLE)
# Export for POST_BUILD deployment in Editor/CrossEngineEditor CMakeLists.
set(QT_WINDEPLOYQT_EXECUTABLE "${QT_WINDEPLOYQT_EXECUTABLE}" CACHE FILEPATH "Qt windeployqt tool (from QT_PATH)" FORCE)
