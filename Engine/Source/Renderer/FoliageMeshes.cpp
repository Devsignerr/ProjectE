#include "Renderer/FoliageMeshes.h"

#include "Renderer/LodMath.h"
#include "Renderer/MeshSimplifier.h"

#include <cmath>
#include <map>
#include <utility>

namespace
{
	uint32 Hash(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7FEB352Du;
		X ^= X >> 15;
		X *= 0x846CA68Bu;
		X ^= X >> 16;
		return X;
	}

	float Random01(uint32& State)
	{
		State = Hash(State + 0x9E3779B9u);
		return static_cast<float>(State & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
	}

	// 삼각형 추가: 앞면(시계 방향)이 Facing 쪽을 보게 순서를 맞춘다
	void AddTriangle(FMeshData& Mesh, uint32 A, uint32 B, uint32 C, const FVector3& Facing)
	{
		const FVector3& P0 = Mesh.Vertices[A].Position;
		const FVector3& P1 = Mesh.Vertices[B].Position;
		const FVector3& P2 = Mesh.Vertices[C].Position;
		// 엔진 규약: 와인딩 검증 Cross(P1-P0, P2-P0)·Normal > 0 (CW 앞면)
		if (FVector3::Dot(FVector3::Cross(P1 - P0, P2 - P0), Facing) >= 0.0f)
		{
			Mesh.Indices.insert(Mesh.Indices.end(), { A, B, C });
		}
		else
		{
			Mesh.Indices.insert(Mesh.Indices.end(), { A, C, B });
		}
	}

	uint32 AddVertex(FMeshData& Mesh, const FVector3& Position, const FVector3& Normal, const FVector4& Color, const FVector2& UV = FVector2())
	{
		FVertex& Vertex = Mesh.Vertices.emplace_back();
		Vertex.Position = Position;
		Vertex.Normal   = Normal.GetNormalized();
		Vertex.Color    = Color;
		Vertex.UV       = UV;
		return static_cast<uint32>(Mesh.Vertices.size() - 1);
	}

	// 아이코스피어 (단위 구, Subdivisions 단계) → 정점/삼각형
	void MakeIcosphere(uint32 Subdivisions, std::vector<FVector3>& OutPoints, std::vector<uint32>& OutTriangles)
	{
		const float T = (1.0f + std::sqrt(5.0f)) * 0.5f;
		OutPoints     = { FVector3(-1, T, 0), FVector3(1, T, 0), FVector3(-1, -T, 0), FVector3(1, -T, 0), FVector3(0, -1, T), FVector3(0, 1, T),
                      FVector3(0, -1, -T), FVector3(0, 1, -T), FVector3(T, 0, -1), FVector3(T, 0, 1), FVector3(-T, 0, -1), FVector3(-T, 0, 1) };
		for (FVector3& Point : OutPoints)
		{
			Point = Point.GetNormalized();
		}
		OutTriangles = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
			             3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
		for (uint32 Level = 0; Level < Subdivisions; ++Level)
		{
			std::map<std::pair<uint32, uint32>, uint32> Midpoints;
			auto Midpoint = [&](uint32 A, uint32 B) {
				const std::pair<uint32, uint32> Key(std::min(A, B), std::max(A, B));
				if (const auto Found = Midpoints.find(Key); Found != Midpoints.end())
				{
					return Found->second;
				}
				OutPoints.push_back(((OutPoints[A] + OutPoints[B]) * 0.5f).GetNormalized());
				const uint32 Index = static_cast<uint32>(OutPoints.size() - 1);
				Midpoints.emplace(Key, Index);
				return Index;
			};
			std::vector<uint32> Next;
			for (size_t Index = 0; Index < OutTriangles.size(); Index += 3)
			{
				const uint32 A = OutTriangles[Index], B = OutTriangles[Index + 1], C = OutTriangles[Index + 2];
				const uint32 AB = Midpoint(A, B), BC = Midpoint(B, C), CA = Midpoint(C, A);
				Next.insert(Next.end(), { A, AB, CA, B, BC, AB, C, CA, BC, AB, BC, CA });
			}
			OutTriangles = std::move(Next);
		}
	}

	// 울퉁불퉁한 덩어리 (덤불/수관/바위): 단위 구를 Scale로 늘리고 반지름 방향 무작위 변형, 법선 = 면 법선 평균
	void AddBlob(FMeshData& Mesh, const FVector3& Center, const FVector3& Scale, float Roughness, uint32 Seed, const FVector4& ColorA, const FVector4& ColorB,
	             float FlattenBelow = -1.0e9f)
	{
		std::vector<FVector3> Points;
		std::vector<uint32>   Triangles;
		MakeIcosphere(2, Points, Triangles);
		const uint32 Base = static_cast<uint32>(Mesh.Vertices.size());
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			uint32       State = Seed * 7919u + static_cast<uint32>(Index) * 104729u;
			const float  Bump  = 1.0f + (Random01(State) - 0.5f) * 2.0f * Roughness;
			FVector3     P     = FVector3(Points[Index].X * Scale.X, Points[Index].Y * Scale.Y, Points[Index].Z * Scale.Z) * Bump;
			P.Z                = std::max(P.Z, FlattenBelow);
			const float  Tint  = Random01(State);
			AddVertex(Mesh, Center + P, Points[Index], FMath::Lerp(ColorA, ColorB, Tint));
		}
		// 면 법선 누적
		std::vector<FVector3> Normals(Points.size(), FVector3::ZeroVector);
		for (size_t Index = 0; Index < Triangles.size(); Index += 3)
		{
			const FVector3& P0 = Mesh.Vertices[Base + Triangles[Index]].Position;
			const FVector3& P1 = Mesh.Vertices[Base + Triangles[Index + 1]].Position;
			const FVector3& P2 = Mesh.Vertices[Base + Triangles[Index + 2]].Position;
			FVector3        N  = FVector3::Cross(P1 - P0, P2 - P0);
			const FVector3  Outward = ((P0 + P1 + P2) / 3.0f) - Center;
			if (FVector3::Dot(N, Outward) < 0.0f)
			{
				N = -N;
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Normals[Triangles[Index + Corner]] += N;
			}
		}
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			Mesh.Vertices[Base + Index].Normal = Normals[Index].GetNormalized();
		}
		for (size_t Index = 0; Index < Triangles.size(); Index += 3)
		{
			const FVector3 Outward = Mesh.Vertices[Base + Triangles[Index]].Position - Center;
			AddTriangle(Mesh, Base + Triangles[Index], Base + Triangles[Index + 1], Base + Triangles[Index + 2], Outward);
		}
	}

	// 원뿔대 (줄기/원뿔 수관): 밑 반지름 → 위 반지름, 옆면만 (+ 위가 0이면 뾰족)
	void AddFrustumSides(FMeshData& Mesh, const FVector3& Bottom, float Height, float RadiusBottom, float RadiusTop, uint32 Sides, const FVector4& ColorBottom,
	                     const FVector4& ColorTop)
	{
		const uint32 Base  = static_cast<uint32>(Mesh.Vertices.size());
		const float  Slope = (RadiusBottom - RadiusTop) / std::max(Height, 1.0f);
		for (uint32 Side = 0; Side <= Sides; ++Side)
		{
			const float    Angle  = FMath::TwoPi * static_cast<float>(Side) / static_cast<float>(Sides);
			const FVector3 Radial(std::cos(Angle), std::sin(Angle), 0.0f);
			const FVector3 Normal = FVector3(Radial.X, Radial.Y, Slope).GetNormalized();
			const float    U      = static_cast<float>(Side) / static_cast<float>(Sides);
			AddVertex(Mesh, Bottom + Radial * RadiusBottom, Normal, ColorBottom, FVector2(U, 1.0f));
			AddVertex(Mesh, Bottom + Radial * RadiusTop + FVector3(0.0f, 0.0f, Height), Normal, ColorTop, FVector2(U, 0.0f));
		}
		for (uint32 Side = 0; Side < Sides; ++Side)
		{
			const uint32   B0 = Base + Side * 2, T0 = B0 + 1, B1 = B0 + 2, T1 = B0 + 3;
			const FVector3 Outward = Mesh.Vertices[B0].Normal + Mesh.Vertices[B1].Normal;
			AddTriangle(Mesh, B0, B1, T1, Outward);
			if (RadiusTop > 0.0f)
			{
				AddTriangle(Mesh, B0, T1, T0, Outward);
			}
		}
	}

	void BuildGrass(FMeshData& Mesh)
	{
		// 잎 9장, 앞뒤 면을 따로 (뒷면 컬링에서도 보이게)
		uint32 State = 1234u;
		const FVector4 BaseColor(0.03f, 0.07f, 0.015f, 1.0f);
		const FVector4 TipColor(0.16f, 0.22f, 0.05f, 1.0f);
		for (uint32 Blade = 0; Blade < 9; ++Blade)
		{
			const float    Angle  = FMath::TwoPi * Random01(State);
			const float    Offset = 12.0f * Random01(State);
			const float    Height = 30.0f + 30.0f * Random01(State);
			const float    Lean   = 6.0f + 12.0f * Random01(State);
			const float    Width  = 2.5f + 1.5f * Random01(State);
			const FVector3 Dir(std::cos(Angle), std::sin(Angle), 0.0f);
			const FVector3 Side(-Dir.Y, Dir.X, 0.0f);
			const FVector3 Root   = Dir * Offset * 0.5f;
			const FVector3 Tip    = Root + Dir * Lean + FVector3(0.0f, 0.0f, Height);
			const FVector3 Facing = FVector3::Cross(Side, Tip - Root).GetNormalized();
			for (int32 Face = 0; Face < 2; ++Face)
			{
				const FVector3 N  = Face == 0 ? Facing : -Facing;
				// 법선을 조금 위로 기울여 하늘빛을 받게
				const FVector3 Shade = (N + FVector3(0.0f, 0.0f, 0.8f)).GetNormalized();
				const uint32   A  = AddVertex(Mesh, Root - Side * Width, Shade, BaseColor, FVector2(0.0f, 1.0f));
				const uint32   B  = AddVertex(Mesh, Root + Side * Width, Shade, BaseColor, FVector2(1.0f, 1.0f));
				const uint32   C  = AddVertex(Mesh, Tip, Shade, TipColor, FVector2(0.5f, 0.0f));
				AddTriangle(Mesh, A, B, C, N);
			}
		}
	}

	void BuildTree(FMeshData& Mesh)
	{
		const FVector4 BarkA(0.06f, 0.035f, 0.02f, 1.0f), BarkB(0.10f, 0.065f, 0.035f, 1.0f);
		AddFrustumSides(Mesh, FVector3(0.0f, 0.0f, -20.0f), 380.0f, 22.0f, 12.0f, 10, BarkA, BarkB);
		const FVector4 LeafA(0.025f, 0.06f, 0.012f, 1.0f), LeafB(0.07f, 0.13f, 0.025f, 1.0f);
		AddBlob(Mesh, FVector3(0.0f, 0.0f, 430.0f), FVector3(190.0f, 190.0f, 150.0f), 0.18f, 11u, LeafA, LeafB);
		AddBlob(Mesh, FVector3(70.0f, 40.0f, 520.0f), FVector3(130.0f, 130.0f, 110.0f), 0.2f, 12u, LeafA, LeafB);
		AddBlob(Mesh, FVector3(-60.0f, -50.0f, 360.0f), FVector3(140.0f, 140.0f, 100.0f), 0.2f, 13u, LeafA, LeafB);
	}

	void BuildPine(FMeshData& Mesh)
	{
		const FVector4 Bark(0.07f, 0.04f, 0.025f, 1.0f);
		AddFrustumSides(Mesh, FVector3(0.0f, 0.0f, -20.0f), 220.0f, 18.0f, 10.0f, 8, Bark, Bark);
		const FVector4 NeedleA(0.012f, 0.035f, 0.015f, 1.0f), NeedleB(0.03f, 0.075f, 0.03f, 1.0f);
		// 겹친 원뿔 4단
		for (int32 Tier = 0; Tier < 4; ++Tier)
		{
			const float Z      = 150.0f + 120.0f * static_cast<float>(Tier);
			const float Radius = 170.0f - 32.0f * static_cast<float>(Tier);
			AddFrustumSides(Mesh, FVector3(0.0f, 0.0f, Z), 230.0f, Radius, 0.0f, 16, NeedleA, NeedleB);
		}
	}
} // namespace

