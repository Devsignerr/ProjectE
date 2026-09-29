#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"
#include "Scene/ResourceHandles.h"

#include <string>
#include <vector>

// 표시용 이름
struct FNameComponent
{
	std::string Name;
};

// 로컬 트랜스폼 + 캐시된 월드 행렬 (FScene::UpdateTransforms가 계층 순서로 갱신)
struct FTransformComponent
{
	FVector3 Position;
	FQuat    Rotation;
	FVector3 Scale = FVector3::OneVector;

	FMatrix4x4 WorldMatrix;

	FMatrix4x4 GetLocalMatrix() const { return FMatrix4x4::MakeTransform(Position, Rotation, Scale); }
	FVector3   GetWorldPosition() const { return WorldMatrix.GetOrigin(); }
	FVector3   GetWorldForward() const { return WorldMatrix.GetAxisX().GetNormalized(); }
};

// 부모/자식 관계. FScene::SetParent로만 변경한다.
struct FHierarchyComponent
{
	FEntity              Parent;
	std::vector<FEntity> Children;
};

// 정적 메시. 런타임 핸들(Mesh/Material)은 직렬화되지 않고, 에셋 참조 문자열에서 로드 후 복원된다.
//   MeshAsset:     "primitive:cube" 같은 내장 도형 이름 (파일 메시 참조는 에셋 파이프라인에서 확장)
//   MaterialAsset: 프로젝트 Content 기준 상대 경로의 .emat 파일. 비어 있으면 기본 머티리얼
struct FStaticMeshComponent
{
	FMeshHandle     Mesh;
	FMaterialHandle Material;
	bool            bVisible = true;
	std::string     MeshAsset;
	std::string     MaterialAsset;
};

// glTF 모델 인스턴스의 루트. 자식 노드 엔티티는 로드 시 생성되며(FTransientComponent) 직렬화되지 않는다.
struct FModelComponent
{
	std::string AssetPath; // 프로젝트 Content 기준 상대 경로 (Content 밖이면 절대 경로)
};

// 파생/생성된 엔티티 표식: 직렬화에서 제외 (모델 자식 노드 등)
struct FTransientComponent
{
	uint8 Unused = 0;
};

// 방향광. 방향은 트랜스폼의 Forward(+X) 축
struct FDirectionalLightComponent
{
	FVector3 Color     = FVector3::OneVector;
	float    Intensity = 1.0f;
};

// 스킨 메시 바인딩 (모델 인스턴스화 시 생성되는 런타임 전용, 직렬화/리플렉션 제외).
// 같은 엔티티의 FStaticMeshComponent 메시가 스킨 정점 스트림을 가지면 GPU 스키닝으로 그린다.
// 본 행렬 = InverseBindMatrices[i] * Joints[i]의 월드 행렬 (스킨 메시 노드 자신의 트랜스폼은 무시 — glTF 규약)
struct FSkinComponent
{
	std::vector<FEntity>    Joints;
	std::vector<FMatrix4x4> InverseBindMatrices;
};

// 모델 루트의 애니메이션 재생 상태. 로직은 FAnimationSystem (Scene/AnimationSystem.h)
//   Clip을 바꾸면 BlendTime 동안 이전 클립에서 크로스페이드한다. 비어 있으면 첫 클립
//   bRootMotion: 루트 본의 수평 이동을 엔티티 트랜스폼으로 옮긴다 (본은 제자리)
struct FAnimationComponent
{
	std::string Clip;
	float       Speed       = 1.0f;
	float       BlendTime   = 0.25f; // 초
	bool        bPlaying    = true;
	bool        bLoop       = true;
	bool        bRootMotion = false;

	FAnimationRuntime Runtime; // 직렬화 제외 (모델 로드 시 채워짐)
};
