#pragma once

#include "Core/ECS/Entity.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class FScene;
struct FSceneDocument; // 파싱된 씬 파일 (SceneSerializer.cpp — JSON 타입을 헤더에 노출하지 않는다)

// 씬 ↔ JSON (.escene). 리플렉션에 등록된 컴포넌트/프로퍼티를 기준으로 동작한다.
//   - 이름/계층은 구조적으로 기록 (Parent = 파일 내 인덱스, 루트는 -1)
//   - PF_Transient 프로퍼티와 리소스 핸들은 제외, FTransientComponent가 있는 엔티티(와 그 하위)는 제외
//   - 로드 시 알 수 없는 컴포넌트/프로퍼티는 경고 후 무시 (버전 간 호환)
//   - 엔티티 목록 직렬화는 프리팹(.eprefab)과 공유한다 (Scene/EntityJson.h)
//   - 로드(FromJsonString) 끝에 프리팹 인스턴스를 원본의 현재 내용으로 맞춘다 (FPrefabLibrary::SyncAllInstances, 오버라이드 유지)
// 리소스 핸들 복원은 Renderer의 ResolveSceneAssets가 담당한다.
struct FSceneSerializer
{
	static constexpr int32_t      Version   = 1;
	static constexpr const wchar_t* Extension = L".escene";

	static std::string ToJsonString(FScene& Scene);
	static bool        FromJsonString(FScene& OutScene, const std::string& Json); // 기존 내용은 비운다

	static bool SaveToFile(FScene& Scene, const std::filesystem::path& Path);
	static bool LoadFromFile(FScene& OutScene, const std::filesystem::path& Path);

	// ---- 서브 씬 (기존 씬에 붙여 불러오기, 2단계)
	// 1) 파일 읽기 + JSON 파싱만 (ECS/리플렉션 접근 없음 → 백그라운드 스레드에서 불러도 된다). 실패하면 nullptr + OutError
	static std::shared_ptr<const FSceneDocument> ParseFile(const std::filesystem::path& Path, std::string* OutError = nullptr);
	// 2) 메인 스레드: Parent 아래에 엔티티를 만들고 그 안의 프리팹 인스턴스만 원본에 맞춘다 (씬의 다른 엔티티는 건드리지 않음).
	//    반환: 만든 엔티티 (파일 순서, 프리팹 동기화로 생긴 자식은 제외)
	static std::vector<FEntity> AppendDocument(FScene& Scene, const FSceneDocument& Document, FEntity Parent);
};
