#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"
#include "Scene/AnimRootMotion.h"
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
//   bRootMotion: (이전 설정) 켜면 RootMotionMode = All과 같다
//   RootMotionMode/RootMotionBone/bRootMotionRotation: 루트 모션 (Scene/AnimRootMotion.h)
//   RetargetSources: 클립을 리타기팅해 덧붙일 다른 모델 경로 (Content 기준, ';' 또는 ',' 구분 — Scene/AnimRetarget.h)
struct FAnimationComponent
{
	std::string     Clip;
	float           Speed       = 1.0f;
	float           BlendTime   = 0.25f; // 초
	bool            bPlaying    = true;
	bool            bLoop       = true;
	bool            bRootMotion = false;
	ERootMotionMode RootMotionMode = ERootMotionMode::None;
	std::string     RootMotionBone;           // 비면 클립마다 자동 (최상위 이동 채널 노드)
	bool            bRootMotionRotation = false; // 루트의 Z축 회전도 추출
	std::string     RetargetSources;

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
	bool  bSnapMovingObjects = true;  // 한 번이라도 움직인 최상위 물체(하위 트리 통째로)를 도트 격자에 맞춰 그림 (직교, 렌더만)
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
//   EnvironmentMap(Phase 33-7): Content 기준 등장방형 HDR(.hdr) → 하늘 배경 + IBL (비우면 절차적 하늘). 회전 = Z축(도, +면 오른쪽으로)
struct FSkyLightComponent
{
	float       Intensity = 1.0f;
	std::string EnvironmentMap;
	float       EnvironmentRotation = 0.0f;
};

// 높이 지수 안개 + 볼류메트릭 안개 (씬 전역 — 처음 찾은 것 하나만, 식은 Renderer/FogMath.h).
//   기준 높이 = 엔티티 월드 Z. 밀도(z) = Density · exp(-HeightFalloff · (z - 기준 높이)) (1/m 단위로 저장, 렌더러가 cm로 바꾼다)
//   색은 선형 HDR (방향광 색과 같은 규약). 볼류메트릭은 카메라 앞 VolumetricDistance까지 3D 격자로 빛 산란(방향광 그림자/로컬 라이트)을
//   계산하고, 그 뒤는 해석식 안개가 이어 받는다. 불투명 메시·하늘(전체 화면 적용)과 파티클(정점에서 계산)에 적용된다
struct FHeightFogComponent
{
	FVector3 Color                       = FVector3(0.45f, 0.55f, 0.7f); // 안개 산란 색 (선형)
	float    Density                     = 0.02f;  // 1/m (기준 높이에서)
	float    HeightFalloff               = 0.2f;   // 1/m (클수록 위로 갈수록 빨리 옅어짐)
	float    StartDistance               = 0.0f;   // cm
	float    MaxOpacity                  = 1.0f;
	FVector3 DirectionalInscatteringColor = FVector3(0.35f, 0.3f, 0.2f); // 태양 쪽을 볼 때 더하는 색 (선형)
	float    DirectionalInscatteringExponent      = 8.0f;
	float    DirectionalInscatteringStartDistance = 1000.0f; // cm
	bool     bVolumetric                 = false;
	float    VolumetricDistance          = 6000.0f; // cm
	FVector3 VolumetricAlbedo            = FVector3::OneVector; // 산란 비율 색 (1 = 흡수 없음)
	float    VolumetricExtinctionScale   = 1.0f;
	float    VolumetricAnisotropy        = 0.2f;    // 헤니-그린스타인 g (0 = 고르게, + = 빛 진행 방향으로)
	float    VolumetricDirectionalScale  = 1.0f;    // 방향광 산란 배율
	float    VolumetricLocalLightScale   = 1.0f;    // 점광원/스포트 산란 배율
};

// 반사 캡처 (식은 Renderer/ReflectionMath.h): 위치에서 본 장면을 큐브맵으로 구워(에디터 도구 → 반사 캡처 굽기, --bake-captures)
//   CaptureAsset 파일(.ecapture, 프리필터 밉 포함)로 저장하고, 영역 안 표면의 반사(IBL 반사)에 쓴다. 우선순위: SSR → 캡처 → 하늘
//   Shape 0 = 구(Radius), 1 = 상자(BoxExtent 반 크기, 월드 축 정렬 — 회전 무시, 시차 보정). 경계 안쪽 FadeDistance에서 섞인다
struct FReflectionCaptureComponent
{
	int32       Shape        = 1;
	float       Radius       = 800.0f;                         // cm (구)
	FVector3    BoxExtent    = FVector3(500.0f, 500.0f, 300.0f); // cm (상자 반 크기)
	float       FadeDistance = 100.0f;                         // cm
	float       Intensity    = 1.0f;
	int32       Priority     = 0;  // 겹치면 큰 값이 먼저 (같으면 작은 영역이 먼저)
	std::string CaptureAsset;      // Content 기준 .ecapture (비어 있으면 굽기가 Captures/<이름>.ecapture로 정한다)
};

// 박스 투영 데칼 (깊이 사전 패스 뒤 DBuffer에 그려 메인 패스가 베이스색/노멀/거칠기에 섞는다, 식은 Renderer/DecalMath.h).
//   상자 = 엔티티 로컬 [-Size/2, Size/2]이고 로컬 -Z 방향으로 찍힌다 (회전 없으면 바닥). U = 로컬 +Y, 텍스처 위 = 로컬 +X
//   머티리얼(.emat): 베이스색 텍스처 × BaseColorFactor (알파 = 불투명도), 노멀 맵, 금속/거칠기. SortOrder가 큰 데칼이 위에 그려진다
struct FDecalComponent
{
	std::string MaterialAsset;
	FVector3    Size              = FVector3(200.0f, 200.0f, 100.0f); // cm (X, Y = 찍히는 면, Z = 투영 깊이)
	float       Opacity           = 1.0f;
	int32       SortOrder         = 0;
	bool        bAffectBaseColor  = true;
	bool        bAffectNormal     = true;
	bool        bAffectRoughness  = true;
	float       FadeStartDistance = 0.0f; // cm, 카메라 거리 페이드 (End <= Start면 없음)
	float       FadeEndDistance   = 0.0f;

	// 런타임 (렌더러가 MaterialAsset에서 해석, 직렬화 제외)
	FMaterialHandle Material;
	std::string     ResolvedMaterialAsset;
};
