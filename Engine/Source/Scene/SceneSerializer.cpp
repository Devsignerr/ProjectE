#include "Scene/SceneSerializer.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scene/EntityJson.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogScene)

std::string FSceneSerializer::ToJsonString(FScene& Scene)
{
	std::vector<FEntity> Entities;
	for (FEntity Root : Scene.GetRootEntities())
	{
		FEntityJson::CollectSubtree(Scene, Root, Entities);
	}

	nlohmann::json Document;
	Document["Version"]  = Version;
	Document["Entities"] = FEntityJson::Write(Scene, Entities);
	return Document.dump(2);
}

bool FSceneSerializer::FromJsonString(FScene& OutScene, const std::string& JsonText)
{
	const nlohmann::json Document = nlohmann::json::parse(JsonText, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogScene, Error, "씬 JSON 파싱 실패");
		return false;
	}

	const int32 FileVersion = Document.value("Version", 0);
	if (FileVersion > Version)
	{
		E_LOG(LogScene, Warning, "씬 파일 버전 {}이(가) 지원 버전 {}보다 높습니다. 일부 데이터가 무시될 수 있습니다", FileVersion, Version);
	}

	const auto EntitiesIt = Document.find("Entities");
	if (EntitiesIt == Document.end() || !EntitiesIt->is_array())
	{
		E_LOG(LogScene, Error, "씬 JSON에 Entities 배열이 없습니다");
		return false;
	}

	OutScene.Clear();
	const std::vector<FEntity> Entities = FEntityJson::Read(OutScene, *EntitiesIt, NullEntity, "씬");

	// 프리팹 인스턴스: 저장된 오버라이드는 유지한 채 원본의 현재 내용으로 맞춘다 (씬을 닫은 동안 원본이 바뀌었을 수 있음)
	FPrefabLibrary::Get().SyncAllInstances(OutScene);

	OutScene.UpdateTransforms();
	E_LOG(LogScene, Display, "씬 로드: 엔티티 {}개", Entities.size());
	return true;
}

bool FSceneSerializer::SaveToFile(FScene& Scene, const std::filesystem::path& Path)
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);

	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogScene, Error, "씬 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString(Scene);
	E_LOG(LogScene, Display, "씬 저장: {}", FStringConv::ToUtf8(Path.wstring()));
	return true;
}

bool FSceneSerializer::LoadFromFile(FScene& OutScene, const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		E_LOG(LogScene, Error, "씬 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	if (!FromJsonString(OutScene, Buffer.str()))
	{
		E_LOG(LogScene, Error, "씬 로드 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}
