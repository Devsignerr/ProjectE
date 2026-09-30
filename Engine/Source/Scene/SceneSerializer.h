#pragma once

#include <filesystem>
#include <string>

class FScene;

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
};
