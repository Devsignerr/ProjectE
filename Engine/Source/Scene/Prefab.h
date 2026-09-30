#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class FScene;

// ---------------------------------------------------------------- 컴포넌트

// 프리팹 인스턴스 루트 표식 (씬에 놓인 인스턴스, 또는 프리팹 파일 안의 중첩 인스턴스 루트).
//   Asset:     Content 기준 .eprefab 경로
//   Overrides: 인스턴스에서 바꾼 항목 목록(FPrefabOverrides JSON). 값은 엔티티 컴포넌트에 그대로 있고 여기에는 "무엇을 바꿨는지"만 둔다
struct FPrefabInstanceComponent
{
	std::string Asset;
	std::string Overrides;
};

// 프리팹에서 온 엔티티의 연결.
//   Id:   프리팹 파일 안에서 고정된 엔티티 ID (이름이 바뀌어도 유지). 중첩 프리팹 안 엔티티는 "바깥 ID/안쪽 ID" ("3/2")
//   Root: 이 엔티티를 소유한 가장 바깥 인스턴스 루트 (씬). 프리팹 파일 안에서는 NullEntity
// 연결이 없는 자식은 인스턴스에서 추가한 엔티티로 본다.
struct FPrefabLinkComponent
{
	std::string Id;
	FEntity     Root;
};

// ---------------------------------------------------------------- 오버라이드

// 인스턴스가 원본과 다르게 가진 항목 (키만 기록). 원본 반영 시 여기 없는 값만 원본 값으로 덮어쓴다.
//   Properties:        "TransformComponent.Position" 처럼 "컴포넌트.프로퍼티" (이름은 "NameComponent.Name")
//   Added/RemovedComponents: 인스턴스에서 추가/제거한 컴포넌트 이름
//   RemovedEntities:   인스턴스에서 삭제한 원본 엔티티 Id (하위 트리 전체)
// 루트의 위치/회전/이름은 항상 인스턴스 값이다 (기록하지 않고 반영하지도 않는다).
struct FPrefabOverrides
{
	struct FEntityOverrides
	{
		std::set<std::string> Properties;
		std::set<std::string> AddedComponents;
		std::set<std::string> RemovedComponents;

		bool IsEmpty() const { return Properties.empty() && AddedComponents.empty() && RemovedComponents.empty(); }
		bool operator==(const FEntityOverrides&) const = default;
	};

	std::map<std::string, FEntityOverrides> Entities; // 키: 링크 Id
	std::set<std::string>                   RemovedEntities;

	static FPrefabOverrides Parse(std::string_view Json); // 빈 문자열/형식 오류 → 빈 목록
	std::string             Serialize() const;             // 비었으면 빈 문자열 (키 정렬, 한 줄)

	bool   IsEmpty() const;
	bool   IsPropertyOverridden(const std::string& Id, std::string_view Key) const;
	size_t Count() const; // 항목 수 (표시용)
	bool   operator==(const FPrefabOverrides&) const = default;
};

// ---------------------------------------------------------------- 원본 (로드된 프리팹)

// 프리팹 파일을 읽어 중첩 인스턴스까지 각 원본의 현재 내용으로 맞춘 상태 (FPrefabLibrary가 소유, 읽기 전용으로 쓴다)
struct FPrefabTemplate
{
	struct FNode
	{
		std::string Id;
		FEntity     Entity;
		int32       Parent = -1; // Nodes 인덱스 (루트 -1)
	};

	FPrefabTemplate();
	~FPrefabTemplate();

	std::unique_ptr<FScene>                  Scene;
	std::vector<FNode>                       Nodes; // [0] = 루트, 부모가 자식보다 앞
	std::unordered_map<std::string, size_t>  IndexById;
	std::unordered_map<uint64, size_t>       IndexByEntity; // FEntity::ToId()
	uint32                                   NextId = 1;    // 새 엔티티에 줄 다음 ID
	std::string                              Asset;         // Content 기준 경로

	FEntity            GetRoot() const;
	const std::string& GetRootId() const;
	const FNode*       FindById(const std::string& Id) const;
	const FNode*       FindByEntity(FEntity Entity) const;
	void               RebuildIndex(FEntity Root); // Scene의 Root 서브트리와 링크로 Nodes 재구성
};

// ---------------------------------------------------------------- 라이브러리

// 프리팹(.eprefab) 로드 캐시 + 인스턴스 연산. 순수 로직 (GPU/ImGui 비의존) — 에디터, 런타임, Lua, 테스트가 함께 쓴다.
//
// 파일 형식 (JSON): { "Version": 1, "NextId": N, "Entities": [ 씬과 같은 엔티티 항목... ] }
//   Entities[0]이 루트. 모든 엔티티에 PrefabLinkComponent { Id, Root = -1 }. 중첩 인스턴스는 루트에 PrefabInstanceComponent를 두고
//   원본 내용을 펼쳐 저장한다 (ID "바깥/안쪽"). 로드 시 중첩 인스턴스는 원본의 현재 내용 + 저장된 오버라이드로 다시 맞춘다.
//
// 오버라이드 규칙 (중요): 오버라이드는 인스턴스와 원본의 차이를 RecordOverrides가 기록한 것이다. 이 차이 계산은
// "인스턴스가 현재 원본에 맞춰져 있을 때"만 옳으므로, 원본이 바뀌면 (1) 기존 원본으로 RecordAllOverrides → (2) Invalidate →
// (3) SyncAllInstances 순서로 열린 씬을 다시 맞춰야 한다. 씬 로드(FSceneSerializer::FromJsonString)는 (3)을 자동으로 한다.
class FPrefabLibrary
{
public:
	static constexpr const wchar_t* Extension = L".eprefab";
	static constexpr int32          Version   = 1;

