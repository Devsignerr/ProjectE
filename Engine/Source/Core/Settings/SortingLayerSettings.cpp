#include "Core/Settings/SortingLayerSettings.h"

#include "Core/Log.h"

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

FSortingLayerSettings::FSortingLayerSettings()
{
	Names.emplace_back(DefaultLayerName);
}

int32 FSortingLayerSettings::FindLayer(std::string_view Name) const
{
	if (Name.empty())
	{
		return -1;
	}
	for (size_t Index = 0; Index < Names.size(); ++Index)
	{
		if (Names[Index] == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

uint32 FSortingLayerSettings::ResolveLayer(std::string_view Name) const
{
	const int32 Found = FindLayer(Name);
	return Found < 0 ? 0u : static_cast<uint32>(Found);
}

uint32 FSortingLayerSettings::ResolveLayerChecked(std::string_view Name) const
{
	const int32 Found = FindLayer(Name);
	if (Found >= 0)
	{
		return static_cast<uint32>(Found);
	}
	if (!Name.empty() && WarnedNames.insert(std::string(Name)).second)
	{
		E_LOG(LogCore, Warning, "정렬 레이어 '{}'가 프로젝트 설정에 없습니다 — Default로 그립니다", Name);
	}
	return 0;
}

bool FSortingLayerSettings::IsValidNewName(std::string_view Name) const
{
	return !Name.empty() && FindLayer(Name) < 0;
}

bool FSortingLayerSettings::AddLayer(std::string Name)
{
	if (Names.size() >= MaxLayers || !IsValidNewName(Name))
	{
		return false;
	}
	Names.push_back(std::move(Name));
	return true;
}

bool FSortingLayerSettings::RenameLayer(uint32 Index, std::string Name)
{
	if (Index == 0 || Index >= Names.size() || !IsValidNewName(Name))
	{
		return false;
	}
	Names[Index] = std::move(Name);
	return true;
}

bool FSortingLayerSettings::RemoveLayer(uint32 Index)
{
	if (Index == 0 || Index >= Names.size())
	{
		return false;
	}
	Names.erase(Names.begin() + Index);
	return true;
}

bool FSortingLayerSettings::MoveLayer(uint32 From, uint32 To)
{
	if (From == 0 || To == 0 || From >= Names.size() || To >= Names.size() || From == To)
	{
		return false;
	}
	std::string Name = std::move(Names[From]);
	Names.erase(Names.begin() + From);
	Names.insert(Names.begin() + To, std::move(Name));
	return true;
}

std::string FSortingLayerSettings::ToJson() const
{
	nlohmann::ordered_json Root;
	Root["Layers"] = Names;
	return Root.dump(2);
}

bool FSortingLayerSettings::FromJson(std::string_view Json, std::string* Error)
{
	const nlohmann::json Root = nlohmann::json::parse(Json, nullptr, false);
	if (Root.is_discarded() || !Root.is_object())
	{
		AppendError(Error, "JSON 형식 오류");
		return false;
	}
	FSortingLayerSettings Loaded;
	if (const auto Layers = Root.find("Layers"); Layers != Root.end() && Layers->is_array())
	{
		bool bFirst = true;
		for (const nlohmann::json& Item : *Layers)
		{
			std::string Name = Item.is_string() ? Item.get<std::string>() : std::string();
			if (bFirst)
			{
				bFirst = false;
				if (Name != DefaultLayerName)
				{
					AppendError(Error, std::format("첫 정렬 레이어는 항상 \"{}\"입니다", DefaultLayerName));
					if (Name.empty() || Name == DefaultLayerName)
					{
						continue;
					}
					// 첫 칸에 다른 이름이 있으면 Default 뒤로 옮겨 살린다
				}
				else
				{
					continue;
				}
			}
			if (Loaded.Names.size() >= MaxLayers)
			{
				AppendError(Error, std::format("정렬 레이어는 최대 {}개입니다 (나머지 무시)", MaxLayers));
				break;
			}
			if (!Loaded.IsValidNewName(Name))
			{
				AppendError(Error, std::format("빈 이름 또는 중복 정렬 레이어 '{}' (무시)", Name));
				continue;
			}
			Loaded.Names.push_back(std::move(Name));
		}
	}
	*this = std::move(Loaded);
	return true;
}
