#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Editor/TerrainEditHistory.h"
#include "Scene/Terrain.h"

#include <filesystem>
#include <string>
#include <vector>

class FInput;
struct FEditorContext;

// 지형 도구 (창 "지형"): 새 지형 만들기, 스컬프트(올리기/내리기/평탄화/부드럽게/노이즈) · 레이어 칠하기 브러시,
// 높이맵 가져오기/내보내기, 저장. 모드가 켜져 있으면 뷰포트 좌클릭 드래그가 브러시가 된다 (기즈모/클릭 선택 대신).
//   - 대상: 선택한 지형 엔티티, 없으면 커서 아래 지형
//   - 스트로크 한 번 = Undo 한 단계 (마우스를 놓을 때 MarkEdited — 끄는 도중에는 커밋하지 않는다)
//   - 브러시 원은 지형 높이를 따라 그린 ImGui 선 (TerrainMath::Raycast)
//   - 단축키 (뷰포트, 모드 중): Shift = 반대 동작(올리기↔내리기, 칠하기 → 레이어 0), [ ] = 반경
class FTerrainToolPanel
{
public:
	enum class EMode : int32
	{
		None,
		Sculpt,
		Paint,
	};

	// 매 프레임 (UI 전): Undo/Redo로 씬의 EditRevision이 바뀐 지형 데이터를 기록으로 맞춘다
	void Update(FEditorContext& Context);
	void Draw(FEditorContext& Context);
	// 뷰포트 이미지 위 (ImGui 프레임 안). 반환: 이번 프레임 마우스를 가져갔다 (기즈모/선택 생략)
	bool HandleViewport(FEditorContext& Context, const FInput& Input, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered);
	bool IsActive() const { return bOpen && Mode != EMode::None; }

	// ---- 자동 검증/스크립트용 스트로크 API (뷰포트와 같은 경로)
	bool BeginStroke(FEditorContext& Context, FEntity Terrain);
	void ApplyStroke(FEditorContext& Context, const FVector2& WorldXY, float DeltaSeconds);
	void EndStroke(FEditorContext& Context, const char* Label);
	FTerrainBrush& GetBrush() { return Brush; }
	void           SetMode(EMode NewMode) { Mode = NewMode; }

	// 새 지형 엔티티 + .eterrain (Content/Terrain/). 반환: 엔티티 (실패 시 NullEntity)
	FEntity CreateTerrain(FEditorContext& Context, uint32 Resolution, float Size);
	FTerrainEditHistory& GetHistory() { return History; }

	bool bOpen = true;
	// 자동 검증 스크린샷용: 마우스가 뷰포트 밖이면 이 월드 XY를 커서로 삼아 브러시 원을 그린다
	bool     bAutomationCursor = false;
	FVector2 AutomationCursor;
	bool     bRequestFocus = false; // 다음 Draw에서 창을 앞으로

private:
	FEntity FindTarget(FEditorContext& Context) const;
	bool    RaycastTerrains(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FEntity& OutEntity, FVector3& OutHit) const;
	void    DrawBrushRing(FEditorContext& Context, const FVector2& ImageMin, const FVector2& ImageSize) const;
	void    ImportHeightmap(FEditorContext& Context, FEntity Terrain, const std::filesystem::path& Path);
	void    ExportHeightmap(FEditorContext& Context, FEntity Terrain, const std::filesystem::path& Path);
	void    Notify(FEditorContext& Context, const std::string& Message, bool bError) const;

	EMode           Mode     = EMode::None;
	ETerrainBrushOp SculptOp = ETerrainBrushOp::Raise;
	FTerrainBrush   Brush;

	// 스트로크 (시작 스냅샷 → 끝날 때 바뀐 영역만 기록)
	bool                bStroking = false;
	FEntity             StrokeEntity;
	std::string         StrokeAsset;
	std::vector<uint16> StrokeHeights;
	std::vector<uint32> StrokeWeights;
	FTerrainRect        StrokeRect;

	// 커서 아래 지형 (직전 HandleViewport)
	bool     bHasHit = false;
	FEntity  HitEntity;
	FVector3 HitPoint;

	// 새 지형 설정
	int32 NewResolutionIndex = 1; // 129 / 257 / 513 / 1025
	float NewSize            = 20000.0f;

	FTerrainEditHistory History;
};
