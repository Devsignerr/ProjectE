#include "Core/Settings/CollisionSettings.h"

#include <format>

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

namespace
{
	void AppendError(std::string* Error, const std::string& Message)
	{
		if (Error != nullptr)
		{
			if (!Error->empty())
			{
				*Error += "; ";
			}
			*Error += Message;
		}
	}
} // namespace

FCollisionLayerSettings::FCollisionLayerSettings()
{
	Names[0] = DefaultLayerName;
	Matrix.fill(static_cast<uint16>(AllLayersMask));
}

bool FCollisionLayerSettings::SetLayerName(uint32 Index, std::string Name)
{
	if (Index == 0 || Index >= MaxLayers || Names[Index] == Name)
	{
		return false;
	}
	Names[Index] = std::move(Name);
	// 다른 레이어가 된 칸: 줄/열을 모두 켬으로 (이전 이름의 설정을 물려받지 않게)
	for (uint32 Other = 0; Other < MaxLayers; ++Other)
	{
		SetCollision(Index, Other, true);
	}
	return true;
}

int32 FCollisionLayerSettings::FindLayer(std::string_view Name) const
{
	if (Name.empty())
	{
		return -1;
	}
	for (uint32 Index = 0; Index < MaxLayers; ++Index)
	{
		if (Names[Index] == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

uint32 FCollisionLayerSettings::ResolveLayer(std::string_view Name) const
{
	const int32 Found = FindLayer(Name);
	return Found < 0 ? 0u : static_cast<uint32>(Found);
}

std::vector<std::string> FCollisionLayerSettings::GetLayerNames() const
{
	std::vector<std::string> Result;
	for (const std::string& Name : Names)
	{
		if (!Name.empty())
		{
			Result.push_back(Name);
		}
	}
	return Result;
}

bool FCollisionLayerSettings::ShouldCollide(uint32 A, uint32 B) const
{
	if (A >= MaxLayers || B >= MaxLayers)
	{
		return true;
	}
	return ((Matrix[A] >> B) & 1u) != 0;
}

void FCollisionLayerSettings::SetCollision(uint32 A, uint32 B, bool bCollide)
{
	if (A >= MaxLayers || B >= MaxLayers)
	{
		return;
	}
	if (bCollide)
	{
		Matrix[A] = static_cast<uint16>(Matrix[A] | (1u << B));
		Matrix[B] = static_cast<uint16>(Matrix[B] | (1u << A));
	}
	else
	{
		Matrix[A] = static_cast<uint16>(Matrix[A] & ~(1u << B));
		Matrix[B] = static_cast<uint16>(Matrix[B] & ~(1u << A));
	}
}

bool FCollisionLayerSettings::MakeMask(const std::vector<std::string>& LayerNames, uint32& OutMask, std::string* OutUnknown) const
{
	OutMask = 0;
	for (const std::string& Name : LayerNames)
	{
		const int32 Found = FindLayer(Name);
		if (Found < 0)
		{
			if (OutUnknown != nullptr)
			{
				*OutUnknown = Name;
			}
			return false;
		}
		OutMask |= 1u << static_cast<uint32>(Found);
	}
	return true;
}

std::string FCollisionLayerSettings::ToJson() const
{
	nlohmann::ordered_json Root;
	// 칸 순서 그대로 (뒤쪽 빈 칸은 생략)
	uint32 Count = 1;
	for (uint32 Index = 0; Index < MaxLayers; ++Index)
	{
		if (!Names[Index].empty())
		{
			Count = Index + 1;
		}
	}
	nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		Layers.push_back(Names[Index]);
	}
	Root["Layers"] = std::move(Layers);

	nlohmann::ordered_json Disabled = nlohmann::ordered_json::array();
	for (uint32 A = 0; A < MaxLayers; ++A)
	{
		for (uint32 B = A; B < MaxLayers; ++B)
		{
			if (!Names[A].empty() && !Names[B].empty() && !ShouldCollide(A, B))
			{
				Disabled.push_back(nlohmann::ordered_json::array({ Names[A], Names[B] }));
			}
		}
	}
	Root["DisabledPairs"] = std::move(Disabled);
	return Root.dump(2);
}

bool FCollisionLayerSettings::FromJson(std::string_view Json, std::string* Error)
{
	const nlohmann::json Root = nlohmann::json::parse(Json, nullptr, false);
	if (Root.is_discarded() || !Root.is_object())
	{
		AppendError(Error, "JSON 형식 오류");
		return false;
	}

	FCollisionLayerSettings Loaded;
	if (const auto Layers = Root.find("Layers"); Layers != Root.end() && Layers->is_array())
	{
		uint32 Slot = 0;
		for (const nlohmann::json& Item : *Layers)
		{
			if (Slot >= MaxLayers)
			{
				AppendError(Error, std::format("레이어는 최대 {}개입니다 (나머지 무시)", MaxLayers));
				break;
			}
			std::string Name = Item.is_string() ? Item.get<std::string>() : std::string();
			if (Slot == 0)
			{
				if (Name != DefaultLayerName)
				{
					AppendError(Error, std::format("첫 레이어는 항상 \"{}\"입니다", DefaultLayerName));
				}
				++Slot;
				continue;
			}
			if (!Name.empty() && Loaded.FindLayer(Name) >= 0)
			{
				AppendError(Error, std::format("레이어 이름 중복: '{}' (뒤의 것은 비움)", Name));
				Name.clear();
			}
			Loaded.Names[Slot++] = std::move(Name);
		}
	}
	if (const auto Pairs = Root.find("DisabledPairs"); Pairs != Root.end() && Pairs->is_array())
	{
		for (const nlohmann::json& Pair : *Pairs)
		{
			if (!Pair.is_array() || Pair.size() != 2 || !Pair[0].is_string() || !Pair[1].is_string())
			{
				AppendError(Error, "DisabledPairs 항목은 [이름, 이름]이어야 합니다");
				continue;
			}
			const int32 A = Loaded.FindLayer(Pair[0].get<std::string>());
			const int32 B = Loaded.FindLayer(Pair[1].get<std::string>());
			if (A < 0 || B < 0)
			{
				AppendError(Error, std::format("없는 레이어의 쌍: [{}, {}]", Pair[0].get<std::string>(), Pair[1].get<std::string>()));
				continue;
			}
			Loaded.SetCollision(static_cast<uint32>(A), static_cast<uint32>(B), false);
		}
	}
	*this = std::move(Loaded);
	return true;
}
