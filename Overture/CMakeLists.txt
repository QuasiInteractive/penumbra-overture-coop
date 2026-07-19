# Penumbra: Overture (GPL game) — CMake 3.10+
# Engine layout per Frictional: sibling HPL1Engine + OALWrapper (NOT the INERTIA_ fork).
# ENet via vcpkg: configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#
# MSVC third-party blobs: CMake looks at ../dependencies by default; if lib/win32/Newton.lib is
# missing there, it probes C:/HPL1/dependencies (standalone / INERTIA_ tree). Headers and libs
# must remain a consistent Frictional-era set—mixing stale angelscript headers with upstream HPL1
# source will fail in script bindings (upgrade angelscript bundle or pin HPL1 to a matching commit).

cmake_minimum_required(VERSION 3.10)
project(Overture C CXX)

# OALWrapper uses FindPackage(SDL): without this, MSVC gets SDLmain.lib bundled into the static
# graph while HPL's LowLevelSystemSDL.cpp already defines WinMain -> duplicate symbol link errors.
set(SDL_BUILDING_LIBRARY TRUE CACHE BOOL "" FORCE)
# FindSDL.cmake caches SDL_LIBRARY with SDLmain in the string; flipping SDL_BUILDING_LIBRARY alone
# does not refresh — drop cached SDL results so Configure re-runs library discovery correctly.
unset(SDL_LIBRARY CACHE)
unset(SDLMAIN_LIBRARY CACHE)

option(PENUMBRA_MULTIPLAYER "Phase 1 ghost sync (requires vcpkg enet)" ON)

