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

project(luna-prefs C)

include(webOS/webOS)

# -- the following two lines are commented
# -- as the global info comes from the top
# -- level cmake script. However, its useful
# -- to still use these for unit testing to
# -- check that this sub-component compiles.

#webos_modules_init(1 0 0 QUALIFIER RC2)
#webos_component(2 0 0)

MESSAGE(STATUS "BUILD START libluna-prefs..")

include(FindPkgConfig)

# -- add local include paths
include_directories(../include/)

# -- check for glib 2.0
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

# -- check for cjson
pkg_check_modules(CJSON REQUIRED cjson)
webos_add_compiler_flags(ALL ${CJSON_CFLAGS})

# commented: see note below
#include_directories(${CJSON_INCLUDE_DIRS}/cjson)

# -- Note that both $MJSON_INCLUDE_DIRS and 
# $CJSON_INCLUDE_DIRS point to the same path.
# lunaprops.h has an explicit cjson/json.h check.
 
# /usr/local/include/json.h --> from mjson
# /usr/local/include/cjson/json.h --> from cjson

# Given this, the best solution is to _NOT_ include the cjson
# include path. The confusion arises since mjson supplies
# a json.h which is different from cjson/json.h, and CMAKE
# is not able to figure out the proper json.h to be used,
# always overriding mjson's json.h

# -- check for sqlite 3.0
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
webos_add_compiler_flags(ALL ${SQLITE3_CFLAGS})
include_directories(${SQLITE3_DIRS})

webos_build_pkgconfig()
#webos_config_build_doxygen(doc Doxyfile)

# -- no way to disable warn_unused_result right now.
webos_add_compiler_flags(ALL -g -O3 -Wall -Wno-unused-but-set-variable -Wno-unused-variable -fno-exceptions)
webos_add_linker_options(ALL --no-undefined)

add_library(luna-prefs SHARED lunaprefs.c)
target_link_libraries(luna-prefs 
                      ${GLIB2_LDFLAGS} 
                      ${CJSON_LDFLAGS}
                      ${SQLITE3_LDFLAGS}
                      )

webos_build_library(NOHEADERS)
MESSAGE(STATUS "BUILD END libluna-prefs")
