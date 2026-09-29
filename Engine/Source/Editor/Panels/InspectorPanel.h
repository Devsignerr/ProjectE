#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

struct FEditorContext;

// 선택된 엔티티의 컴포넌트 편집
class FInspectorPanel
{
public:
	void Draw(FEditorContext& Context);

	bool bOpen = true;

private:
	void DrawNameComponent(FEditorContext& Context, FEntity Entity);
	void DrawTransformComponent(FEditorContext& Context, FEntity Entity);
	void DrawStaticMeshComponent(FEditorContext& Context, FEntity Entity);
	void DrawDirectionalLightComponent(FEditorContext& Context, FEntity Entity);
	void DrawAddComponentMenu(FEditorContext& Context, FEntity Entity);

	// 회전은 오일러 각으로 편집하되, 쿼터니언→오일러 변환의 불안정을 피하기 위해 편집 중 값을 캐시
	FEntity  EulerCacheEntity;
	FQuat    EulerCacheRotation;
	FVector3 EulerCacheDegrees; // (Pitch, Yaw, Roll)
};
