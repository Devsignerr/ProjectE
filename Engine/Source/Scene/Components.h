#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"
#include "Scene/ResourceHandles.h"

#include <memory>
#include <string>
#include <vector>

struct FModelMetadata;

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

	// FSceneAssetResolver가 핸들을 만든 경로 (리플렉션/직렬화 제외 캐시) — 경로가 바뀌면(스크립트, 복제) 다시 해석한다
	std::string ResolvedMeshAsset;
	std::string ResolvedMaterialAsset;
};

// glTF 모델 인스턴스의 루트. 자식 노드 엔티티는 로드 시 생성되며(FTransientComponent) 직렬화되지 않는다.
// 모델 인스턴스 런타임 (직렬화/리플렉션 제외, FModelLoader가 채운다): 소켓 해석용
struct FModelRuntime
{
	std::shared_ptr<const FModelMetadata> Metadata;     // 에셋 캐시와 공유 (.emeta)
	std::vector<FEntity>                  NodeEntities; // 모델 노드 인덱스 → 엔티티
};

struct FModelComponent
{
	std::string AssetPath; // 프로젝트 Content 기준 상대 경로 (Content 밖이면 절대 경로)

	FModelRuntime Runtime;
};

// 소켓 부착 (언리얼 AttachToComponent + 소켓). Target 모델의 소켓(뼈 기준 지점)을 부모 삼아 따라간다.
//   이 엔티티의 트랜스폼은 소켓 기준 로컬 값이 된다 (계층 부모는 무시). 소켓을 찾지 못하면 평소처럼 계층을 따른다
struct FSocketAttachmentComponent
{
	FEntity     Target; // FModelComponent가 있는 모델 루트
	std::string Socket;
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

// 카메라. 시점은 트랜스폼의 월드 위치/회전(+X 앞). 플레이 모드와 런타임은 bPrimary인 첫 카메라로 렌더한다
struct FCameraComponent
{
	float FovYDegrees   = 60.0f;
	float NearZ         = 10.0f;     // cm
	float FarZ          = 100000.0f; // cm
	bool  bPrimary      = true;
	int32 Priority      = 0;         // 주 카메라가 여럿이면 큰 값이 이긴다 (같으면 먼저 찾은 것) — 플레이어별 로컬 카메라 등
	bool  bOrthographic = false;     // 직교 투영 (픽셀 아트 텍셀 스냅은 직교에서만 정확)
	float OrthoHeight   = 1000.0f;   // 직교일 때 화면 세로가 담는 월드 높이 (cm)
};

// 픽셀 아트 렌더링 설정 (씬 전역 — 씬에서 처음 찾은 활성 컴포넌트 하나만 쓴다).
// 씬을 (출력 ÷ PixelSize) 해상도로 렌더하고 최근접 정수 배율로 확대한다. 직교 카메라는 도트 격자에 스냅 + 서브픽셀 보정.
struct FPixelArtComponent
{
	bool  bEnabled          = true;
	int32 PixelSize         = 4;      // 도트 하나 = 화면 픽셀 N×N (정수 배율)
	bool  bSnapCamera       = true;   // 직교 카메라 위치를 도트 격자에 맞춤 (이동 시 도트 반짝임 제거)
	float OutlineStrength   = 0.6f;   // 깊이 경계(실루엣) 1px 외곽선 어둡기 (0 = 끔)
	float HighlightStrength = 0.35f;  // 볼록 모서리 1px 밝기 (0 = 끔)
	float DepthThreshold    = 25.0f;  // 외곽선 판정 깊이 차 (cm)
	int32 ColorLevels       = 0;      // 채널당 색 단계 수 (0 = 양자화 끔, 2 이상)
	float DitherStrength    = 0.5f;   // 양자화 Bayer 디더 세기 (0~1)
};

// Lua 스크립트 인스턴스. 실행 상태(Lua 테이블)는 FScriptSystem이 엔티티별로 보관하고 여기에는 데이터만 둔다.
//   ScriptAsset:       프로젝트 Content 기준 상대 경로 (예: "Scripts/Rotator.lua")
//   PropertyOverrides: 스크립트 Properties 기본값을 덮어쓰는 JSON 객체 문자열 (인스펙터가 편집, 씬에 저장)
// 스크립트가 도는 곳 (멀티플레이). Standalone(1인용)은 서버이자 클라이언트이므로 모두 돈다
enum class EScriptExecution : int32
{
	ServerOnly = 0, // 게임 로직 (기본). 전용/리슨 서버와 Standalone
	ClientOnly = 1, // 연출/입력/UI. 클라이언트와 리슨 호스트, Standalone
	Both       = 2,
};

struct FScriptComponent
{
	std::string ScriptAsset;
	std::string PropertyOverrides;
	int32       ExecutionLocation = static_cast<int32>(EScriptExecution::ServerOnly); // EScriptExecution
};

// 점광원. 위치는 트랜스폼 월드 위치. 색은 sRGB로 저장하고 렌더러가 선형으로 바꿔 계산한다.
//   Intensity: 1m(100cm) 거리에서의 밝기 (방향광 Intensity와 같은 단위), 거리 제곱에 반비례 + Radius에서 0으로 감쇠
//   감쇠/원뿔 식은 Renderer/LightMath.h (셰이더 Lighting.hlsli와 같은 식)
struct FPointLightComponent
{
	FVector3 Color        = FVector3::OneVector;
	float    Intensity    = 10.0f;
	float    Radius       = 1000.0f; // cm: 영향 반경 (이 거리에서 0)
	bool     bCastShadows = false;   // 큐브 그림자 (6면)
};

// 스포트라이트. 방향은 트랜스폼의 Forward(+X) 축. 내부 원뿔 안은 최대 밝기, 외부 원뿔 밖은 0
struct FSpotLightComponent
{
	FVector3 Color          = FVector3::OneVector;
	float    Intensity      = 20.0f;
	float    Radius         = 2000.0f; // cm
	float    InnerConeAngle = 20.0f;   // 도 (중심축과의 반각)
	float    OuterConeAngle = 35.0f;   // 도 (반각, 최대 80)
	bool     bCastShadows   = false;   // 그림자 맵 1장
};

// 하늘광 (씬 전역 — 처음 찾은 것 하나만): 환경광(IBL)과 하늘 배경 밝기 배율. 없으면 1. 밤/실내 씬은 낮춘다
struct FSkyLightComponent
{
	float Intensity = 1.0f;
};
