#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Editor/FoliageEditHistory.h"
#include "Scene/Foliage.h"

#include <random>
#include <string>
#include <vector>

class FInput;
struct FEditorContext;

// 폴리지 도구 (창 "폴리지"): 폴리지 타입 목록 편집(메시/머티리얼/밀도/크기/경사·높이/정렬/거리/충돌) + 브러시 칠하기·지우기.
//   - 칠할 면: 지형(정확한 높이·법선) + 정적 메시의 월드 경계 상자 윗면 (에디터에는 메시 CPU 정점이 없어 근사 — 상자/평평한 물체에 맞음)
//   - 칠하기 = 체크한 타입마다 원 안 목표 밀도까지 채움, Shift = 지우기 (체크한 타입), 스트로크 한 번 = Undo 한 단계
//   - 타입 설정 바꾸기도 Undo 한 단계 (위젯 조작이 끝날 때)
class FFoliageToolPanel
{
public:
	enum class EMode : int32
	{
		None,
		Paint,
		Erase,
	};

	void Update(FEditorContext& Context);
	void Draw(FEditorContext& Context);
	bool HandleViewport(FEditorContext& Context, const FInput& Input, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered);
	bool IsActive() const { return bOpen && Mode != EMode::None; }
	void SetMode(EMode NewMode) { Mode = NewMode; }

	// 자동 검증/스크립트용 스트로크 API
	bool BeginStroke(FEditorContext& Context, FEntity Foliage);
	void ApplyStroke(FEditorContext& Context, const FVector2& WorldXY, float DeltaSeconds, bool bErase);
	void EndStroke(FEditorContext& Context, const char* Label);
	void SetPaintTypes(const std::vector<bool>& Enabled) { PaintTypes = Enabled; }

	// 새 폴리지 엔티티 + .efoliage (Content/Foliage/, 기본 타입 5개: 풀/덤불/활엽수/침엽수/바위)
	FEntity                     CreateFoliage(FEditorContext& Context);
	static FFoliageAsset        MakeDefaultAsset();
	// 칠할 면 (지형 + 메시 경계 상자 윗면) 중 MaxZ 아래 가장 높은 곳
	static bool QuerySurface(FEditorContext& Context, float X, float Y, float MaxZ, FVector3& OutPosition, FVector3& OutNormal);
	FFoliageEditHistory& GetHistory() { return History; }

	float BrushRadius  = 800.0f; // cm
	float DensityScale = 1.0f;
	bool  bOpen        = true;
	bool  bAutomationCursor = false; // 자동 검증 스크린샷: 마우스가 밖이면 이 XY를 커서로
	FVector2 AutomationCursor;
	bool  bRequestFocus = false;

private:
	FEntity FindTarget(FEditorContext& Context) const;
	bool    RaycastSurface(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FVector3& OutHit) const;
	void    DrawBrushRing(FEditorContext& Context, const FVector2& ImageMin, const FVector2& ImageSize) const;
	void    DrawTypeEditor(FEditorContext& Context, FFoliageAsset& Asset, const std::string& AssetPath, FEntity Target);
	// 타입 설정 위젯 Undo: 위젯 조작 시작에 스냅샷, 끝나면 기록
	void    TrackEdit(FEditorContext& Context, FFoliageAsset& Asset, const std::string& AssetPath, FEntity Target);
	void    Commit(FEditorContext& Context, FFoliageAsset& Asset, const std::string& AssetPath, FEntity Target, FFoliageEditHistory::FSnapshot Before,
	               const char* Label);

	EMode             Mode = EMode::None;
	std::vector<bool> PaintTypes; // 타입별 칠하기 대상
	int32             SelectedType = 0;

	bool                           bStroking = false;
	FEntity                        StrokeEntity;
	std::string                    StrokeAsset;
	FFoliageEditHistory::FSnapshot StrokeBefore;
	bool                           bStrokeChanged = false;

	bool                           bEditing = false; // 타입 위젯 조작 중
	FFoliageEditHistory::FSnapshot EditBefore;

	bool     bHasHit = false;
	FVector3 HitPoint;

	std::mt19937        Random{ 20261001u };
	FFoliageEditHistory History;
};
