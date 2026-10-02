#include "Scene/SceneSerializer.h"

#include "Core/Profiling.h"
#include "Core/FileSystem.h"
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
	E_PROFILE_SCOPE("씬 로드");
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

struct FSceneDocument
{
	nlohmann::json Entities; // 배열
	std::string    Label;    // 로그 표시용 (파일 이름)
};

std::shared_ptr<const FSceneDocument> FSceneSerializer::ParseFile(const std::filesystem::path& Path, std::string* OutError)
{
	const auto Fail = [OutError](std::string Reason) -> std::shared_ptr<const FSceneDocument> {
		if (OutError != nullptr)
		{
			*OutError = std::move(Reason);
		}
		return nullptr;
	};
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		return Fail("씬 파일을 열 수 없습니다: " + FStringConv::ToUtf8(Path.wstring()));
	}
	nlohmann::json Document = nlohmann::json::parse(Text, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Document.is_discarded() || !Document.is_object())
	{
		return Fail("씬 JSON 파싱 실패: " + FStringConv::ToUtf8(Path.wstring()));
	}
	const auto EntitiesIt = Document.find("Entities");
	if (EntitiesIt == Document.end() || !EntitiesIt->is_array())
	{
		return Fail("씬 JSON에 Entities 배열이 없습니다: " + FStringConv::ToUtf8(Path.wstring()));
	}
	auto Result      = std::make_shared<FSceneDocument>();
	Result->Entities = std::move(*EntitiesIt);
	Result->Label    = FStringConv::ToUtf8(Path.filename().wstring());
	return Result;
}

std::vector<FEntity> FSceneSerializer::AppendDocument(FScene& Scene, const FSceneDocument& Document, FEntity Parent)
{
	const std::vector<FEntity> Entities = FEntityJson::Read(Scene, Document.Entities, Parent, Document.Label);
	for (const FEntity Entity : Entities)
	{
		// 가장 바깥 인스턴스 루트만 (중첩 인스턴스는 바깥 루트 동기화가 함께 맞춘다)
		if (Scene.GetRegistry().IsValid(Entity) && FPrefabLibrary::IsInstanceRoot(Scene, Entity) && FPrefabLibrary::FindInstanceRoot(Scene, Entity) == Entity)
		{
			FPrefabLibrary::Get().SyncInstance(Scene, Entity);
		}
	}
	Scene.UpdateTransforms();
	return Entities;
}

bool FSceneSerializer::LoadFromFile(FScene& OutScene, const std::filesystem::path& Path)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		E_LOG(LogScene, Error, "씬 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	if (!FromJsonString(OutScene, Text))
	{
		E_LOG(LogScene, Error, "씬 로드 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}
