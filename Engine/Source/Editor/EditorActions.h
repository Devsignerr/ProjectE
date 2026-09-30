#pragma once

#include "Core/ECS/Entity.h"

#include <filesystem>
#include <vector>

struct FEditorContext;

// 여러 패널/단축키가 공유하는 선택 대상 편집 명령 (편집 알림으로 Undo 단계가 된다)
struct FEditorActions
{
	// 선택 엔티티(최상위, 자식 포함)를 같은 부모 아래 복제하고 복제본을 선택한다 (Ctrl+D)
	static void DuplicateSelection(FEditorContext& Context);

	// 선택 엔티티(최상위, 자식 포함)를 삭제하고 선택을 비운다 (Delete)
	static void DeleteSelection(FEditorContext& Context);

	// 파괴된 엔티티를 선택에서 뺀다
	static void PruneSelection(FEditorContext& Context);

	// 계층 → 콘텐츠 브라우저 드롭: Entity(선택된 엔티티면 선택의 최상위 전부)를 Directory에 .eprefab으로 저장하고
	// 원래 엔티티를 그 인스턴스로 바꾼다. 반환: 만든 파일들
	static std::vector<std::filesystem::path> CreatePrefabs(FEditorContext& Context, FEntity Entity, const std::filesystem::path& Directory);

	// 프리팹 인스턴스를 Parent 아래(없으면 루트)에 만들고 메시/머티리얼/모델을 해석한다 (선택/편집 알림은 호출자). 실패 시 NullEntity
	static FEntity InstantiatePrefab(FEditorContext& Context, const std::filesystem::path& Path, FEntity Parent);
};
