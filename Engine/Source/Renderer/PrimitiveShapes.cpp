#include "Renderer/PrimitiveShapes.h"

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

	return Mesh;
}
