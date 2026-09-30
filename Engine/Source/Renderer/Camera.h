#pragma once

#include "Core/Math/Math.h"

// 카메라. 위치 + 회전(쿼터니언)으로 뷰 행렬을, FOV(원근) 또는 직교 높이/종횡비/클립 거리로 투영 행렬을 만든다.
class FCamera
{
public:
	void SetPosition(const FVector3& InPosition) { Position = InPosition; }
	void SetRotation(const FQuat& InRotation) { Rotation = InRotation; }

	// Target을 바라보도록 회전 설정 (Roll 없음)
	void LookAt(const FVector3& Target);

	void SetPerspective(float InFovYDegrees, float InAspectRatio, float InNearZ, float InFarZ);
	void SetAspectRatio(float InAspectRatio) { AspectRatio = InAspectRatio; }
	// 직교 투영: 화면 세로가 담는 월드 높이(cm). 종횡비/클립 거리는 원근과 공유
	void SetOrthographic(float InOrthoHeight, float InAspectRatio, float InNearZ, float InFarZ);
	// 원근 모드로 되돌린다 (FOV는 유지)
	void SetPerspectiveMode() { bOrthographic = false; }

	const FVector3& GetPosition() const { return Position; }
	const FQuat&    GetRotation() const { return Rotation; }
	float           GetFovYDegrees() const { return FovYDegrees; }
	float           GetAspectRatio() const { return AspectRatio; }
	float           GetNearZ() const { return NearZ; }
	float           GetFarZ() const { return FarZ; }
	bool            IsOrthographic() const { return bOrthographic; }
	float           GetOrthoHeight() const { return OrthoHeight; }

	FVector3 GetForwardVector() const { return Rotation.GetForwardVector(); }
	FVector3 GetRightVector() const { return Rotation.GetRightVector(); }
	FVector3 GetUpVector() const { return Rotation.GetUpVector(); }

	FMatrix4x4 GetViewMatrix() const;
	FMatrix4x4 GetProjectionMatrix() const;
	FMatrix4x4 GetViewProjectionMatrix() const { return GetViewMatrix() * GetProjectionMatrix(); }

private:
	FVector3 Position;
	FQuat    Rotation;
	float    FovYDegrees = 60.0f;
	float    AspectRatio = 16.0f / 9.0f;
	float    NearZ       = 10.0f;     // cm
	float    FarZ        = 100000.0f; // cm
	float    OrthoHeight = 1000.0f;   // cm (직교일 때만)
	bool     bOrthographic = false;
};
