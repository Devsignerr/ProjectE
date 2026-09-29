#include "Core/Paths.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/AssetCache.h"
#include "Renderer/GltfLoader.h"
#include "Renderer/Material.h"
#include "Renderer/MeshData.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/ShaderTypes.h"

#include <filesystem>

namespace
{
	constexpr float Tol = 1.0e-4f;

	// 정점의 탄젠트가 규약을 만족하는지: 법선과 직교, 단위 길이, w = ±1
	bool IsValidTangent(const FVertex& Vertex)
	{
		const FVector3 T(Vertex.Tangent.X, Vertex.Tangent.Y, Vertex.Tangent.Z);
		return T.IsNormalized(1.0e-3f) && FMath::Abs(FVector3::Dot(T, Vertex.Normal)) < 1.0e-3f &&
		       (Vertex.Tangent.W == 1.0f || Vertex.Tangent.W == -1.0f);
	}

	FVector3 Bitangent(const FVertex& Vertex)
	{
		const FVector3 T(Vertex.Tangent.X, Vertex.Tangent.Y, Vertex.Tangent.Z);
		return FVector3::Cross(Vertex.Normal, T) * Vertex.Tangent.W;
	}
} // namespace

// 셰이더 cbuffer Material과 같은 배치: BaseColor(0) Emissive(16) Metallic(28) Roughness(32) ... AlphaCutoff(44)
static_assert(sizeof(FMaterialConstants) == 48);
static_assert(offsetof(FMaterialConstants, EmissiveFactor) == 16);
static_assert(offsetof(FMaterialConstants, Metallic) == 28);
static_assert(offsetof(FMaterialConstants, Roughness) == 32);
static_assert(offsetof(FMaterialConstants, AlphaCutoff) == 44);
static_assert(sizeof(FPerFrameConstants) % 16 == 0);
static_assert(MaterialSlot_Count == 5);

E_TEST(Tangent_FlatQuadPointsAlongU)
{
	// 엔진 +Z를 향하는 사각형. UV 원점 왼쪽 위: +U = +Y(오른쪽), +V = -X(아래쪽 = 뒤쪽)
	FMeshData Quad;
	const FVector3 Positions[4] = { { 0, 0, 0 }, { 0, 1, 0 }, { -1, 1, 0 }, { -1, 0, 0 } };
	const FVector2 UVs[4]       = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FVertex& Vertex = Quad.Vertices.emplace_back();
		Vertex.Position = Positions[Index];
		Vertex.Normal   = FVector3::UpVector;
		Vertex.UV       = UVs[Index];
	}
	Quad.Indices = { 0, 1, 2, 0, 2, 3 };
	Quad.ComputeTangents();

	for (const FVertex& Vertex : Quad.Vertices)
	{
		E_EXPECT_TRUE(IsValidTangent(Vertex));
		// 탄젠트 = +U 방향 = +Y
		E_EXPECT_EQUALS(FVector3(Vertex.Tangent.X, Vertex.Tangent.Y, Vertex.Tangent.Z), FVector3::RightVector, Tol);
		// 바이탄젠트 = 텍스처 위쪽 = -V 방향 = +X
		E_EXPECT_EQUALS(Bitangent(Vertex), FVector3::ForwardVector, Tol);
	}

	// UV를 좌우 반전하면 바이탄젠트 부호(w)가 반대가 되어야 한다 (미러링된 UV 섬)
	FMeshData Mirrored = Quad;
	for (FVertex& Vertex : Mirrored.Vertices)
	{
		Vertex.UV.X = 1.0f - Vertex.UV.X;
	}
	Mirrored.ComputeTangents();
	E_EXPECT_TRUE(Mirrored.Vertices[0].Tangent.W == -Quad.Vertices[0].Tangent.W);
	E_EXPECT_EQUALS(Bitangent(Mirrored.Vertices[0]), FVector3::ForwardVector, Tol); // 텍스처 위쪽은 그대로

	// UV가 퇴화하면 법선에 수직인 임의 탄젠트
	FMeshData Degenerate = Quad;
	for (FVertex& Vertex : Degenerate.Vertices)
	{
		Vertex.UV = FVector2::ZeroVector;
	}
	Degenerate.ComputeTangents();
	E_EXPECT_TRUE(IsValidTangent(Degenerate.Vertices[0]));
}

E_TEST(Tangent_CubeFacesAreConsistent)
{
	const FMeshData Cube = FPrimitiveShapes::MakeCube(1.0f);
	for (size_t Index = 0; Index + 2 < Cube.Indices.size(); Index += 3)
	{
		const FVertex& V0 = Cube.Vertices[Cube.Indices[Index + 0]];
		const FVertex& V1 = Cube.Vertices[Cube.Indices[Index + 1]];
		const FVertex& V2 = Cube.Vertices[Cube.Indices[Index + 2]];
		E_EXPECT_TRUE(IsValidTangent(V0));

		// 삼각형의 실제 dP/du, -dP/dv와 저장된 탄젠트/바이탄젠트 방향 일치
		const FTangentBasis Basis = ComputeTriangleTangentBasis(V0.Position, V1.Position, V2.Position, V0.UV, V1.UV, V2.UV);
		E_EXPECT_TRUE(Basis.bValid);
		E_EXPECT_TRUE(FVector3::Dot(FVector3(V0.Tangent.X, V0.Tangent.Y, V0.Tangent.Z), Basis.Tangent) > 0.0f);
		E_EXPECT_TRUE(FVector3::Dot(Bitangent(V0), Basis.Bitangent) > 0.0f);
	}
}

