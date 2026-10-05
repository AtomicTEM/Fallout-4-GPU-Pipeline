#pragma once

// Precompiled header for tests/GpuTests.cpp. It stands in for src/PCH.h so
// the plugin's GPU modules can be compiled without F4SE/CommonLibF4RD: the
// handful of engine types they touch are declared here with the same member
// names as CommonLibF4RD.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

using namespace std::literals;

namespace logger
{
	template <class... Args>
	void Write(std::string_view a_level, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		std::fputs(fmt::format("[{}] {}\n", a_level, fmt::format(a_format, std::forward<Args>(a_args)...)).c_str(), stdout);
	}

	template <class... Args>
	void debug(fmt::format_string<Args...> a_format, Args&&... a_args) { Write("debug", a_format, std::forward<Args>(a_args)...); }
	template <class... Args>
	void info(fmt::format_string<Args...> a_format, Args&&... a_args) { Write("info", a_format, std::forward<Args>(a_args)...); }
	template <class... Args>
	void warn(fmt::format_string<Args...> a_format, Args&&... a_args) { Write("warn", a_format, std::forward<Args>(a_args)...); }
	template <class... Args>
	void error(fmt::format_string<Args...> a_format, Args&&... a_args) { Write("error", a_format, std::forward<Args>(a_args)...); }
	template <class... Args>
	void critical(fmt::format_string<Args...> a_format, Args&&... a_args) { Write("critical", a_format, std::forward<Args>(a_args)...); }
}

namespace RE
{
	class NiPoint3
	{
	public:
		float& operator[](std::size_t a_index) noexcept { return (&x)[a_index]; }
		const float& operator[](std::size_t a_index) const noexcept { return (&x)[a_index]; }

		float x{ 0.0F };
		float y{ 0.0F };
		float z{ 0.0F };
	};

	class NiPoint4
	{
	public:
		float pt[4]{};
	};

	class alignas(0x10) NiMatrix3
	{
	public:
		NiPoint4 entry[3];
	};

	class NiTransform
	{
	public:
		NiMatrix3 rotate;
		NiPoint3 translate;
		float scale{ 1.0F };
	};

	class NiBound
	{
	public:
		NiPoint3 center;
		float fRadius{ 0.0F };
	};

	class NiAVObject
	{
	public:
		[[nodiscard]] bool ShadowCaster() const noexcept { return true; }

		NiAVObject* parent{ nullptr };
		NiTransform world;
		NiBound worldBound;
		float fadeAmount{ 1.0F };
	};

	class BSGeometry;
	class BSShaderProperty;
	class NiCamera;
	class NiProperty;
}