	// 엔진 전역 인스턴스 (씬 로드 동기화, Lua 생성이 사용). Content 기본값은 프로젝트 Content 디렉터리
	static FPrefabLibrary& Get();

	void                         SetContentDirectory(const std::filesystem::path& Directory);
	const std::filesystem::path& GetContentDirectory() const;
	std::filesystem::path        ResolveAssetPath(const std::string& Asset) const;   // 상대 → Content 기준 절대
	std::string                  MakeAssetPath(const std::filesystem::path& Path) const; // Content 안이면 '/' 상대 경로

	// ---- 원본
	// 캐시된 원본 (없으면 로드). 실패(파일 없음/형식 오류)면 nullptr + OutError
	const FPrefabTemplate* Load(const std::string& Asset, std::string* OutError = nullptr);
	void                   Invalidate(); // 캐시 전체 비우기 (원본 파일이 바뀐 뒤)
	// Asset이 (중첩을 따라가며) Target을 포함하는가. 같은 파일이면 true (순환 검사)
	bool DependsOn(const std::string& Asset, const std::string& Target);

	// ---- 씬 인스턴스
	// Parent 아래(없으면 루트)에 인스턴스 생성. 반환: 인스턴스 루트 (실패 시 NullEntity)
	FEntity Instantiate(FScene& Scene, const std::string& Asset, FEntity Parent, std::string* OutError = nullptr);
	// 원본의 현재 내용으로 맞춤 (오버라이드는 유지). 반환: 씬이 바뀌었는가
	bool SyncInstance(FScene& Scene, FEntity InstanceRoot);
	bool SyncAllInstances(FScene& Scene);
	// 원본과 달라진 항목을 오버라이드로 기록 (에디터 편집 커밋 직전). 반환: 오버라이드가 바뀌었는가
	bool RecordOverrides(FScene& Scene, FEntity InstanceRoot);
	bool RecordAllOverrides(FScene& Scene);

	// 되돌리기: 프로퍼티 하나 / 컴포넌트 추가·제거 하나 / 전부 (추가한 자식 엔티티 삭제 포함)
	bool RevertProperty(FScene& Scene, FEntity Member, const std::string& Key);
	bool RevertComponent(FScene& Scene, FEntity Member, const std::string& ComponentName);
	bool RevertAll(FScene& Scene, FEntity InstanceRoot);
	// 인스턴스의 현재 상태를 원본 파일에 쓰고 오버라이드를 비운다. 캐시를 비우고 Scene의 같은 원본 인스턴스를 모두 다시 맞춘다
	bool ApplyToPrefab(FScene& Scene, FEntity InstanceRoot, std::string* OutError = nullptr);
	// 연결을 끊고 일반 엔티티로 (중첩 인스턴스는 그대로 인스턴스로 남는다)
	void Unpack(FScene& Scene, FEntity InstanceRoot);

	// Entity 서브트리로 새 프리팹 파일을 만들고, 원래 엔티티를 그 인스턴스로 바꾼다. 인스턴스 일부/루트면 거부
	bool CreatePrefab(FScene& Scene, FEntity Entity, const std::filesystem::path& File, std::string* OutError = nullptr);

	// ---- 조회
	// 엔티티가 속한 가장 바깥 인스턴스 루트 (인스턴스 소속이 아니면 NullEntity)
	static FEntity FindInstanceRoot(const FScene& Scene, FEntity Entity);
	static bool    IsInstanceRoot(const FScene& Scene, FEntity Entity);

	// ---- 프리팹 편집 창 (에셋 공간: 링크 Root = NullEntity)
	// 원본을 Scene에 불러온다 (Parent 아래). 반환: 루트, OutNextId: 파일의 NextId
	FEntity LoadAssetInto(FScene& Scene, const std::string& Asset, FEntity Parent, uint32& OutNextId, std::string* OutError = nullptr);
	// 편집 상태 문자열 (파일과 같은 형식, 새 엔티티 ID는 아직 없음) ↔ 씬
	std::string AssetStateToJson(FScene& Scene, FEntity Root, uint32 NextId) const;
	FEntity     AssetStateFromJson(FScene& Scene, const std::string& Json, FEntity Parent, uint32& OutNextId) const;
	// Root 서브트리를 File에 저장: 새 엔티티에 ID 부여, 중첩 인스턴스 오버라이드 갱신. 원본을 스스로 포함하면 거부
	bool SaveAsset(FScene& Scene, FEntity Root, const std::filesystem::path& File, uint32& InOutNextId, std::string* OutError = nullptr);

private:
	const FPrefabTemplate* LoadInternal(const std::string& Asset, std::vector<std::wstring>& Stack, std::string* OutError);
	bool                   BuildAssetDocument(FScene& Scene, FEntity Root, bool bKeepSelfIds, FEntity SelfRoot, uint32& InOutNextId,
	                                          const std::string& SavingAsset, std::string& OutJson, std::vector<std::string>& OutIds,
	                                          std::string* OutError);
	std::wstring           MakeKey(const std::string& Asset) const;

	std::filesystem::path                                             ContentDirectory;
	bool                                                              bContentDirectorySet = false;
	std::unordered_map<std::wstring, std::unique_ptr<FPrefabTemplate>> Cache; // 키: 정규화 절대 경로 (소문자)
	std::unordered_map<std::wstring, std::string>                     Failed; // 실패한 경로 → 오류 (같은 오류 반복 로그 방지)
};
