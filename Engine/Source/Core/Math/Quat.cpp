#include "Core/Math/Quat.h"

FQuat FQuat::FromAxisAngle(const FVector3& NormalizedAxis, float AngleRadians)
{
	float S, C;
	FMath::SinCos(&S, &C, AngleRadians * 0.5f);
	return { NormalizedAxis.X * S, NormalizedAxis.Y * S, NormalizedAxis.Z * S, C };
}

FQuat FQuat::FromEuler(float PitchDegrees, float YawDegrees, float RollDegrees)
{
	// 왼손 Z-up 축계에서 UE 부호 규약을 맞추기 위해 Pitch/Roll은 음의 축 회전
	const FQuat Yaw   = FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(YawDegrees));
	const FQuat Pitch = FromAxisAngle(FVector3::RightVector, -FMath::DegreesToRadians(PitchDegrees));
	const FQuat Roll  = FromAxisAngle(FVector3::ForwardVector, -FMath::DegreesToRadians(RollDegrees));

	// Roll → Pitch → Yaw 순으로 적용
	return Yaw * Pitch * Roll;
}

FQuat FQuat::FromRotationMatrix(const float (&M)[4][4])
{
	// MakeRotation의 역연산. 대각합이 양수면 W 우선, 아니면 가장 큰 대각 성분 우선 (수치 안정성)
	const float Trace = M[0][0] + M[1][1] + M[2][2];
	FQuat       Result;

	if (Trace > 0.0f)
	{
		float S  = FMath::Sqrt(Trace + 1.0f);
		Result.W = S * 0.5f;
		S        = 0.5f / S;
		Result.X = (M[1][2] - M[2][1]) * S;
		Result.Y = (M[2][0] - M[0][2]) * S;
		Result.Z = (M[0][1] - M[1][0]) * S;
	}
	else
	{
		int32 I = 0;
		if (M[1][1] > M[0][0]) I = 1;
		if (M[2][2] > M[I][I]) I = 2;
		const int32 J = (I + 1) % 3;
		const int32 K = (I + 2) % 3;

		float Q[4];
		float S = FMath::Sqrt(M[I][I] - M[J][J] - M[K][K] + 1.0f);
		Q[I]    = S * 0.5f;
		S       = 0.5f / S;
		Q[3]    = (M[J][K] - M[K][J]) * S;
		Q[J]    = (M[I][J] + M[J][I]) * S;
		Q[K]    = (M[I][K] + M[K][I]) * S;

		Result = FQuat(Q[0], Q[1], Q[2], Q[3]);
	}

	return Result.GetNormalized();
}

FQuat FQuat::operator*(const FQuat& Q) const
{
	// 해밀턴 곱: (this * Q) — Q 먼저, this 나중
	return {
		W * Q.X + X * Q.W + Y * Q.Z - Z * Q.Y,
		W * Q.Y - X * Q.Z + Y * Q.W + Z * Q.X,
		W * Q.Z + X * Q.Y - Y * Q.X + Z * Q.W,
		W * Q.W - X * Q.X - Y * Q.Y - Z * Q.Z,
	};
}

FVector3 FQuat::RotateVector(const FVector3& V) const
{
	// v' = v + 2w(q × v) + 2 q × (q × v)
	const FVector3 QVec(X, Y, Z);
	const FVector3 T = FVector3::Cross(QVec, V) * 2.0f;
	return V + T * W + FVector3::Cross(QVec, T);
}

FVector3 FQuat::UnrotateVector(const FVector3& V) const
{
	return Conjugate().RotateVector(V);
}

FQuat FQuat::GetNormalized(float Tolerance) const
{
	const float LenSq = LengthSquared();
	if (LenSq > Tolerance)
	{
		const float Scale = FMath::InvSqrt(LenSq);
		return { X * Scale, Y * Scale, Z * Scale, W * Scale };
	}
	return Identity;
}

FQuat FQuat::Slerp(const FQuat& A, const FQuat& B, float Alpha)
{
	float CosAngle = Dot(A, B);

	// 최단 경로 선택
	float Sign = 1.0f;
	if (CosAngle < 0.0f)
	{
		CosAngle = -CosAngle;
		Sign     = -1.0f;
	}

	float ScaleA;
	float ScaleB;
	if (CosAngle < 0.9999f)
	{
		const float Angle    = FMath::Acos(CosAngle);
		const float InvSin   = 1.0f / FMath::Sin(Angle);
		ScaleA = FMath::Sin((1.0f - Alpha) * Angle) * InvSin;
		ScaleB = FMath::Sin(Alpha * Angle) * InvSin;
	}
	else
	{
		// 거의 같은 회전이면 선형 보간으로 대체
		ScaleA = 1.0f - Alpha;
		ScaleB = Alpha;
	}
	ScaleB *= Sign;

	const FQuat Result(A.X * ScaleA + B.X * ScaleB,
	                   A.Y * ScaleA + B.Y * ScaleB,
	                   A.Z * ScaleA + B.Z * ScaleB,
	                   A.W * ScaleA + B.W * ScaleB);
	return Result.GetNormalized();
}

bool FQuat::Equals(const FQuat& Q, float Tolerance) const
{
	const bool bSame = FMath::Abs(X - Q.X) <= Tolerance && FMath::Abs(Y - Q.Y) <= Tolerance &&
	                   FMath::Abs(Z - Q.Z) <= Tolerance && FMath::Abs(W - Q.W) <= Tolerance;
	const bool bNegated = FMath::Abs(X + Q.X) <= Tolerance && FMath::Abs(Y + Q.Y) <= Tolerance &&
	                      FMath::Abs(Z + Q.Z) <= Tolerance && FMath::Abs(W + Q.W) <= Tolerance;
	return bSame || bNegated;
}
