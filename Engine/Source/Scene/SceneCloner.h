#pragma once

#include "Core/ECS/Entity.h"

#include <unordered_map>

class FScene;

// 씬 → 씬 메모리 복제 (에디터 플레이 모드 스냅샷).
// JSON 왕복과 달리 런타임 리소스 핸들과 생성된(Transient) 엔티티까지 그대로 복사하므로 GPU 리소스를 다시 만들지 않는다.
//   - 리플렉션에 컴포넌트로 등록된 타입만 복사된다 (CopyComponent 훅). 미등록 컴포넌트는 복제본에 없다
//   - 엔티티 참조(계층 Parent/Children, Entity 타입 프로퍼티)는 복제본 엔티티로 다시 매핑한다
//   - FHierarchyComponent를 가진 엔티티(FScene::CreateEntity로 만든 엔티티)가 대상이며, 풀 순서를 유지한다
struct FSceneCloner
{
	using FEntityMap = std::unordered_map<uint64, FEntity>; // 원본 FEntity::ToId() → 복제본 엔티티

	// Dest의 기존 내용은 비운다. OutEntityMap이 있으면 원본 → 복제본 매핑을 채운다
	static void Clone(const FScene& Source, FScene& Dest, FEntityMap* OutEntityMap = nullptr);
};
