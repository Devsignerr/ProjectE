#pragma once

#include "Core/Math/Math.h"
#include "Renderer/Image.h"
#include "Renderer/MeshData.h"

#include <filesystem>
#include <string>
#include <vector>

// glTF 2.0에서 읽은 중간 모델 데이터 (엔진 좌표계로 변환 완료, GPU 리소스 없음)

struct FModelImage
{
	std::string Name;
	FImage      Image; // 디코딩 실패 시 IsValid() == false
};

struct FModelMaterial
{
	std::string Name;
	FVector4    BaseColorFactor = FVector4::OneVector;
	int32       BaseColorImage  = -1; // FModelData::Images 인덱스
	float       MetallicFactor  = 1.0f;
	float       RoughnessFactor = 1.0f;
};

// glTF primitive 하나 = 메시 하나
struct FModelMesh
{
	std::string Name;
	FMeshData   Data;
	int32       Material = -1; // FModelData::Materials 인덱스
};

struct FModelNode
{
	std::string        Name;
	int32              Parent = -1;
	std::vector<int32> Children;
	FVector3           Translation;
	FQuat              Rotation;
	FVector3           Scale = FVector3::OneVector;
	std::vector<int32> Meshes; // FModelData::Meshes 인덱스 (노드의 glTF 메시가 가진 primitive들)
};

struct FModelData
{
	std::string                 Name;
	std::vector<FModelImage>    Images;
	std::vector<FModelMaterial> Materials;
	std::vector<FModelMesh>     Meshes;
	std::vector<FModelNode>     Nodes;
	std::vector<int32>          RootNodes;
};

// cgltf 기반 glTF/GLB 로더.
// 좌표 변환: glTF(오른손, +Y 위, +Z 앞/시청자 방향, +X 오른쪽) → 엔진(왼손, +Z 위, +X 앞, +Y 오른쪽)
//   엔진 = (-glTF.z, glTF.x, glTF.y). 반사 변환이므로 glTF의 CCW 앞면이 엔진의 CW 앞면이 된다(인덱스 순서 유지).
struct FGltfLoader
{
	static bool Load(const std::filesystem::path& Path, FModelData& OutModel);

	static FVector3   ConvertPosition(const FVector3& Gltf) { return { -Gltf.Z, Gltf.X, Gltf.Y }; }
	static FVector3   ConvertScale(const FVector3& Gltf) { return { Gltf.Z, Gltf.X, Gltf.Y }; }
	static FQuat      ConvertRotation(const FQuat& Gltf);
	static FMatrix4x4 ConvertMatrix(const FMatrix4x4& GltfRowMajor);
};
