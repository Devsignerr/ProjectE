#pragma once

#include "Core/Math/Vector3.h"

// 단위 쿼터니언 (회전).
// 부호 규약: FromAxisAngle(UpVector, +90°)는 Forward(+X)를 Right(+Y)로 돌린다 (UE와 동일).
// 합성: (A * B)는 B를 먼저 적용하고 그다음 A를 적용한다.
struct FQuat
{
	float X;
	float Y;
	float Z;
	float W;

	constexpr FQuat() : X(0.0f), Y(0.0f), Z(0.0f), W(1.0f) {}
	constexpr FQuat(float InX, float InY, float InZ, float InW) : X(InX), Y(InY), Z(InZ), W(InW) {}

	static const FQuat Identity;

	// 정규화된 축을 중심으로 AngleRadians 만큼 회전
	static FQuat FromAxisAngle(const FVector3& NormalizedAxis, float AngleRadians);

	// 오일러 각(도). UE 규약: +Pitch = 기수 위로(Forward→Up), +Yaw = 오른쪽으로(Forward→Right), +Roll = 오른쪽 날개 아래로(Right→-Up).
	// 적용 순서: Roll → Pitch → Yaw
	static FQuat FromEuler(float PitchDegrees, float YawDegrees, float RollDegrees);

	// 정규 직교 회전 행렬(FMatrix4x4 행벡터 규약, 스케일 없음)의 상단 3x3에서 쿼터니언 추출
	static FQuat FromRotationMatrix(const float (&M)[4][4]);

	// FromEuler의 역연산 (도). Pitch는 [-90, 90], 짐벌락(|Pitch| ≈ 90)에서는 Roll을 0으로 둔다
	void ToEuler(float& OutPitchDegrees, float& OutYawDegrees, float& OutRollDegrees) const;

	FQuat  operator*(const FQuat& Q) const;
	FQuat& operator*=(const FQuat& Q) { *this = *this * Q; return *this; }

	constexpr bool operator==(const FQuat& Q) const { return X == Q.X && Y == Q.Y && Z == Q.Z && W == Q.W; }
	constexpr bool operator!=(const FQuat& Q) const { return !(*this == Q); }

	FVector3 RotateVector(const FVector3& V) const;
	FVector3 UnrotateVector(const FVector3& V) const;

	constexpr FQuat Conjugate() const { return { -X, -Y, -Z, W }; }
	// 단위 쿼터니언 가정 (켤레와 동일)
	constexpr FQuat Inverse() const { return Conjugate(); }

	constexpr float LengthSquared() const { return X * X + Y * Y + Z * Z + W * W; }
	float           Length() const { return FMath::Sqrt(LengthSquared()); }

	FQuat GetNormalized(float Tolerance = FMath::SmallNumber) const;
	void  Normalize(float Tolerance = FMath::SmallNumber) { *this = GetNormalized(Tolerance); }
	bool  IsNormalized(float Tolerance = FMath::KindaSmallNumber) const { return FMath::Abs(1.0f - LengthSquared()) <= Tolerance; }

	FVector3 GetForwardVector() const { return RotateVector(FVector3::ForwardVector); }
	FVector3 GetRightVector() const { return RotateVector(FVector3::RightVector); }
	FVector3 GetUpVector() const { return RotateVector(FVector3::UpVector); }

	static constexpr float Dot(const FQuat& A, const FQuat& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W; }

	// 구면 선형 보간 (최단 경로, 결과 정규화)
	static FQuat Slerp(const FQuat& A, const FQuat& B, float Alpha);

	// q와 -q는 같은 회전이므로 동일하게 취급
	bool Equals(const FQuat& Q, float Tolerance = FMath::KindaSmallNumber) const;
};

inline const FQuat FQuat::Identity(0.0f, 0.0f, 0.0f, 1.0f);
