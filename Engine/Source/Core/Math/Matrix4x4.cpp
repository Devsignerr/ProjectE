#include "Core/Math/Matrix4x4.h"

#include "Core/Assert.h"

namespace
{
	// 여인수 행렬(전치된 수반 행렬)과 행렬식을 함께 계산
	float ComputeAdjugate(const float (&In)[4][4], float (&Out)[4][4])
	{
		const float* m = &In[0][0];
		float*       o = &Out[0][0];

		o[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		o[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		o[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		o[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		o[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		o[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		o[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		o[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		o[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
		o[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
		o[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
		o[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
		o[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9]  * m[2] * m[7]  + m[9]  * m[3] * m[6];
		o[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8]  * m[2] * m[7]  - m[8]  * m[3] * m[6];
		o[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]  - m[8]  * m[1] * m[7]  + m[8]  * m[3] * m[5];
		o[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]  + m[8]  * m[1] * m[6]  - m[8]  * m[2] * m[5];

		return m[0] * o[0] + m[1] * o[4] + m[2] * o[8] + m[3] * o[12];
	}
} // namespace

FMatrix4x4 FMatrix4x4::operator*(const FMatrix4x4& B) const
{
	FMatrix4x4 Result;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Col = 0; Col < 4; ++Col)
		{
			Result.M[Row][Col] = M[Row][0] * B.M[0][Col] + M[Row][1] * B.M[1][Col] +
			                     M[Row][2] * B.M[2][Col] + M[Row][3] * B.M[3][Col];
		}
	}
	return Result;
}

FVector4 FMatrix4x4::TransformVector4(const FVector4& V) const
{
	return {
		V.X * M[0][0] + V.Y * M[1][0] + V.Z * M[2][0] + V.W * M[3][0],
		V.X * M[0][1] + V.Y * M[1][1] + V.Z * M[2][1] + V.W * M[3][1],
		V.X * M[0][2] + V.Y * M[1][2] + V.Z * M[2][2] + V.W * M[3][2],
		V.X * M[0][3] + V.Y * M[1][3] + V.Z * M[2][3] + V.W * M[3][3],
	};
}

FVector3 FMatrix4x4::TransformPosition(const FVector3& P) const
{
	return {
		P.X * M[0][0] + P.Y * M[1][0] + P.Z * M[2][0] + M[3][0],
		P.X * M[0][1] + P.Y * M[1][1] + P.Z * M[2][1] + M[3][1],
		P.X * M[0][2] + P.Y * M[1][2] + P.Z * M[2][2] + M[3][2],
	};
}

FVector3 FMatrix4x4::TransformVector(const FVector3& V) const
{
	return {
		V.X * M[0][0] + V.Y * M[1][0] + V.Z * M[2][0],
		V.X * M[0][1] + V.Y * M[1][1] + V.Z * M[2][1],
		V.X * M[0][2] + V.Y * M[1][2] + V.Z * M[2][2],
	};
}

FMatrix4x4 FMatrix4x4::GetTransposed() const
{
	FMatrix4x4 Result;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Col = 0; Col < 4; ++Col)
		{
			Result.M[Row][Col] = M[Col][Row];
		}
	}
	return Result;
}

float FMatrix4x4::Determinant() const
{
	float Adjugate[4][4];
	return ComputeAdjugate(M, Adjugate);
}

bool FMatrix4x4::TryGetInverse(FMatrix4x4& OutInverse) const
{
	float       Adjugate[4][4];
	const float Det = ComputeAdjugate(M, Adjugate);
	// 특이 판정은 행렬 크기에 상대적으로 한다: |det| / (행 길이의 곱)은 행마다 배율을 바꿔도 같고 직교 행렬이면 1.
	//   절대값(|det| <= 1e-8)으로 판정하면 직교 투영 뷰-투영(det ≈ 4 / (폭 × 높이 × 깊이 범위) ≈ 1e-11)처럼
	//   멀쩡한 행렬을 특이로 보고 항등 행렬을 돌려준다 (에디터 직교 카메라 클릭 선택이 늘 원점 위로 쏘던 원인)
	float RowLengthProduct = 1.0f;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		RowLengthProduct *= FMath::Sqrt(M[Row][0] * M[Row][0] + M[Row][1] * M[Row][1] + M[Row][2] * M[Row][2] + M[Row][3] * M[Row][3]);
	}
	if (!(RowLengthProduct > 0.0f) || !(FMath::Abs(Det) > RowLengthProduct * 1.0e-6f))
	{
		return false;
	}

	const float InvDet = 1.0f / Det;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Col = 0; Col < 4; ++Col)
		{
			OutInverse.M[Row][Col] = Adjugate[Row][Col] * InvDet;
		}
	}
	return true;
}

FMatrix4x4 FMatrix4x4::GetInverse() const
{
	FMatrix4x4 Result;
	if (!TryGetInverse(Result))
	{
		return Identity;
	}
	return Result;
}

bool FMatrix4x4::Equals(const FMatrix4x4& Other, float Tolerance) const
{
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Col = 0; Col < 4; ++Col)
		{
			if (FMath::Abs(M[Row][Col] - Other.M[Row][Col]) > Tolerance)
			{
				return false;
			}
		}
	}
	return true;
}

void FMatrix4x4::Decompose(FVector3& OutTranslation, FQuat& OutRotation, FVector3& OutScale) const
{
	OutTranslation = GetOrigin();
	OutScale       = FVector3(GetAxisX().Length(), GetAxisY().Length(), GetAxisZ().Length());

	// 반사(행렬식 음수)면 X 스케일에 부호 반영
	if (Determinant() < 0.0f)
	{
		OutScale.X = -OutScale.X;
	}

	FMatrix4x4 RotationOnly;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		const float InvScale = FMath::Abs(OutScale[Row]) > FMath::SmallNumber ? 1.0f / OutScale[Row] : 0.0f;
		for (int32 Col = 0; Col < 3; ++Col)
		{
			RotationOnly.M[Row][Col] = M[Row][Col] * InvScale;
		}
	}
	OutRotation = FQuat::FromRotationMatrix(RotationOnly.M);
}

FMatrix4x4 FMatrix4x4::MakeTranslation(const FVector3& Translation)
{
	FMatrix4x4 Result;
	Result.SetOrigin(Translation);
	return Result;
}

FMatrix4x4 FMatrix4x4::MakeScale(const FVector3& Scale)
{
	FMatrix4x4 Result;
	Result.M[0][0] = Scale.X;
	Result.M[1][1] = Scale.Y;
	Result.M[2][2] = Scale.Z;
	return Result;
}

FMatrix4x4 FMatrix4x4::MakeRotation(const FQuat& Q)
{
	const float XX = Q.X * Q.X, YY = Q.Y * Q.Y, ZZ = Q.Z * Q.Z;
	const float XY = Q.X * Q.Y, XZ = Q.X * Q.Z, YZ = Q.Y * Q.Z;
	const float WX = Q.W * Q.X, WY = Q.W * Q.Y, WZ = Q.W * Q.Z;

	return FMatrix4x4(
		FVector4(1.0f - 2.0f * (YY + ZZ), 2.0f * (XY + WZ),        2.0f * (XZ - WY),        0.0f),
		FVector4(2.0f * (XY - WZ),        1.0f - 2.0f * (XX + ZZ), 2.0f * (YZ + WX),        0.0f),
		FVector4(2.0f * (XZ + WY),        2.0f * (YZ - WX),        1.0f - 2.0f * (XX + YY), 0.0f),
		FVector4(0.0f,                    0.0f,                    0.0f,                    1.0f));
}

FMatrix4x4 FMatrix4x4::MakeTransform(const FVector3& Translation, const FQuat& Rotation, const FVector3& Scale)
{
	return MakeScale(Scale) * MakeRotation(Rotation) * MakeTranslation(Translation);
}

FMatrix4x4 FMatrix4x4::MakeLookAt(const FVector3& Eye, const FVector3& Target, const FVector3& WorldUp)
{
	// 뷰 공간 축: Z = 앞, X = 오른쪽, Y = 위
	const FVector3 ZAxis = (Target - Eye).GetNormalized();
	E_CHECKF(!ZAxis.IsNearlyZero(), "MakeLookAt: Eye와 Target이 같습니다");

	FVector3 XAxis = FVector3::Cross(WorldUp, ZAxis).GetNormalized();
	if (XAxis.IsNearlyZero())
	{
		// 시선이 WorldUp과 평행하면 대체 Up 사용
		XAxis = FVector3::Cross(FVector3::ForwardVector, ZAxis).GetNormalized();
	}
	const FVector3 YAxis = FVector3::Cross(ZAxis, XAxis);

	return FMatrix4x4(
		FVector4(XAxis.X, YAxis.X, ZAxis.X, 0.0f),
		FVector4(XAxis.Y, YAxis.Y, ZAxis.Y, 0.0f),
		FVector4(XAxis.Z, YAxis.Z, ZAxis.Z, 0.0f),
		FVector4(-FVector3::Dot(XAxis, Eye), -FVector3::Dot(YAxis, Eye), -FVector3::Dot(ZAxis, Eye), 1.0f));
}

FMatrix4x4 FMatrix4x4::MakePerspectiveFov(float FovYRadians, float AspectRatio, float NearZ, float FarZ)
{
	E_CHECKF(FarZ > NearZ && NearZ > 0.0f, "MakePerspectiveFov: 잘못된 클립 거리 (Near {}, Far {})", NearZ, FarZ);

	const float YScale = 1.0f / FMath::Tan(FovYRadians * 0.5f);
	const float XScale = YScale / AspectRatio;
	const float Range  = FarZ / (FarZ - NearZ);

	return FMatrix4x4(
		FVector4(XScale, 0.0f,   0.0f,           0.0f),
		FVector4(0.0f,   YScale, 0.0f,           0.0f),
		FVector4(0.0f,   0.0f,   Range,          1.0f),
		FVector4(0.0f,   0.0f,   -NearZ * Range, 0.0f));
}

FMatrix4x4 FMatrix4x4::MakeOrthographic(float Width, float Height, float NearZ, float FarZ)
{
	E_CHECKF(FarZ > NearZ, "MakeOrthographic: 잘못된 클립 거리 (Near {}, Far {})", NearZ, FarZ);

	const float Range = 1.0f / (FarZ - NearZ);

	return FMatrix4x4(
		FVector4(2.0f / Width, 0.0f,          0.0f,           0.0f),
		FVector4(0.0f,         2.0f / Height, 0.0f,           0.0f),
		FVector4(0.0f,         0.0f,          Range,          0.0f),
		FVector4(0.0f,         0.0f,          -NearZ * Range, 1.0f));
}
