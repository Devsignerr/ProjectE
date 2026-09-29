#include "Core/StringConv.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/GltfLoader.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	constexpr float Tol = 1.0e-4f;

	std::string Base64Encode(const std::vector<uint8>& Bytes)
	{
		static const char* Table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string        Result;
		size_t             Index = 0;
		while (Index + 2 < Bytes.size())
		{
			const uint32 Triple = (Bytes[Index] << 16) | (Bytes[Index + 1] << 8) | Bytes[Index + 2];
			Result += Table[(Triple >> 18) & 63];
			Result += Table[(Triple >> 12) & 63];
			Result += Table[(Triple >> 6) & 63];
			Result += Table[Triple & 63];
			Index += 3;
		}
		if (Index < Bytes.size())
		{
			uint32 Triple = Bytes[Index] << 16;
			if (Index + 1 < Bytes.size()) Triple |= Bytes[Index + 1] << 8;
			Result += Table[(Triple >> 18) & 63];
			Result += Table[(Triple >> 12) & 63];
			Result += (Index + 1 < Bytes.size()) ? Table[(Triple >> 6) & 63] : '=';
			Result += '=';
		}
		return Result;
	}

	template <typename T>
	void Append(std::vector<uint8>& Bytes, const T& Value)
	{
		const size_t Offset = Bytes.size();
		Bytes.resize(Offset + sizeof(T));
		std::memcpy(&Bytes[Offset], &Value, sizeof(T));
	}

	// 삼각형 하나짜리 glTF를 임시 파일로 작성 (위치/법선/UV/인덱스 + 노드 계층 + 머티리얼)
	std::filesystem::path WriteTestGltf()
	{
		std::vector<uint8> Buffer;
		const float Positions[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
		const float Normals[9]   = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
		const float UVs[6]       = { 0, 0, 1, 0, 0, 1 };
		const uint16 Indices[3]  = { 0, 1, 2 };
		for (float V : Positions) Append(Buffer, V);
		for (float V : Normals) Append(Buffer, V);
		for (float V : UVs) Append(Buffer, V);
		for (uint16 V : Indices) Append(Buffer, V);
		Append<uint16>(Buffer, 0); // 4바이트 정렬 패딩

		const std::string Json = std::format(R"({{
"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],
"nodes":[{{"name":"Root","translation":[1,2,3],"rotation":[0,0.7071068,0,0.7071068],"children":[1]}},{{"name":"Tri","mesh":0,"scale":[2,3,4]}}],
"meshes":[{{"name":"TriMesh","primitives":[{{"attributes":{{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2}},"indices":3,"material":0}}]}}],
"materials":[{{"name":"Mat","pbrMetallicRoughness":{{"baseColorFactor":[0.5,0.25,1,1],"metallicFactor":0,"roughnessFactor":0.5}}}}],
"accessors":[
 {{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}},
 {{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"}},
 {{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"}},
 {{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}}],
"bufferViews":[
 {{"buffer":0,"byteOffset":0,"byteLength":36}},{{"buffer":0,"byteOffset":36,"byteLength":36}},
 {{"buffer":0,"byteOffset":72,"byteLength":24}},{{"buffer":0,"byteOffset":96,"byteLength":6}}],
"buffers":[{{"byteLength":{},"uri":"data:application/octet-stream;base64,{}"}}]
}})", Buffer.size(), Base64Encode(Buffer));

		const std::filesystem::path Path = std::filesystem::temp_directory_path() / L"ProjectE_테스트_Triangle.gltf";
		std::ofstream File(Path, std::ios::binary);
		File << Json;
		return Path;
	}
} // namespace

E_TEST(Gltf_AxisConversion)
{
	// glTF 오른쪽(+X) → 엔진 오른쪽(+Y), 위(+Y) → 위(+Z), 앞(-Z) → 앞(+X)
	E_EXPECT_EQUALS(FGltfLoader::ConvertPosition(FVector3(1, 0, 0)), FVector3::RightVector, Tol);
	E_EXPECT_EQUALS(FGltfLoader::ConvertPosition(FVector3(0, 1, 0)), FVector3::UpVector, Tol);
	E_EXPECT_EQUALS(FGltfLoader::ConvertPosition(FVector3(0, 0, -1)), FVector3::ForwardVector, Tol);
	E_EXPECT_EQUALS(FGltfLoader::ConvertScale(FVector3(1, 2, 3)), FVector3(3, 1, 2), Tol);
}

E_TEST(Gltf_RotationConversionMatchesMatrixConjugation)
{
	// 쿼터니언 변환은 행렬 켤레 변환(C^T R C)과 같아야 한다
	const FQuat Samples[] = {
		FQuat::FromAxisAngle(FVector3(0, 1, 0), 0.5f * FMath::Pi),
		FQuat::FromAxisAngle(FVector3(1, 0, 0), -1.2f),
		FQuat::FromAxisAngle(FVector3(0.3f, 0.5f, -0.8f).GetNormalized(), 2.4f),
		FQuat::FromEuler(20.0f, -70.0f, 130.0f),
	};
	for (const FQuat& GltfQuat : Samples)
	{
		const FMatrix4x4 Expected = FGltfLoader::ConvertMatrix(FMatrix4x4::MakeRotation(GltfQuat));
		const FMatrix4x4 Actual   = FMatrix4x4::MakeRotation(FGltfLoader::ConvertRotation(GltfQuat));
		E_EXPECT_EQUALS(Actual, Expected, 1.0e-3f);
	}

	// 변환된 회전은 정점 변환과도 일치: Rot_e(Conv(p)) == Conv(Rot_gl(p))
	const FQuat    GltfQuat = FQuat::FromAxisAngle(FVector3(0, 1, 0), 0.5f * FMath::Pi);
	const FVector3 P(1.0f, 2.0f, 3.0f);
	E_EXPECT_EQUALS(FGltfLoader::ConvertRotation(GltfQuat).RotateVector(FGltfLoader::ConvertPosition(P)),
	                FGltfLoader::ConvertPosition(GltfQuat.RotateVector(P)), 1.0e-3f);
}

E_TEST(Gltf_LoadTriangleFile)
{
	const std::filesystem::path Path = WriteTestGltf();

	FModelData Model;
	const bool bLoaded = FGltfLoader::Load(Path, Model);
	std::filesystem::remove(Path);
	E_EXPECT_TRUE(bLoaded);
	if (!bLoaded)
	{
		return;
	}

	E_EXPECT_EQ(Model.Meshes.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Model.Nodes.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Model.Materials.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Model.RootNodes.size(), static_cast<size_t>(1));

	const FModelMesh& Mesh = Model.Meshes[0];
	E_EXPECT_EQ(Mesh.Data.Vertices.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Mesh.Data.Indices.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Mesh.Material, 0);
	// glTF (1,0,0) → 엔진 (0,1,0), 법선 (0,0,1) → (-1,0,0)
	E_EXPECT_EQUALS(Mesh.Data.Vertices[1].Position, FVector3(0, 1, 0), Tol);
	E_EXPECT_EQUALS(Mesh.Data.Vertices[2].Position, FVector3(0, 0, 1), Tol);
	E_EXPECT_EQUALS(Mesh.Data.Vertices[0].Normal, FVector3(-1, 0, 0), Tol);
	E_EXPECT_EQUALS(Mesh.Data.Vertices[1].UV, FVector2(1, 0), Tol);

	// 와인딩: 엔진 규약(CW 앞면)에서는 Cross(E1, E2)가 법선과 같은 방향 (FPrimitiveShapes 큐브와 동일)
	{
		const FVector3& P0 = Mesh.Data.Vertices[Mesh.Data.Indices[0]].Position;
		const FVector3& P1 = Mesh.Data.Vertices[Mesh.Data.Indices[1]].Position;
		const FVector3& P2 = Mesh.Data.Vertices[Mesh.Data.Indices[2]].Position;
		const FVector3  GeometricNormal = FVector3::Cross(P1 - P0, P2 - P0);
		E_EXPECT_TRUE(FVector3::Dot(GeometricNormal, Mesh.Data.Vertices[0].Normal) > 0.0f);
	}

	// 노드: Root 이동 (1,2,3) → (-3,1,2), 자식 Tri에 메시, 스케일 (2,3,4) → (4,2,3)
	const FModelNode& Root = Model.Nodes[Model.RootNodes[0]];
	E_EXPECT_TRUE(Root.Name == "Root");
	E_EXPECT_EQUALS(Root.Translation, FVector3(-3, 1, 2), Tol);
	E_EXPECT_EQ(Root.Children.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Root.Meshes.empty());
	// glTF Y축 90° → 엔진 Z축(위) 회전, 반사로 각도 반전
	const FQuat ExpectedRotation = FGltfLoader::ConvertRotation(FQuat(0.0f, 0.7071068f, 0.0f, 0.7071068f));
	E_EXPECT_EQUALS(Root.Rotation, ExpectedRotation, 1.0e-3f);

	const FModelNode& Tri = Model.Nodes[Root.Children[0]];
	E_EXPECT_EQ(Tri.Parent, Model.RootNodes[0]);
	E_EXPECT_EQ(Tri.Meshes.size(), static_cast<size_t>(1));
	E_EXPECT_EQUALS(Tri.Scale, FVector3(4, 2, 3), Tol);

	// 머티리얼
	E_EXPECT_EQUALS(Model.Materials[0].BaseColorFactor, FVector4(0.5f, 0.25f, 1.0f, 1.0f), Tol);
	E_EXPECT_NEAR(Model.Materials[0].RoughnessFactor, 0.5f, Tol);
	E_EXPECT_EQ(Model.Materials[0].BaseColorImage, -1);
}

// 실제 샘플 에셋 (Sandbox/Assets/DamagedHelmet.glb, Khronos CC-BY 4.0). 파일이 없으면 건너뛴다.
E_TEST(Gltf_LoadDamagedHelmetIfPresent)
{
	const std::filesystem::path Path = std::filesystem::path(E_TEST_ASSET_DIR) / L"DamagedHelmet.glb";
	if (!std::filesystem::exists(Path))
	{
		E_LOG(LogCore, Warning, "DamagedHelmet.glb 없음 — 테스트 건너뜀");
		return;
	}

	FModelData Model;
	E_EXPECT_TRUE(FGltfLoader::Load(Path, Model));
	E_EXPECT_TRUE(!Model.Meshes.empty());
	E_EXPECT_TRUE(!Model.Nodes.empty());
	E_EXPECT_TRUE(!Model.RootNodes.empty());
	E_EXPECT_TRUE(!Model.Materials.empty());
	E_EXPECT_TRUE(Model.Materials[0].BaseColorImage >= 0);

	// 모든 이미지가 디코딩되고, 정점/인덱스가 유효하며 법선이 정규화되어 있어야 한다
	for (const FModelImage& Image : Model.Images)
	{
		E_EXPECT_TRUE(Image.Image.IsValid());
	}
	for (const FModelMesh& Mesh : Model.Meshes)
	{
		E_EXPECT_TRUE(Mesh.Data.Indices.size() % 3 == 0);
		for (uint32 Index : Mesh.Data.Indices)
		{
			E_EXPECT_TRUE(Index < Mesh.Data.Vertices.size());
		}
		for (size_t V = 0; V < Mesh.Data.Vertices.size(); V += 997)
		{
			E_EXPECT_TRUE(Mesh.Data.Vertices[V].Normal.IsNormalized(1.0e-2f));
		}

		// 삼각형 대부분의 기하 법선이 정점 법선과 같은 방향이어야 한다 (와인딩 규약 검증)
		size_t Agree = 0;
		size_t Total = 0;
		for (size_t Index = 0; Index + 2 < Mesh.Data.Indices.size(); Index += 3)
		{
			const FVertex& V0 = Mesh.Data.Vertices[Mesh.Data.Indices[Index + 0]];
			const FVertex& V1 = Mesh.Data.Vertices[Mesh.Data.Indices[Index + 1]];
			const FVertex& V2 = Mesh.Data.Vertices[Mesh.Data.Indices[Index + 2]];
			const FVector3 GeometricNormal = FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position);
			const FVector3 AverageNormal   = V0.Normal + V1.Normal + V2.Normal;
			if (!GeometricNormal.IsNearlyZero(1.0e-12f))
			{
				++Total;
				Agree += FVector3::Dot(GeometricNormal, AverageNormal) > 0.0f ? 1 : 0;
			}
		}
		E_EXPECT_TRUE(Total > 0 && Agree * 10 > Total * 9);
	}
}
