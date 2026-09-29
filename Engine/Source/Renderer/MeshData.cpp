#include "Renderer/MeshData.h"

namespace
{
	// 법선에 수직인 임의 방향 (UV가 없거나 퇴화했을 때)
	FVector3 AnyPerpendicular(const FVector3& Normal)
	{
		const FVector3 Reference = FMath::Abs(Normal.Z) < 0.9f ? FVector3::UpVector : FVector3::ForwardVector;
		return FVector3::Cross(Reference, Normal).GetNormalized();
	}
} // namespace

FTangentBasis ComputeTriangleTangentBasis(const FVector3& P0, const FVector3& P1, const FVector3& P2,
                                          const FVector2& UV0, const FVector2& UV1, const FVector2& UV2)
{
	const FVector3 E1  = P1 - P0;
	const FVector3 E2  = P2 - P0;
	const FVector2 DUV1 = UV1 - UV0;
	const FVector2 DUV2 = UV2 - UV0;

	FTangentBasis Basis;
	const float Determinant = DUV1.X * DUV2.Y - DUV2.X * DUV1.Y;
	if (FMath::Abs(Determinant) < 1.0e-12f)
	{
		return Basis;
	}
	const float InvDeterminant = 1.0f / Determinant;

	// E = dP/du * dU + dP/dv * dV 를 풀면
	const FVector3 dPdu = (E1 * DUV2.Y - E2 * DUV1.Y) * InvDeterminant;
	const FVector3 dPdv = (E2 * DUV1.X - E1 * DUV2.X) * InvDeterminant;
	Basis.Tangent   = dPdu;
	Basis.Bitangent = -dPdv; // 노멀 맵 +Y = UV 위쪽 = -V
	Basis.bValid    = true;
	return Basis;
}

FVector4 OrthonormalizeTangent(const FVector3& Normal, const FVector3& Tangent, const FVector3& Bitangent)
{
	// 그람-슈미트: 탄젠트에서 법선 성분 제거
	FVector3 T = (Tangent - Normal * FVector3::Dot(Normal, Tangent)).GetNormalized();
	if (T.IsNearlyZero())
	{
		T = AnyPerpendicular(Normal);
	}
	// Cross(N, T)가 원하는 바이탄젠트와 같은 방향이면 +1
	const float Sign = FVector3::Dot(FVector3::Cross(Normal, T), Bitangent) < 0.0f ? -1.0f : 1.0f;
	return FVector4(T, Sign);
}

void FMeshData::ComputeTangents()
{
	std::vector<FVector3> Tangents(Vertices.size());
	std::vector<FVector3> Bitangents(Vertices.size());

	for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
	{
		const uint32 I0 = Indices[Index + 0];
		const uint32 I1 = Indices[Index + 1];
		const uint32 I2 = Indices[Index + 2];
		const FTangentBasis Basis = ComputeTriangleTangentBasis(Vertices[I0].Position, Vertices[I1].Position, Vertices[I2].Position,
		                                                        Vertices[I0].UV, Vertices[I1].UV, Vertices[I2].UV);
		if (!Basis.bValid)
		{
			continue;
		}
		// 면적 가중 없이 누적 (dPdu 크기가 이미 삼각형 UV 밀도를 반영)
		for (const uint32 I : { I0, I1, I2 })
		{
			Tangents[I] += Basis.Tangent;
			Bitangents[I] += Basis.Bitangent;
		}
	}

	for (size_t Index = 0; Index < Vertices.size(); ++Index)
	{
		Vertices[Index].Tangent = OrthonormalizeTangent(Vertices[Index].Normal, Tangents[Index], Bitangents[Index]);
	}
}