set(OAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../OALWrapper" CACHE PATH "Frictional OALWrapper clone")
set(HPL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../HPL1Engine" CACHE PATH "Frictional HPL1Engine clone")

if(NOT EXISTS "${HPL_ROOT}/include/hpl.h")
	message(FATAL_ERROR
		"HPL1Engine missing. Clone https://github.com/FrictionalGames/HPL1Engine into:\n  ${HPL_ROOT}")
endif()
if(NOT EXISTS "${OAL_ROOT}/CMakeLists.txt")
	message(FATAL_ERROR
		"OALWrapper missing. Clone https://github.com/FrictionalGames/OALWrapper into:\n  ${OAL_ROOT}")
endif()

message(STATUS "HPL_ROOT=${HPL_ROOT}")
message(STATUS "OAL_ROOT=${OAL_ROOT}")

# Optional override; single-line docstring — multi-line CACHE docs break CMake parsing here.
set(PENUMBRA_DEPENDENCIES_ROOT "" CACHE PATH
	"sibling ../dependencies unless empty; sibling without Newton.lib -> try C:/HPL1/deps.")

set(_deps_default "${CMAKE_CURRENT_SOURCE_DIR}/../dependencies")
set(_deps_root "${_deps_default}")
if(NOT "${PENUMBRA_DEPENDENCIES_ROOT}" STREQUAL "")
	set(_deps_root "${PENUMBRA_DEPENDENCIES_ROOT}")
endif()

if(WIN32)
	string(REPLACE "\\" "/" _deps_root "${_deps_root}")
	if("${PENUMBRA_DEPENDENCIES_ROOT}" STREQUAL ""
			AND NOT EXISTS "${_deps_root}/lib/win32/Newton.lib")
		foreach(_cand IN ITEMS "C:/HPL1/dependencies" "C:/hpl1/dependencies")
			if(EXISTS "${_cand}/lib/win32/Newton.lib")
				set(_deps_root "${_cand}")
				message(STATUS " MSVC deps auto-selected: ${_deps_root} (sibling ${_deps_default}/lib/win32 had no Newton.lib; INERTIA_ / standalone HPL1 tree)")
				break()
			endif()
		endforeach()
	endif()

	set(PENUMBRA_ENGINE_DEPENDENCIES_ROOT "${_deps_root}")
	link_directories("${_deps_root}/lib/win32")
endif()

# MSVC deprecation noise (sprintf, strncpy, fopen, …) across HPL + OALWrapper + Penumbra sources.
if(MSVC)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

add_subdirectory("${OAL_ROOT}" "${CMAKE_BINARY_DIR}/thirdparty/oalwrapper")
add_subdirectory("${HPL_ROOT}" "${CMAKE_BINARY_DIR}/thirdparty/hpl")

message(STATUS "PENUMBRA_DEPS_USED=${_deps_root}")

if(WIN32)
	if(NOT EXISTS "${_deps_root}/include")
		message(FATAL_ERROR "Missing Penumbra Windows dependency headers.\n"
			"Expected: ${_deps_root}/include/\n"
			"vcpkg supplies OAL audio libs (ogg/vorbis/SDL1/OpenAL).\n"
			"Newton, Cg, FLTK headers normally come alongside lib/win32 in the same MSVC bundle.\n"
			"Auto-probing C:/HPL1/dependencies kicks in only when sibling ../dependencies lacks Newton.lib; "
			"override explicitly with:\n"
			"  cmake -DPENUMBRA_DEPENDENCIES_ROOT=C:/path/to/dependencies ...")
	endif()
	file(GLOB _penumb_win32_libs LIST_DIRECTORIES false "${_deps_root}/lib/win32/*.lib")
	list(LENGTH _penumb_win32_libs _penumb_nlibs)
	if(_penumb_nlibs EQUAL "0")
		message(FATAL_ERROR
			"dependencies/lib/win32 has no \\\"*.lib\\\" files (directory empty or wrong path).\n"
			"Headers alone are not enough—you need the MSVC Win32 libs from Frictional’s original dependencies.zip bundle.\n"
			"\nWhere to get it (GitHub’s old \\\"downloads\\\" panel is gone; see discussion + mirrors):\n"
			"  https://github.com/FrictionalGames/HPL1Engine/issues/16\n"
			"\nUnzip so this path exists:\n"
			"  ${_deps_root}/lib/win32/Newton.lib\n"
			"\n(Optional for some VS setups:) also add GLaux.lib into lib/win32 (often mentioned in compile guides)."
	)
	endif()
	if(NOT EXISTS "${_deps_root}/lib/win32/Newton.lib")
		message(FATAL_ERROR
			"dependencies/lib/win32/Newton.lib is missing (${_penumb_nlibs} other .lib files were found—you may have a wrong/partial archive).\n"
			"Use Frictional’s Penumbra/HPL MSVC dependency bundle discussed here:\n"
			"  https://github.com/FrictionalGames/HPL1Engine/issues/16\n")
	endif()
endif()

include_directories(
	"${_deps_root}/include"
	"${HPL_ROOT}/include"
	"${OAL_ROOT}/include"
	${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX AND NOT APPLE)
	link_directories("${_deps_root}/lib/linux")
endif()

if(WIN32)
	set(CMAKE_EXECUTABLE_SUFFIX ".exe")
else()
	set(CMAKE_EXECUTABLE_SUFFIX ".bin")
endif()

file(GLOB PENUMBRA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB PENUMBRA_MP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/multiplayer/*.cpp")

set(PENUMBRA_ENET_TARGET "")
if(PENUMBRA_MULTIPLAYER)
	find_package(unofficial-enet CONFIG QUIET)
	if(TARGET unofficial::enet::enet)
		set(PENUMBRA_ENET_TARGET unofficial::enet::enet)
	elseif(TARGET unofficial::enet)
		set(PENUMBRA_ENET_TARGET unofficial::enet)
	else()
		find_package(enet CONFIG QUIET)
		if(TARGET enet::enet)
			set(PENUMBRA_ENET_TARGET enet::enet)
		elseif(TARGET enet)
			set(PENUMBRA_ENET_TARGET enet)
		endif()
	endif()
	if(NOT PENUMBRA_ENET_TARGET)
		message(WARNING
			"vcpkg package 'enet' not found via unofficial-enet (targets unofficial::enet::enet / unofficial::enet).\n"
			"Configure with: -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
			"Or turn off -DPENUMBRA_MULTIPLAYER=OFF — building without multiplayer.")
		set(PENUMBRA_MULTIPLAYER OFF CACHE BOOL "" FORCE)
	endif()
endif()

add_executable(Overture ${PENUMBRA_SRC} ${PENUMBRA_MP_SRC})
if(WIN32)
	set_target_properties(Overture PROPERTIES WIN32_EXECUTABLE TRUE)
	target_sources(Overture PRIVATE overture.rc) # co-op exe icon
endif()

# Quoted includes from multiplayer/*.cpp resolve same-dir headers; game code uses multiplayer/...
target_include_directories(Overture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/multiplayer")

# HPL ships WinMain in LowLevelSystemSDL.cpp calling hplMain — do not link SDLmain.lib.
# Friction-era MSVC .lib blobs are typically not SAFESEH-safe on x86; disable for the linker.
if(MSVC AND WIN32)
	target_link_options(Overture PRIVATE /SAFESEH:NO)
	target_link_libraries(Overture PRIVATE legacy_stdio_definitions)
endif()

if(PENUMBRA_MULTIPLAYER)
	target_compile_definitions(Overture PRIVATE PENUMBRA_MULTIPLAYER=1)
	target_link_libraries(Overture PRIVATE ${PENUMBRA_ENET_TARGET})
	if(WIN32)
		# iphlpapi: GetAdaptersInfo for per-subnet discovery broadcast (Hamachi et al.)
		target_link_libraries(Overture PRIVATE ws2_32 winmm iphlpapi)
	endif()
endif()

set_target_properties(Overture PROPERTIES
	INSTALL_RPATH_USE_LINK_PATH false
	OUTPUT_NAME overture)

if(WIN32)
	target_link_libraries(Overture
		PRIVATE
		HPL OALWrapper newton angelscript fltk cg cgGL
		libogg libvorbis libvorbisfile OpenAL32 alut SDL SDL_ttf SDL_image)
	target_link_libraries(Overture PRIVATE opengl32 glu32)
else()
	target_link_libraries(Overture
		PRIVATE
		HPL OALWrapper Newton angelscript fltk Cg CgGL ogg vorbis vorbisfile openal alut SDL SDL_ttf SDL_image)
	target_link_libraries(Overture PRIVATE GL GLU)
endif()
