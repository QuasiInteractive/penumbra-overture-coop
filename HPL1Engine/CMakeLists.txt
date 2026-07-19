cmake_minimum_required (VERSION 2.6)
project (HPL)

set(HPL_VERSION_MAJOR 1)
set(HPL_VERSION_MINOR 0)

if(DEFINED PENUMBRA_ENGINE_DEPENDENCIES_ROOT)
	set(_DEP_ROOT "${PENUMBRA_ENGINE_DEPENDENCIES_ROOT}")
else()
	set(_DEP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../dependencies")
endif()

include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/../OALWrapper/include
	"${_DEP_ROOT}/include"
)
file(GLOB_RECURSE all_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.c
)
# Removed Sources (CMake used to omit win32 for Linux-only setups; MSVC needs win32.cpp, not posix.)
file(GLOB_RECURSE to_remove_all Video*Theora.cpp)
file(GLOB _hpl_posix "${CMAKE_CURRENT_SOURCE_DIR}/sources/impl/platform/posix.cpp")
file(GLOB _hpl_win32 "${CMAKE_CURRENT_SOURCE_DIR}/sources/impl/platform/win32.cpp")
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	list(APPEND to_remove_all ${_hpl_posix})
else()
	list(APPEND to_remove_all ${_hpl_win32})
endif()
list(REMOVE_ITEM all_sources ${to_remove_all})
list(REMOVE_DUPLICATES all_sources)

add_library(HPL STATIC ${all_sources})

add_library(Newton STATIC IMPORTED)
add_library(angelscript STATIC IMPORTED)
add_library(SDLmain STATIC IMPORTED)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set_target_properties(Newton PROPERTIES
		IMPORTED_LOCATION "${_DEP_ROOT}/lib/win32/Newton.lib")
	set_target_properties(angelscript PROPERTIES
		IMPORTED_LOCATION "${_DEP_ROOT}/lib/win32/angelscript.lib")
	set_target_properties(SDLmain PROPERTIES
		IMPORTED_LOCATION "${_DEP_ROOT}/lib/win32/SDLmain.lib")
else()
	set_target_properties(Newton PROPERTIES
		IMPORTED_LOCATION "${_DEP_ROOT}/lib/linux/libNewton.a")
	set_target_properties(angelscript PROPERTIES
		IMPORTED_LOCATION "${_DEP_ROOT}/lib/linux/libangelscript.a")
	set_target_properties(SDLmain PROPERTIES
		IMPORTED_LOCATION "${_DEP_ROOT}/lib/linux/libSDLmain.a")
endif()
