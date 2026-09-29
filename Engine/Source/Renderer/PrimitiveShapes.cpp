#include "Renderer/PrimitiveShapes.h"

#include <algorithm>
#include <cmath>

namespace
{
	// 바깥에서 봤을 때 시계 방향이 되도록 사각형 면 추가.
	// 바깥에서 면을 보는 시선 방향은 -Normal 이므로, 보는 사람의 오른쪽 = Cross(Up, -Normal) (왼손 좌표계 LookAt과 동일 규칙)
	void AddQuadFace(FMeshData& Mesh, const FVector3& Center, const FVector3& Normal, const FVector3& Up, float HalfExtent,
	                 const FVector4& Color)
	{
		const FVector3 Right = FVector3::Cross(Up, -Normal);
		const uint32   Base  = static_cast<uint32>(Mesh.Vertices.size());

		// 왼쪽 아래 → 왼쪽 위 → 오른쪽 위 → 오른쪽 아래 (UV 원점은 왼쪽 위)
		const FVector3 Corners[4] = {
			Center + (-Right - Up) * HalfExtent,
			Center + (-Right + Up) * HalfExtent,
			Center + (Right + Up) * HalfExtent,
			Center + (Right - Up) * HalfExtent,
		};
		const FVector2 UVs[4] = { { 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f } };

		for (int32 Index = 0; Index < 4; ++Index)
		{
			FVertex Vertex;
			Vertex.Position = Corners[Index];
			Vertex.Normal   = Normal;
			Vertex.UV       = UVs[Index];
			Vertex.Color    = Color;
			Mesh.Vertices.push_back(Vertex);
		}

		Mesh.Indices.insert(Mesh.Indices.end(), { Base + 0, Base + 1, Base + 2, Base + 0, Base + 2, Base + 3 });
	}
} // namespace

FMeshData FPrimitiveShapes::MakeCube(float Size, const FVector4& Color)
{
	const float HalfExtent = Size * 0.5f;

	FMeshData Mesh;
	Mesh.Vertices.reserve(24);
	Mesh.Indices.reserve(36);

	const FVector3 X = FVector3::ForwardVector;
	const FVector3 Y = FVector3::RightVector;
	const FVector3 Z = FVector3::UpVector;

	AddQuadFace(Mesh, X * HalfExtent, X, Z, HalfExtent, Color);   // 앞 (+X)
	AddQuadFace(Mesh, -X * HalfExtent, -X, Z, HalfExtent, Color); // 뒤 (-X)
	AddQuadFace(Mesh, Y * HalfExtent, Y, Z, HalfExtent, Color);   // 오른쪽 (+Y)
	AddQuadFace(Mesh, -Y * HalfExtent, -Y, Z, HalfExtent, Color); // 왼쪽 (-Y)
	AddQuadFace(Mesh, Z * HalfExtent, Z, X, HalfExtent, Color);   // 위 (+Z)
	AddQuadFace(Mesh, -Z * HalfExtent, -Z, X, HalfExtent, Color); // 아래 (-Z)

	Mesh.ComputeTangents();
	return Mesh;
}

FMeshData FPrimitiveShapes::MakeSphere(float Radius, uint32 Segments, uint32 Rings, const FVector4& Color)
{
	Segments = std::max(Segments, 3u);
	Rings    = std::max(Rings, 2u);

	FMeshData Mesh;
	Mesh.Vertices.reserve(static_cast<size_t>(Rings + 1) * (Segments + 1));
	Mesh.Indices.reserve(static_cast<size_t>(Rings) * Segments * 6);

	for (uint32 Ring = 0; Ring <= Rings; ++Ring)
	{
		const float Theta = FMath::Pi * static_cast<float>(Ring) / static_cast<float>(Rings); // 0 = +Z 극
		for (uint32 Segment = 0; Segment <= Segments; ++Segment)
		{
			const float    Phi = 2.0f * FMath::Pi * static_cast<float>(Segment) / static_cast<float>(Segments);
			const FVector3 Direction(std::sin(Theta) * std::cos(Phi), std::sin(Theta) * std::sin(Phi), std::cos(Theta));
			FVertex        Vertex;
			Vertex.Position = Direction * Radius;
			Vertex.Normal   = Direction;
			Vertex.UV       = FVector2(static_cast<float>(Segment) / static_cast<float>(Segments), static_cast<float>(Ring) / static_cast<float>(Rings));
			Vertex.Color    = Color;
			Mesh.Vertices.push_back(Vertex);
		}
	}

	auto AddTriangle = [&Mesh](uint32 A, uint32 B, uint32 C) {
		const FVector3& P0     = Mesh.Vertices[A].Position;
		const FVector3& P1     = Mesh.Vertices[B].Position;
		const FVector3& P2     = Mesh.Vertices[C].Position;
		const FVector3  Cross  = FVector3::Cross(P1 - P0, P2 - P0);
		const FVector3  Normal = Mesh.Vertices[A].Normal + Mesh.Vertices[B].Normal + Mesh.Vertices[C].Normal;
		if (Cross.LengthSquared() <= 1.0e-12f)
		{
			return; // 극점의 퇴화 삼각형
		}
		if (FVector3::Dot(Cross, Normal) > 0.0f)
		{
			Mesh.Indices.insert(Mesh.Indices.end(), { A, B, C });
		}
		else
		{
			Mesh.Indices.insert(Mesh.Indices.end(), { A, C, B });
		}
	};

	const uint32 Stride = Segments + 1;
	for (uint32 Ring = 0; Ring < Rings; ++Ring)
	{
		for (uint32 Segment = 0; Segment < Segments; ++Segment)
		{
			const uint32 TopLeft     = Ring * Stride + Segment;
			const uint32 TopRight    = TopLeft + 1;
			const uint32 BottomLeft  = TopLeft + Stride;
			const uint32 BottomRight = BottomLeft + 1;
			AddTriangle(TopLeft, TopRight, BottomRight);
			AddTriangle(TopLeft, BottomRight, BottomLeft);
		}
	}

	Mesh.ComputeTangents();
	return Mesh;
}
