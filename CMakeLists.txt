cmake_minimum_required(VERSION 3.21)

# ---- Options ----

option(COPY_BUILD "Copy the build output to the Fallout 4 directory." OFF)
option(BUILD_GPU_TESTS "Build tests/GpuTests (GPU pipeline tests that run without Fallout 4)." OFF)
option(REQUIRE_PRECOMPILED_SHADERS "Fail the configure step when fxc is not found, instead of building a DLL that compiles its shaders at runtime." ON)

# ---- Cache build vars ----

macro(set_from_environment VARIABLE)
	if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif ()
endmacro()

set_from_environment(Fallout4Path)
set_from_environment(VCPKG_ROOT)

if (DEFINED VCPKG_ROOT)
	set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
else ()
	message(
		WARNING
		"Variable VCPKG_ROOT is not set. Continuing without vcpkg."
	)
endif ()

# The C/C++ runtime is linked statically. A /MD build of the current MSVC STL
# crashes on its first std::mutex lock when the process has loaded an
# msvcp140.dll older than 14.40, which Proton prefixes and older Windows
# installs can have.
set(Boost_USE_STATIC_RUNTIME ON CACHE BOOL "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

# ---- Project ----

project(
	GPUWorldPipeline
	VERSION 0.1.0
	LANGUAGES CXX
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
	@ONLY
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	@ONLY
)

# ---- Include guards ----

if (PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(
		FATAL_ERROR
		"In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
	)
endif ()

# ---- Globals ----

if (MSVC)
	if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
		add_compile_options(
			/MP	# Build with Multiple Processes
		)
	endif ()
endif ()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(Boost_USE_STATIC_LIBS ON)

# ---- Dependencies ----

if (NOT TARGET CommonLibF4)
	add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/CommonLibF4RD/CommonLibF4" CommonLibF4)
endif ()

find_package(fmt REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)

# ---- Add source files ----

include(cmake/sourcelist.cmake)

source_group(
	TREE ${CMAKE_CURRENT_SOURCE_DIR}
	FILES ${SOURCES} ${SHADERS_ABS}
)

source_group(
	TREE ${CMAKE_CURRENT_BINARY_DIR}
	FILES ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
)

# ---- Embedded shaders ----

set(EMBEDDED_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/include/EmbeddedShaders.h")

add_custom_command(
	OUTPUT "${EMBEDDED_SHADER_HEADER}"
	COMMAND
		${CMAKE_COMMAND}
		"-DOUTPUT=${EMBEDDED_SHADER_HEADER}"
		"-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders"
		"-DSHADERS=${SHADERS}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateShaderHeader.cmake"
	DEPENDS
		${SHADERS_ABS}
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateShaderHeader.cmake"
	COMMENT "Embedding HLSL shaders"
	VERBATIM
)

add_custom_target(GenerateEmbeddedShaders DEPENDS "${EMBEDDED_SHADER_HEADER}")

# ---- Precompiled shaders ----

# fxc from the Windows SDK compiles the compute shaders at build time, so the
# DLL needs no shader compiler at runtime. Wine/Proton's built-in
# d3dcompiler_47 cannot compile them.
set(_programFilesX86 "ProgramFiles(x86)")
set(_windowsKitsBin "$ENV{${_programFilesX86}}/Windows Kits/10/bin")
set(_fxcHints)
foreach (_sdkVersion IN ITEMS "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}" "$ENV{WindowsSDKVersion}")
	string(REGEX REPLACE "[\\/]+$" "" _sdkVersion "${_sdkVersion}")
	if (_sdkVersion)
		list(APPEND _fxcHints "${_windowsKitsBin}/${_sdkVersion}/x64")
	endif ()
endforeach ()
file(GLOB _installedSdks LIST_DIRECTORIES true "${_windowsKitsBin}/10.*")
list(SORT _installedSdks COMPARE NATURAL ORDER DESCENDING)
foreach (_sdk IN LISTS _installedSdks)
	list(APPEND _fxcHints "${_sdk}/x64")
endforeach ()
find_program(FXC_EXECUTABLE fxc HINTS ${_fxcHints})

set(PRECOMPILED_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/include/PrecompiledShaders.h")
set(PRECOMPILED_SHADER_SOURCES)
if (FXC_EXECUTABLE)
	message(STATUS "Precompiling shaders with ${FXC_EXECUTABLE}")
	set(_bytecodeDir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	set(_bytecodeFiles)
	foreach (_shader IN LISTS COMPUTE_SHADERS)
		string(REPLACE ":" ";" _parts "${_shader}")
		list(GET _parts 0 _file)
		list(GET _parts 1 _entry)
		string(MAKE_C_IDENTIFIER "${_file}_${_entry}" _ident)
		set(_bytecode "${_bytecodeDir}/${_ident}.cso")

		# The same profile and flags as ShaderLibrary's runtime compile.
		add_custom_command(
			OUTPUT "${_bytecode}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bytecodeDir}"
			COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_0 /E "${_entry}" /O3 /Ges /Fo "${_bytecode}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_file}"
			DEPENDS ${SHADERS_ABS}
			COMMENT "Compiling ${_file}:${_entry}"
			VERBATIM
		)
		list(APPEND _bytecodeFiles "${_bytecode}")
	endforeach ()

	add_custom_command(
		OUTPUT "${PRECOMPILED_SHADER_HEADER}"
		COMMAND
			"${CMAKE_COMMAND}"
			"-DOUTPUT=${PRECOMPILED_SHADER_HEADER}"
			"-DBYTECODE_DIR=${_bytecodeDir}"
			"-DSHADERS=${COMPUTE_SHADERS}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaderBytecode.cmake"
		DEPENDS
			${_bytecodeFiles}
			"${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaderBytecode.cmake"
		COMMENT "Embedding shader bytecode"
		VERBATIM
	)

	add_custom_target(GeneratePrecompiledShaders DEPENDS "${PRECOMPILED_SHADER_HEADER}")
	set(PRECOMPILED_SHADER_SOURCES "${PRECOMPILED_SHADER_HEADER}")
elseif (REQUIRE_PRECOMPILED_SHADERS)
	message(
		FATAL_ERROR
		"fxc.exe (Windows SDK) was not found, so the shaders cannot be precompiled. "
		"Install the Windows 10/11 SDK, or pass -DREQUIRE_PRECOMPILED_SHADERS=OFF to build a DLL that "
		"compiles its shaders at runtime (it will not work under Wine/Proton)."
	)
else ()
	message(WARNING "fxc.exe not found; the DLL will compile its shaders at runtime with d3dcompiler_47.dll")
endif ()

# ---- Create DLL ----

add_library(
	${PROJECT_NAME}
	SHARED
	${SOURCES}
	${SHADERS_ABS}
	"${EMBEDDED_SHADER_HEADER}"
	${PRECOMPILED_SHADER_SOURCES}
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	.clang-format
)

# HLSL files are compiled at runtime by d3dcompiler_47.dll; keep Visual Studio
# from trying to run FXC on them.
set_source_files_properties(${SHADERS_ABS} PROPERTIES HEADER_FILE_ONLY TRUE)

add_dependencies(${PROJECT_NAME} GenerateEmbeddedShaders)
if (TARGET GeneratePrecompiledShaders)
	add_dependencies(${PROJECT_NAME} GeneratePrecompiledShaders)
	target_compile_definitions(${PROJECT_NAME} PRIVATE GWP_PRECOMPILED_SHADERS)
endif ()

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		_UNICODE
)

target_compile_features(
	${PROJECT_NAME}
	PRIVATE
		cxx_std_20
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		CommonLibF4::CommonLibF4
		fmt::fmt
		spdlog::spdlog
		d3d11.lib
		dxgi.lib
		dxguid.lib
)

target_precompile_headers(
	${PROJECT_NAME}
	PRIVATE
		src/PCH.h
)

if (MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
			/sdl	# Enable Additional Security Checks
			/utf-8	# Set Source and Executable character sets to UTF-8
			/Zi	# Debug Information Format

			/permissive-	# Standards conformance
			/Zc:preprocessor	# Enable preprocessor conformance mode

			/W4	# Warning level
			/WX	# Warning level (warnings are errors)

			"$<$<CONFIG:DEBUG>:>"
			"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC->"
	)

	target_link_options(
		${PROJECT_NAME}
		PRIVATE
			/WX	# Treat Linker Warnings as Errors

			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif ()

# ---- Tests ----

if (BUILD_GPU_TESTS)
	add_subdirectory(tests)
endif ()

# ---- File copying ----

if (COPY_BUILD)
	if (DEFINED Fallout4Path)
		add_custom_command(
			TARGET ${PROJECT_NAME}
			POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${Fallout4Path}/Data/F4SE/Plugins/
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> ${Fallout4Path}/Data/F4SE/Plugins/
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/${PROJECT_NAME}.ini ${Fallout4Path}/Data/F4SE/Plugins/
		)
	else ()
		message(
			WARNING
			"Variable Fallout4Path is not defined. Skipping post-build copy command."
		)
	endif ()
endif ()
