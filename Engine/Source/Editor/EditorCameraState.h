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

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json); // 실패 시 false (값은 바뀌지 않음)

	bool Save(const std::filesystem::path& Path) const;
	bool Load(const std::filesystem::path& Path);

	// Bounds 전체가 화면에 들어오도록, 현재 시선 방향(Forward)을 유지한 채 물러난 카메라 위치
	static FVector3 ComputeFramingPosition(const FBox& Bounds, const FVector3& Forward, float FovYDegrees, float AspectRatio);
};
