set(SOURCES
	src/Core/Calibration.cpp
	src/Core/Calibration.h
	src/Core/Guard.cpp
	src/Core/Guard.h
	src/Core/ImmediatePasses.cpp
	src/Core/ImmediatePasses.h
	src/Core/Pipeline.cpp
	src/Core/Pipeline.h
	src/Engine/EngineHooks.cpp
	src/Engine/EngineHooks.h
	src/Engine/Layouts.h
	src/PCH.h
	src/Plugin.cpp
	src/Plugin.h
	src/Render/BatchRenderer.cpp
	src/Render/BatchRenderer.h
	src/Render/D3DHooks.cpp
	src/Render/D3DHooks.h
	src/Render/GpuBuffers.cpp
	src/Render/GpuBuffers.h
	src/Render/HiZ.cpp
	src/Render/HiZ.h
	src/Render/InputLayouts.cpp
	src/Render/InputLayouts.h
	src/Render/NvApi.cpp
	src/Render/NvApi.h
	src/Render/ShaderConstants.h
	src/Render/ShaderLibrary.cpp
	src/Render/ShaderLibrary.h
	src/Render/StateGuards.h
	src/Scene/BucketManager.cpp
	src/Scene/BucketManager.h
	src/Scene/MergeMath.cpp
	src/Scene/MergeMath.h
	src/Scene/ObjectRegistry.cpp
	src/Scene/ObjectRegistry.h
	src/Scene/VertexFormat.cpp
	src/Scene/VertexFormat.h
	src/Scene/ViewRegistry.cpp
	src/Scene/ViewRegistry.h
	src/Settings.cpp
	src/Settings.h
	src/Util/Counters.h
	src/Util/Math.h
	src/Util/SpinLock.h
	src/main.cpp
)

# HLSL sources, embedded into the DLL at build time (see cmake/GenerateShaderHeader.cmake)
set(SHADERS
	Common.hlsli
	Cull.hlsl
	HiZ.hlsl
	MergeIndices.hlsl
	MergeVertices.hlsl
)

# Compute shader entry points (file:entry), compiled with fxc at build time
# and embedded as bytecode (see cmake/EmbedShaderBytecode.cmake)
set(COMPUTE_SHADERS
	Cull.hlsl:CSCompact
	Cull.hlsl:CSMultiDraw
	HiZ.hlsl:CSInit
	HiZ.hlsl:CSReduce
	MergeIndices.hlsl:CSMain
	MergeVertices.hlsl:CSMain
)

set(SHADERS_ABS)
foreach (_shader IN LISTS SHADERS)
	list(APPEND SHADERS_ABS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}")
endforeach ()
