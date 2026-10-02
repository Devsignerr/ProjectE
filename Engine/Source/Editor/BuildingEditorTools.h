#pragma once

#include "Core/ECS/Entity.h"

#include <optional>

struct FEditorContext;

// 절차적 건물 에디터 도구 (인스펙터 추가 UI + 생성/지우기 + 자동 검증 인자).
//   생성 = FBuildingSceneBuilder::Generate → 에셋 해석(모델/메시 핸들) → MarkEdited("건물 생성") 한 단계 → (선택) 내비메시 굽기.
//   다시 생성 보존 정책은 Scene/Building/BuildingScene.h 머리 주석 (그룹 아래 교체, BuildingPartComponent 유지 표식은 남김, 유지를 켠 층/호실 그룹은 고정)
class FBuildingEditorTools
{
public:
	static void DrawInspector(FEditorContext& Context, FEntity Entity);
	// SeedOverride가 있으면 컴포넌트 시드를 바꾼 뒤 생성
	static bool Generate(FEditorContext& Context, FEntity Entity, std::optional<int32> SeedOverride = std::nullopt);
	static void Clear(FEditorContext& Context, FEntity Entity);

	// 자동 검증: --generate-building <엔티티 이름> [--building-seed N] [--building-section N] [--building-bake-navmesh]. 반환 = 생성한 건물 수
	static int32 RunCommandLine(FEditorContext& Context);
};
