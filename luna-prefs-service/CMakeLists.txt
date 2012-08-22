# @@@LICENSE
#
# Copyright (c) 2012 Hewlett-Packard Development Company, L.P.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# LICENSE@@@

cmake_minimum_required(VERSION 2.8.7)

project(luna-prefs-service C)

include(webOS/webOS)

# -- the following two lines are commented
# -- as the global info comes from the top
# -- level cmake script. However, its useful
# -- to still use these for unit testing to
# -- check that this sub-component compiles.

webos_modules_init(1 0 0 QUALIFIER RC2)
webos_component(2 0 0)

MESSAGE (STATUS "BUILD START luna-prefs-service")

include(FindPkgConfig)

# -- add local include paths
include_directories(../include/)

# -- check for glib 2.0
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

# -- check for mjson
pkg_check_modules(MJSON REQUIRED mjson)
webos_add_compiler_flags(ALL ${MJSON_CFLAGS})
include_directories(${MJSON_INCLUDE_DIRS})

# -- check for cjson
pkg_check_modules(CJSON REQUIRED cjson)
webos_add_compiler_flags(ALL ${CJSON_CFLAGS})

# commented to get correct json.h files
#include_directories(${CJSON_INCLUDE_DIRS}/cjson)

# -- check for LS2 (internally depends on pmloglib)
pkg_check_modules(LS2 REQUIRED luna-service2)
include_directories(LS2_INCLUDE_DIRS)
webos_add_compiler_flags(ALL ${LS2_CFLAGS})

# -- check for liblunaprefs
pkg_check_modules(LUNA_PREFS REQUIRED luna-prefs)
webos_add_compiler_flags(ALL ${LUNA_PREFS_CFLAGS})
include_directories(${LUNA_PREFS_INCLUDE_DIRS})

#webos_build_pkgconfig()
#webos_config_build_doxygen(doc Doxyfile)

# -- no way to disable warn_unused_result right now.
webos_add_compiler_flags(ALL -g -O3 -Wall -Wno-unused-but-set-variable -Wno-unused-variable -fno-exceptions)
webos_add_linker_options(ALL --no-undefined)

add_executable(luna-prefs-service main.c)
target_link_libraries(luna-prefs-service
                      ${GLIB2_LDFLAGS} 
                      ${CJSON_LDFLAGS}
                      ${MJSON_LDFLAGS}
                      ${LS2_LDFLAGS}
                      ${LUNA_PREFS_LDFLAGS}
                      )

webos_build_program()
MESSAGE(STATUS "BUILD END luna-prefs-service")
