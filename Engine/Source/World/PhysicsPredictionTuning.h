#pragma once

#include <algorithm>

// 물리 예측 상수 (World 모듈 내부 — 3D GameWorldPhysicsPrediction.cpp와 2D GameWorldPhysicsPrediction2D.cpp가 같은 값을 쓴다).
// 규칙은 GameWorldPhysicsPrediction.cpp 머리 주석이 기준
namespace PhysicsPredictionTuning
{
	inline constexpr float HistorySeconds            = 1.5f;   // 로컬 기록 보관 (왕복 지연 + 여유)
	inline constexpr float ReleaseDelaySeconds       = 1.0f;   // 근처/접촉이 이만큼 없으면 해제 후보
	inline constexpr float ReleaseRadiusScale        = 1.25f;  // 해제 판정 반경 (진입 반경 × — 경계에서 들락날락하지 않게)
	inline constexpr float RestSpeed                 = 5.0f;   // cm/s, 이보다 느리면 멈춘 것으로 본다
	inline constexpr float ReleaseErrorDistance      = 2.0f;   // cm, 서버와 이만큼 가까워야 해제
	inline constexpr float BlendOutSeconds           = 0.25f;  // 해제할 때 화면을 보간 위치로 옮기는 시간
	inline constexpr float PositionCorrectionSeconds = 0.15f;  // 위치 오차가 1/e로 줄어드는 시간
	inline constexpr float VelocityCorrectionSeconds = 0.15f;
	inline constexpr float RotationCorrectionSeconds = 0.15f;
	inline constexpr float SnapDistance              = 100.0f; // cm, 이보다 크면 바로 옮긴다
	inline constexpr float MaxEnterSpeed             = 300.0f; // cm/s, 이보다 빠른 물체는 닿을 때만 예측한다 (서버가 쏜 공 등 — 내가 영향을 주지 않는 빠른 물체)
	inline constexpr float MaxVelocitySampleGap      = 0.25f;  // 초, 이보다 먼 두 스냅샷으로는 속도를 구하지 않는다 (멈춰서 안 보내던 구간)
	inline constexpr float TimingDecayPerSecond      = 0.05f;  // 시각 오프셋(감소하는 최댓값)이 내려가는 속도
	inline constexpr float TimingResetSeconds        = 0.5f;   // 새 표본이 이보다 작으면 오프셋을 다시 잡는다 (지연이 크게 줄었을 때)

	inline float SmoothStep(float X)
	{
		X = std::clamp(X, 0.0f, 1.0f);
		return X * X * (3.0f - 2.0f * X);
	}
} // namespace PhysicsPredictionTuning
