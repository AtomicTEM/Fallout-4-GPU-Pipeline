# GPU functional tests: the plugin's compute shaders and GPU modules running
# on a real Direct3D 11 device, without Fallout 4 or F4SE.

add_executable(
	GpuTests
	GpuTests.cpp
	TestPCH.h
	../src/Render/GpuBuffers.cpp
	../src/Render/HiZ.cpp
	../src/Render/ShaderLibrary.cpp
	../src/Scene/MergeMath.cpp
	../src/Scene/VertexFormat.cpp
)

add_dependencies(GpuTests GenerateEmbeddedShaders)
if (TARGET GeneratePrecompiledShaders)
	add_dependencies(GpuTests GeneratePrecompiledShaders)
	target_compile_definitions(GpuTests PRIVATE GWP_PRECOMPILED_SHADERS)
endif ()

target_compile_features(GpuTests PRIVATE cxx_std_20)

target_include_directories(
	GpuTests
	PRIVATE
		${CMAKE_BINARY_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/src
)

target_precompile_headers(GpuTests PRIVATE TestPCH.h)

target_link_libraries(
	GpuTests
	PRIVATE
		fmt::fmt
		d3d11.lib
		dxgi.lib
		dxguid.lib
)

if (MSVC)
	target_compile_options(GpuTests PRIVATE /utf-8 /permissive- /W4 /wd4201)
endif ()
