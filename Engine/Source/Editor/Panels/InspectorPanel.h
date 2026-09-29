#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>
#include <vector>

struct FEditorContext;
struct FPropertyInfo;
struct FTypeInfo;

// 선택된 엔티티의 컴포넌트를 리플렉션 정보로 편집한다.
// 등록된 컴포넌트 타입(FTypeRegistry)을 순회하므로 새 컴포넌트는 등록만 하면 자동으로 표시된다.
class FInspectorPanel
{
public:
	void Draw(FEditorContext& Context);

	bool bOpen = true;

private:
	void DrawNameField(FEditorContext& Context, FEntity Entity);
	void DrawComponent(FEditorContext& Context, FEntity Entity, const FTypeInfo& Type, void* Component);
	bool DrawProperty(const FPropertyInfo& Property, void* Component, FEntity Entity);
	void DrawTransformExtras(FEditorContext& Context, FEntity Entity);
	void DrawStaticMeshExtras(FEditorContext& Context, FEntity Entity);
	void DrawScriptExtras(FEditorContext& Context, FEntity Entity); // 스크립트 선택 + Properties 오버라이드
	void DrawAddComponentMenu(FEditorContext& Context, FEntity Entity);

	// 쿼터니언은 오일러 각으로 편집. 변환 불안정을 피하기 위해 편집 중 값을 (엔티티, 프로퍼티)별로 캐시
	FEntity              EulerCacheEntity;
	const FPropertyInfo* EulerCacheProperty = nullptr;
	FQuat                EulerCacheRotation;
	FVector3             EulerCacheDegrees; // (Pitch, Yaw, Roll)

	std::vector<std::string> ScriptFiles; // 스크립트 선택 팝업 목록 (열 때 Content를 다시 스캔)
};
