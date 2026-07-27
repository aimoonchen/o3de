#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#

# Query the gem name from the gem.json file if possible, otherwise fallback to using CrossEngineEditor
o3de_find_ancestor_gem_root(gem_path gem_name "${CMAKE_CURRENT_SOURCE_DIR}")
if (NOT gem_name)
    set(gem_name "CrossEngineEditor")
endif()

add_subdirectory(Code)
