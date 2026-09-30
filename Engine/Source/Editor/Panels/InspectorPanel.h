#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Prefab.h"

#include <string>
#include <vector>

struct FEditorContext;
struct FPropertyInfo;
struct FTypeInfo;

// 선택된 엔티티의 컴포넌트를 리플렉션 정보로 편집한다.
// 등록된 컴포넌트 타입(FTypeRegistry)을 순회하므로 새 컴포넌트는 등록만 하면 자동으로 표시된다.
// 프리팹 인스턴스면 머리글(원본/되돌리기/원본에 적용/연결 해제)과 오버라이드된 프로퍼티 표시(왼쪽 하늘색 막대, 우클릭 되돌리기)를 더한다.
class FInspectorPanel
{
public:
	void Draw(FEditorContext& Context);
	// 창 없이 내용만 (Context.Scene의 Context.SelectedEntity). 프리팹 편집 창이 자기 미리보기 씬으로 쓴다
	void DrawContents(FEditorContext& Context);

	bool bOpen = true;

private:
	void DrawNameField(FEditorContext& Context, FEntity Entity);
	void DrawComponent(FEditorContext& Context, FEntity Entity, const FTypeInfo& Type, void* Component);
	bool DrawProperty(const FPropertyInfo& Property, void* Component, FEntity Entity);
	// 에셋 경로 문자열 칸 (AssetFilter가 있는 프로퍼티): 콘텐츠 브라우저 드롭 받기. 반환: 바뀜
	bool DrawAssetSlot(FEditorContext& Context, const FTypeInfo& Type, const FPropertyInfo& Property, void* Component);
	void DrawTransformExtras(FEditorContext& Context, FEntity Entity);
	void DrawStaticMeshExtras(FEditorContext& Context, FEntity Entity);
	void DrawScriptExtras(FEditorContext& Context, FEntity Entity); // 스크립트 선택 + Properties 오버라이드
	void DrawAnimationExtras(FEditorContext& Context, FEntity Entity); // 클립 선택 드롭다운 + 재생 상태
	void DrawAddComponentMenu(FEditorContext& Context, FEntity Entity);

	// ---- 프리팹
	void UpdatePrefabView(FEditorContext& Context, FEntity Entity);
	void DrawPrefabHeader(FEditorContext& Context, FEntity Entity);
	// 직전 항목이 오버라이드된 프로퍼티(Key = "컴포넌트.프로퍼티")면 표시 + 우클릭 되돌리기
	void DrawOverrideMarker(const std::string& Key);
	void ApplyPendingPrefabAction(FEditorContext& Context, FEntity Entity);

	struct FPrefabView
	{
		bool             bMember  = false; // 선택 엔티티가 씬 인스턴스 소속
		bool             bPlaying = false;
		FEntity          Root;
		std::string      Id;
		std::string      Asset;
		FPrefabOverrides Overrides;
	};
	enum class EPrefabAction : uint8
	{
		None,
		RevertProperty,
		RevertComponent,
		RevertAll,
		Apply,
		Unpack,
	};
	FPrefabView   PrefabView;
	EPrefabAction PendingPrefabAction = EPrefabAction::None;
	std::string   PendingPrefabKey; // 되돌릴 프로퍼티 키 / 컴포넌트 이름

	// 쿼터니언은 오일러 각으로 편집. 변환 불안정을 피하기 위해 편집 중 값을 (엔티티, 프로퍼티)별로 캐시
	FEntity              EulerCacheEntity;
	const FPropertyInfo* EulerCacheProperty = nullptr;
	FQuat                EulerCacheRotation;
	FVector3             EulerCacheDegrees; // (Pitch, Yaw, Roll)

	std::vector<std::string> ScriptFiles; // 스크립트 선택 팝업 목록 (열 때 Content를 다시 스캔)
};