E_TEST(Tangent_GltfConversionMatchesEngineComputation)
{
	// glTF 공간(오른손 Y-up)의 삼각형 → glTF 규약 탄젠트를 계산해 ConvertTangent로 변환한 결과가,
	// 같은 삼각형을 엔진 공간으로 옮긴 뒤 엔진에서 직접 계산한 탄젠트와 같아야 한다.
	const FVector3 GlPositions[3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.2f, 0.0f }, { 0.3f, 1.0f, 0.1f } };
	const FVector2 UVs[3]         = { { 0.1f, 0.9f }, { 0.8f, 0.7f }, { 0.2f, 0.1f } };

	const FVector3 GlNormal = FVector3::Cross(GlPositions[1] - GlPositions[0], GlPositions[2] - GlPositions[0]).GetNormalized();
	const FTangentBasis GlBasis   = ComputeTriangleTangentBasis(GlPositions[0], GlPositions[1], GlPositions[2], UVs[0], UVs[1], UVs[2]);
	const FVector4      GlTangent = OrthonormalizeTangent(GlNormal, GlBasis.Tangent, GlBasis.Bitangent);

	FVector3 EnginePositions[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		EnginePositions[Index] = FGltfLoader::ConvertPosition(GlPositions[Index]);
	}
	const FVector3      EngineNormal = FGltfLoader::ConvertPosition(GlNormal);
	const FTangentBasis EngineBasis  = ComputeTriangleTangentBasis(EnginePositions[0], EnginePositions[1], EnginePositions[2], UVs[0], UVs[1], UVs[2]);
	const FVector4      Expected     = OrthonormalizeTangent(EngineNormal, EngineBasis.Tangent, EngineBasis.Bitangent);

	const FVector4 Converted = FGltfLoader::ConvertTangent(GlTangent);
	E_EXPECT_EQUALS(Converted, Expected, 1.0e-3f);
	// 반사 변환이므로 부호가 뒤집힌다
	E_EXPECT_TRUE(Converted.W == -GlTangent.W);
}

E_TEST(Gltf_DamagedHelmetHasPbrTexturesAndTangents)
{
	if (!FPaths::HasProject())
	{
		return;
	}
	const std::filesystem::path Path = FPaths::GetProjectContentDirectory() / L"DamagedHelmet.glb";
	if (!std::filesystem::exists(Path))
	{
		return;
	}

	FModelData Model;
	E_EXPECT_TRUE(FGltfLoader::Load(Path, Model));
	E_EXPECT_TRUE(!Model.Materials.empty());
	if (Model.Materials.empty())
	{
		return;
	}
	const FModelMaterial& Material = Model.Materials[0];
	// DamagedHelmet: 베이스/금속거칠기/노멀/AO/발광 텍스처 5종 모두 있음
	E_EXPECT_TRUE(Material.BaseColorImage >= 0);
	E_EXPECT_TRUE(Material.MetallicRoughnessImage >= 0);
	E_EXPECT_TRUE(Material.NormalImage >= 0);
	E_EXPECT_TRUE(Material.OcclusionImage >= 0);
	E_EXPECT_TRUE(Material.EmissiveImage >= 0);
	E_EXPECT_TRUE(Material.EmissiveFactor.X > 0.0f);

	size_t Checked = 0;
	for (const FModelMesh& Mesh : Model.Meshes)
	{
		for (size_t Index = 0; Index < Mesh.Data.Vertices.size(); Index += 1009, ++Checked)
		{
			const FVertex& Vertex = Mesh.Data.Vertices[Index];
			const FVector3 T(Vertex.Tangent.X, Vertex.Tangent.Y, Vertex.Tangent.Z);
			E_EXPECT_TRUE(T.IsNormalized(1.0e-2f) && FMath::Abs(Vertex.Tangent.W) == 1.0f);
			E_EXPECT_TRUE(FMath::Abs(FVector3::Dot(T, Vertex.Normal)) < 0.1f);
		}
	}
	E_EXPECT_TRUE(Checked > 0);
}

E_TEST(AssetCache_OldModelVersionIsRebuilt)
{
	if (!FPaths::HasProject())
	{
		return;
	}
	const std::filesystem::path Source = FPaths::GetProjectContentDirectory() / L"DamagedHelmet.glb";
	if (!std::filesystem::exists(Source))
	{
		return;
	}
	const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(Source, FAssetCache::ModelExtension);

	// 이전 버전(1) 헤더만 가진 쿠킹 파일을 원본보다 새 시각으로 기록 → 버전 불일치로 재변환되어야 한다
	FBinaryWriter Writer;
	Writer.Write(FAssetCache::ModelMagic);
	Writer.Write(static_cast<uint32>(1));
	E_EXPECT_TRUE(Writer.SaveToFile(CookedPath));
	std::filesystem::last_write_time(CookedPath, std::filesystem::last_write_time(Source) + std::chrono::seconds(10));

	FModelData Model;
	E_EXPECT_TRUE(FAssetCache::LoadModelAsset(Source, Model) == FAssetCache::ESource::Converted);
	E_EXPECT_TRUE(!Model.Meshes.empty());

	// 새로 기록된 쿠킹본은 현재 버전으로 읽힌다
	FModelData Again;
	E_EXPECT_TRUE(FAssetCache::LoadModelAsset(Source, Again) == FAssetCache::ESource::Cooked);
	E_EXPECT_TRUE(!Again.Materials.empty() && Again.Materials[0].NormalImage == Model.Materials[0].NormalImage);
}
