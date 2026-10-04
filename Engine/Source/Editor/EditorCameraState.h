#pragma once

#include "Core/Math/Math.h"

#include <filesystem>
#include <string>

// 에디터 카메라 상태: 프로젝트 Saved/EditorCamera.json에 저장해 다음 실행 때 복원한다
struct FEditorCameraState
{
	FVector3 Position  = FVector3(-600.0f, -400.0f, 300.0f);
	FQuat    Rotation;
	float    MoveSpeed = 500.0f; // cm/초
	bool     bOrthographic = false;   // 편집 뷰포트 직교 투영
	float    OrthoHeight   = 1000.0f; // cm (직교일 때 화면 세로가 담는 월드 높이)
	// 뷰포트 2D 모드 (Phase 56-5a): 켜져 있으면 위 카메라는 2D 카메라(+Y에서 -Y, 직교)이고, 3D로 돌아갈 때 아래 3D 카메라를 되살린다
	bool     bViewport2D        = false;
	bool     bHasSaved3D        = false;
	FVector3 Saved3DPosition;
	FQuat    Saved3DRotation;
	bool     bSaved3DOrthographic = false;
	float    Saved3DOrthoHeight   = 1000.0f;

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json); // 실패 시 false (값은 바뀌지 않음)

	bool Save(const std::filesystem::path& Path) const;
	bool Load(const std::filesystem::path& Path);

	// Bounds 전체가 화면에 들어오도록, 현재 시선 방향(Forward)을 유지한 채 물러난 카메라 위치
	static FVector3 ComputeFramingPosition(const FBox& Bounds, const FVector3& Forward, float FovYDegrees, float AspectRatio);
	// 직교: Bounds 경계 구가 좁은 쪽 화면에 여유 있게 들어오는 직교 높이 (cm)
	static float ComputeFramingOrthoHeight(const FBox& Bounds, float AspectRatio);
	// 원근 → 직교 전환 시 초점 거리(FocusDistance)에서 화면 크기가 같아 보이는 직교 높이
	static float ComputeMatchingOrthoHeight(float FovYDegrees, float FocusDistance);

	static constexpr float MinOrthoHeight = 10.0f;     // cm
	static constexpr float MaxOrthoHeight = 1000000.0f;
};
