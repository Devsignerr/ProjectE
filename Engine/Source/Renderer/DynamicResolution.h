#pragma once

#include "Core/CoreTypes.h"

// 동적 해상도 제어기 (Phase 48, 순수 로직 — 테스트 DynamicResolution_*).
//   입력 = 프레임마다 GPU 씬 렌더 시간(ms, 타이머라 몇 프레임 늦은 값), 출력 = 화면 비율(%).
//   - 지수 평활(Smoothing) 값으로 판단한다 (한 프레임 튐에 반응하지 않음)
//   - 예산 초과(평활 > 목표 × DecreaseThreshold)면 줄이고, 여유(평활 < 목표 × IncreaseThreshold)면 늘린다 (그 사이는 유지 = 히스테리시스)
//   - 새 비율 = 현재 × sqrt(목표 × Headroom / 평활) (GPU 비용 ∝ 픽셀 수 ∝ 비율²) → Step(%) 단위로 반올림 → [Min, Max]
//   - 바꾼 뒤 MinFramesBetweenChanges 동안은 다시 바꾸지 않는다 (타이머 지연 + 평활이 따라올 시간). 크게 넘으면(×UrgentThreshold) 1/3만 기다린다
//   - 바꿀 때 평활 값을 예측치(× (새/옛)²)로 옮겨 다음 판단이 옛 측정에 끌려가지 않게 한다
//   이산 단계(기본 5%)인 이유: 내부 해상도 리소스는 크기별로 다시 만들기 때문 (SceneRenderer.h 동적 해상도 주석)
struct FDynamicResolutionSettings
{
	float  TargetGpuMs             = 16.6f;
	float  MinPercentage           = 50.0f;
	float  MaxPercentage           = 100.0f;
	float  StepPercentage          = 5.0f;
	uint32 MinFramesBetweenChanges = 30;
	float  Smoothing               = 0.1f;  // 지수 평활 계수 (새 표본 비중)
	float  DecreaseThreshold       = 1.0f;  // 평활 / 목표가 이보다 크면 줄인다
	float  IncreaseThreshold       = 0.85f; // 평활 / 목표가 이보다 작으면 늘린다
	float  Headroom                = 0.92f; // 새 비율의 목표 여유 (목표의 92%를 노린다 → 바로 다시 넘지 않게)
	float  UrgentThreshold         = 1.3f;  // 이만큼 넘으면 대기 프레임 1/3
};

class FDynamicResolutionController
{
public:
	// 현재 비율을 정하고 측정을 비운다 (켤 때/카메라 컷 등)
	void Reset(float Percentage);

	// 프레임 하나: GpuMs <= 0이면 측정 없음(그대로). 반환 = 이번 프레임에 쓸 화면 비율
	float Update(float GpuMs, const FDynamicResolutionSettings& Settings);

	float  GetPercentage() const { return Percentage; }
	float  GetSmoothedGpuMs() const { return SmoothedMs; }
	uint32 GetChangeCount() const { return ChangeCount; }

	// 순수 식: 평활 시간과 목표로 다음 비율 후보 (단계 반올림·범위 적용 전)
	static float ComputeDesiredPercentage(float Current, float SmoothedMs, float TargetMs, float Headroom);

private:
	float  Percentage       = 100.0f;
	float  SmoothedMs       = 0.0f;
	uint32 Samples          = 0;
	uint32 FramesSinceChange = 0;
	uint32 ChangeCount      = 0;
};
