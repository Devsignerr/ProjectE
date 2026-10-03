#pragma once

#include "Core/Math/Math.h"
#include "Renderer/Image.h"
#include "Renderer/Material.h"
#include "Renderer/TextureCompression.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkinnedMeshData.h"
#include "Scene/Animation.h"

#include <filesystem>
#include <string>
#include <vector>

// glTF 2.0에서 읽은 중간 모델 데이터 (엔진 좌표계로 변환 완료, GPU 리소스 없음)

struct FModelImage
{
	std::string        Name;
	FImage             Image;   // glTF 디코딩 결과 (실패 시 IsValid() == false). 압축 후 비워진다
	FCompressedTexture Texture; // 쿠킹 텍스처 (FAssetCache::CompressModelImages가 머티리얼 용도에 맞춰 채움)
	// 쿠킹 모델 파일(FModelData::CookedPath) 안 이 텍스처 본문 위치 (밉 스트리밍이 밉 단위로 다시 읽는다). 0 = 없음
	uint64 CookedPayloadOffset = 0;
};

// glTF 2.0 금속/거칠기 머티리얼. *Image는 FModelData::Images 인덱스(-1이면 없음)
struct FModelMaterial
{
	std::string Name;
	FVector4    BaseColorFactor   = FVector4::OneVector;
	FVector3    EmissiveFactor    = FVector3::ZeroVector; // KHR_materials_emissive_strength 반영
	float       MetallicFactor    = 1.0f;
	float       RoughnessFactor   = 1.0f;
	float       NormalScale       = 1.0f;
	float       OcclusionStrength = 1.0f;
	// glTF alphaMode (OPAQUE → Opaque, MASK → Masked, BLEND → Translucent) / alphaCutoff / doubleSided
	EMaterialBlendMode BlendMode   = EMaterialBlendMode::Opaque;
	float              AlphaCutoff = 0.5f;
	bool               bTwoSided   = false;

	int32 BaseColorImage         = -1; // sRGB
	int32 MetallicRoughnessImage = -1; // 선형 (G=거칠기, B=금속)
	int32 NormalImage            = -1; // 선형
	int32 OcclusionImage         = -1; // 선형 (R)
	int32 EmissiveImage          = -1; // sRGB
};

// glTF primitive 하나 = 메시 하나
struct FModelMesh
{
	std::string Name;
	FMeshData   Data;
	int32       Material = -1; // FModelData::Materials 인덱스

	std::vector<FSkinVertex> SkinVertices; // JOINTS_0/WEIGHTS_0 (비어 있으면 정적 메시)
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
	int32              Skin = -1; // FModelData::Skins 인덱스
};

struct FModelData
{
	std::string                 Name;
	std::vector<FModelImage>    Images;
	std::vector<FModelMaterial> Materials;
	std::vector<FModelMesh>     Meshes;
	std::vector<FModelNode>     Nodes;
	std::vector<int32>          RootNodes;
	std::vector<FModelSkin>     Skins;
	std::vector<FAnimationClip> Animations; // 채널 Node = Nodes 인덱스
	// 이 데이터를 읽었거나 기록한 쿠킹 파일 (FAssetCache::LoadModelAsset). 비어 있으면 쿠킹본 없음 → 이미지 밉 스트리밍 안 함
	std::filesystem::path CookedPath;
};

// cgltf 기반 glTF/GLB 로더.
// 좌표 변환: glTF(오른손, +Y 위, +Z 앞/시청자 방향, +X 오른쪽) → 엔진(왼손, +Z 위, +X 앞, +Y 오른쪽)
//   엔진 = (-glTF.z, glTF.x, glTF.y). 반사 변환이지만 엔진 카메라 규약도 반사라 화면상 와인딩은 유지되므로,
//   glTF의 CCW 앞면을 엔진의 CW 앞면으로 맞추기 위해 삼각형 인덱스 순서를 뒤집는다.
struct FGltfLoader
{
	static bool Load(const std::filesystem::path& Path, FModelData& OutModel);

	// 축 변환만 한다 (방향 벡터에도 사용). 위치/이동은 ImportScale을 곱해 엔진 단위(cm)로 바꾼다
	static FVector3   ConvertPosition(const FVector3& Gltf) { return { -Gltf.Z, Gltf.X, Gltf.Y }; }

	// glTF는 미터 → 엔진 센티미터
	static constexpr float ImportScale = FUnits::MetersToUnits;
	static FVector3   ConvertScale(const FVector3& Gltf) { return { Gltf.Z, Gltf.X, Gltf.Y }; }
	static FQuat      ConvertRotation(const FQuat& Gltf);
	// 탄젠트: xyz는 위치처럼 변환, 반사로 Cross(N, T)의 방향이 뒤집히므로 바이탄젠트 부호 w를 반전한다
	static FVector4   ConvertTangent(const FVector4& Gltf);
	static FMatrix4x4 ConvertMatrix(const FMatrix4x4& GltfRowMajor);
};
