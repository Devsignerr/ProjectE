#pragma once

// Scene/Sprite 내부 전용 JSON 도우미 (.cpp에서만 포함 — nlohmann 헤더를 공개 헤더로 퍼뜨리지 않는다)

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

namespace Sprite2DJson
{
	using FJson = nlohmann::json;

	inline void Warn(std::vector<std::string>* Warnings, std::string Message)
	{
		if (Warnings != nullptr)
		{
			Warnings->push_back(std::move(Message));
		}
	}

	// 루트 파싱 + 버전 확인 (없으면 1로 간주, 더 높으면 경고 후 읽을 수 있는 만큼)
	inline bool ParseRoot(std::string_view Text, uint32 CurrentVersion, const char* Kind, FJson& OutRoot, std::vector<std::string>* Warnings,
	                      std::string* OutError)
	{
		OutRoot = FJson::parse(Text, nullptr, false);
		if (OutRoot.is_discarded() || !OutRoot.is_object())
		{
			if (OutError != nullptr)
			{
				*OutError = std::format("{} JSON 형식 오류", Kind);
			}
			return false;
		}
		const auto VersionIt = OutRoot.find("Version");
		if (VersionIt != OutRoot.end() && VersionIt->is_number_integer() && VersionIt->get<int64>() > static_cast<int64>(CurrentVersion))
		{
			Warn(Warnings, std::format("{} 버전 {}은 이 엔진({})보다 새 형식입니다 — 아는 필드만 읽습니다", Kind, VersionIt->get<int64>(), CurrentVersion));
		}
		return true;
	}

	inline int32 GetInt(const FJson& Object, const char* Key, int32 Default)
	{
		const auto It = Object.find(Key);
		if (It == Object.end() || !It->is_number())
		{
			return Default;
		}
		return It->is_number_integer() ? static_cast<int32>(It->get<int64>()) : static_cast<int32>(It->get<double>());
	}

	inline float GetFloat(const FJson& Object, const char* Key, float Default)
	{
		const auto It = Object.find(Key);
		return (It == Object.end() || !It->is_number()) ? Default : It->get<float>();
	}

	inline bool GetBool(const FJson& Object, const char* Key, bool Default)
	{
		const auto It = Object.find(Key);
		return (It == Object.end() || !It->is_boolean()) ? Default : It->get<bool>();
	}

	inline std::string GetString(const FJson& Object, const char* Key)
	{
		const auto It = Object.find(Key);
		return (It == Object.end() || !It->is_string()) ? std::string() : It->get<std::string>();
	}

	inline const FJson* GetArray(const FJson& Object, const char* Key)
	{
		const auto It = Object.find(Key);
		return (It == Object.end() || !It->is_array()) ? nullptr : &*It;
	}

	// [x, y] → FVector2 (아니면 Default)
	inline FVector2 GetVector2(const FJson& Object, const char* Key, const FVector2& Default)
	{
		const FJson* Array = GetArray(Object, Key);
		if (Array == nullptr || Array->size() != 2 || !(*Array)[0].is_number() || !(*Array)[1].is_number())
		{
			return Default;
		}
		return FVector2((*Array)[0].get<float>(), (*Array)[1].get<float>());
	}
} // namespace Sprite2DJson