bool FoliageMeshes::Build(std::string_view Name, FMeshData& OutMesh)
{
	OutMesh = FMeshData{};
	if (Name == "grass")
	{
		BuildGrass(OutMesh);
	}
	else if (Name == "bush")
	{
		AddBlob(OutMesh, FVector3(0.0f, 0.0f, 35.0f), FVector3(70.0f, 70.0f, 55.0f), 0.25f, 21u, FVector4(0.02f, 0.05f, 0.012f, 1.0f),
		        FVector4(0.06f, 0.11f, 0.02f, 1.0f), -5.0f);
	}
	else if (Name == "tree")
	{
		BuildTree(OutMesh);
	}
	else if (Name == "pine")
	{
		BuildPine(OutMesh);
	}
	else if (Name == "rock")
	{
		AddBlob(OutMesh, FVector3(0.0f, 0.0f, 20.0f), FVector3(80.0f, 65.0f, 50.0f), 0.3f, 31u, FVector4(0.06f, 0.058f, 0.055f, 1.0f),
		        FVector4(0.16f, 0.15f, 0.14f, 1.0f), -10.0f);
	}
	else
	{
		return false;
	}
	OutMesh.ComputeTangents();
	if (OutMesh.Indices.size() / 3 >= MeshSimplifier::MinTriangles)
	{
		MeshSimplifier::GenerateLods(OutMesh, LodMath::MaxLods);
	}
	return true;
}
