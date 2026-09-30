#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/ResourceHandles.h"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

class FScene;

// ============================================================================================================
// 나이아가라식 파티클 시스템
//   시스템(.eparticle) → 이미터 여러 개 → 이미터마다 단계별 모듈 목록(이미터 갱신 / 입자 생성 / 입자 갱신) + 렌더러.
//   모듈 입력은 "동적 입력"(상수 / 무작위 범위 / 곡선). 이미터마다 계산 방식(CPU / GPU)을 고른다.
//   모듈 종류와 입력 순서는 GPU 셰이더(ParticleSimulate.hlsl)와 1:1로 맞춘다 — 번호를 바꾸지 말고 끝에 추가할 것.
//   단위: cm / 초 / 도. 색은 HDR 선형 + 알파.
// ============================================================================================================

enum class EParticleSimTarget : int32
{
	CPU = 0, // 수천 개 이하, 반투명 정렬, 리본
	GPU = 1, // 수만 개 이상 (정렬/리본 없음)
};

enum class EParticleBlendMode : int32
{
	Alpha    = 0, // 반투명 (CPU는 뒤→앞 정렬)
	Additive = 1, // 가산 (불꽃/빛)
};

// ---- 동적 입력 ----------------------------------------------------------------------------------------------

enum class EParticleValueMode : int32
{
	Constant = 0, // A
	Random   = 1, // A~B 사이 무작위 (입자마다 한 번, 성분별로)
	Curve    = 2, // 곡선: 입자 단계는 수명 비율(0~1), 이미터 단계는 주기 안 시간 비율(0~1)
};

struct FParticleCurveKey
{
	float    Time = 0.0f; // 0~1
	FVector4 Value;
};

struct FParticleValue
{
	EParticleValueMode             Mode = EParticleValueMode::Constant;
	FVector4                       A;
	FVector4                       B;
	std::vector<FParticleCurveKey> Curve; // Time 오름차순

	static FParticleValue Constant(const FVector4& Value);
	static FParticleValue Range(const FVector4& Min, const FVector4& Max);
	static FParticleValue MakeCurve(std::vector<FParticleCurveKey> Keys);

	// 평가: Random01 = 성분별 [0,1) 난수, Alpha = 곡선 위치(0~1)
	FVector4 Evaluate(const FVector4& Random01, float Alpha) const;
	FVector4 SampleCurve(float Alpha) const;
};

// ---- 모듈 ---------------------------------------------------------------------------------------------------

enum class EParticleStage : int32
{
	EmitterUpdate = 0,
	ParticleSpawn = 1,
	ParticleUpdate = 2,
	Count
};

// 나이아가라 기본 모듈 이름을 따른다 (Engine/Plugins/FX/Niagara/Content/Modules). 셰이더와 번호 공유
enum class EParticleModuleType : int32
{
	// 이미터 갱신
	SpawnRate = 0,        // 초당 개수
	SpawnBurst = 1,       // 주기 안 특정 시각에 한 번에 (SpawnBurstInstantaneous)
	// 입자 생성
	InitializeParticle = 2, // 수명 / 색 / 크기 / 회전 / 질량
	ShapeLocation = 3,      // 점/구/상자/원기둥/원뿔/원환 안의 시작 위치
	AddVelocity = 4,        // 고정 방향 속도 (로컬)
	AddVelocityInCone = 5,  // 원뿔 안의 방향 속도
	AddVelocityFromPoint = 6, // 한 점에서 바깥으로
	// 입자 갱신
	GravityForce = 7,
	Drag = 8,
	AccelerationForce = 9,
	CurlNoiseForce = 10,      // 부드러운 흐름 노이즈
	VortexForce = 11,         // 축 둘레 회전
	PointAttractionForce = 12, // 한 점으로 끌림
	ScaleColor = 13,          // 시작 색 × 수명 곡선
	ScaleSpriteSize = 14,     // 시작 크기 × 수명 곡선
	SpriteRotationRate = 15,  // 초당 회전 (도)
	SubUVAnimation = 16,      // 플립북 프레임 진행
	Collision = 17,           // 평면(Z = 높이)과 충돌 (튀기기/마찰/죽이기)
	Count
};

enum class EParticleInputKind : int32
{
	Float,
	Vector, // xyz
	Color,  // rgba (HDR)
	Bool,   // x != 0
	Enum,   // x = 정수 (EnumNames)
};

struct FParticleInputInfo
{
	const char*        Id;          // 직렬화 키 (나이아가라 입력 이름에 가깝게)
	const char*        DisplayName; // 한국어 표시 이름
	EParticleInputKind Kind;
	FVector4           Default;
	bool               bAllowRandom = true;
	bool               bAllowCurve  = true;
	const char*        EnumNames    = nullptr; // Kind == Enum일 때 "\0"으로 구분된 목록
	float              Speed        = 1.0f;    // 편집 드래그 증분
};

struct FParticleModuleInfo
{
	EParticleModuleType            Type;
	const char*                    Id;          // "SpawnRate" 등
	const char*                    DisplayName; // "초당 생성"
	EParticleStage                 Stage;
	std::span<const FParticleInputInfo> Inputs;
	const char*                    Description;
};

struct FParticleModule
{
	EParticleModuleType         Type     = EParticleModuleType::SpawnRate;
	bool                        bEnabled = true;
	std::vector<FParticleValue> Inputs; // 모듈 정보의 입력 순서

	static FParticleModule Make(EParticleModuleType Type); // 기본값으로 채움
	const FParticleModuleInfo& GetInfo() const;
	const FParticleValue&      Input(size_t Index) const { return Inputs[Index]; }
};

// 모듈 목록 (종류 → 정보). 인덱스 = EParticleModuleType
std::span<const FParticleModuleInfo> GetParticleModuleInfos();
const FParticleModuleInfo*           FindParticleModuleInfo(std::string_view Id);

// ---- 렌더러 -------------------------------------------------------------------------------------------------

enum class EParticleRendererType : int32
{
	Sprite = 0, // 카메라를 향한 판
	Mesh   = 1, // 입자마다 메시 (크기 X = 균등 배율)
	Ribbon = 2, // 생성 순서로 이은 띠 (CPU 전용)
};

enum class EParticleSpriteAlignment : int32
{
	FaceCamera = 0,
	Velocity   = 1, // 속도 방향으로 늘인다 (불티, 빗줄기)
};

struct FParticleRendererSettings
{
	EParticleRendererType    Type       = EParticleRendererType::Sprite;
	bool                     bEnabled   = true;
	EParticleBlendMode       BlendMode  = EParticleBlendMode::Additive;
	std::string              TexturePath; // 시스템 파일 폴더 기준 상대 경로, 비면 기본 부드러운 원
	int32                    SubImageColumns = 1; // 플립북 칸 수 (가로/세로)
	int32                    SubImageRows    = 1;
	EParticleSpriteAlignment Alignment        = EParticleSpriteAlignment::FaceCamera;
	float                    VelocityStretch  = 0.02f; // 속도 정렬일 때 늘이는 정도 (초)
	std::string              MeshAsset        = "primitive:sphere"; // 메시 렌더러
	float                    RibbonWidthScale = 1.0f;
	bool                     bSoftParticles   = false; // (예약) 깊이 기반 부드러운 가장자리

	// 런타임 (직렬화 제외): 리소스 관리자가 채운다
	FTextureHandle Texture;
	FMeshHandle    Mesh;
};

// ---- 이미터 / 시스템 --------------------------------------------------------------------------------------------

struct FParticleEmitter
{
	std::string        Name        = "Emitter";
	bool               bEnabled    = true;
	EParticleSimTarget SimTarget   = EParticleSimTarget::CPU;
	bool               bLocalSpace = false; // 입자가 이미터를 따라 움직인다
	float              Duration    = 2.0f;  // 주기 (초)
	bool               bLoop       = true;
	uint32             MaxParticles = 1000;
	uint32             Seed         = 1;

	std::array<std::vector<FParticleModule>, static_cast<size_t>(EParticleStage::Count)> Stages;
	std::vector<FParticleRendererSettings>                                              Renderers;

	std::vector<FParticleModule>&       GetStage(EParticleStage Stage) { return Stages[static_cast<size_t>(Stage)]; }
	const std::vector<FParticleModule>& GetStage(EParticleStage Stage) const { return Stages[static_cast<size_t>(Stage)]; }
	// 해당 종류의 켜진 첫 모듈 (없으면 nullptr)
	const FParticleModule* FindModule(EParticleModuleType Type) const;

	// 기본 구성 (초당 생성 + 초기화 + 원뿔 속도 + 크기/색 곡선 + 스프라이트)
	static FParticleEmitter MakeDefault();
};

// 파티클 시스템 에셋 = .eparticle 파일 (JSON, "Version": 2). 이전 단일 이미터 형식(버전 없음)은 읽을 때 변환한다
struct FParticleSystemAsset
{
	static constexpr const wchar_t* Extension = L".eparticle";
	static constexpr int32          Version   = 2;

	std::string                   Name;
	std::vector<FParticleEmitter> Emitters;

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json); // 런타임 핸들(텍스처/메시)은 같은 자리 렌더러에서 유지
	bool        LoadFromFile(const std::filesystem::path& Path);
	bool        SaveToFile(const std::filesystem::path& Path) const;

	static FParticleSystemAsset MakeDefault(const std::string& Name);
};

// ---- 런타임 --------------------------------------------------------------------------------------------------

// 입자 속성 (CPU). GPU 입자 구조와 필드 뜻이 같다
struct FParticle
{
	FVector3 Position; // 월드 (로컬 공간 이미터면 이미터 기준)
	FVector3 Velocity;
	FVector4 BaseColor = FVector4::OneVector; // 생성 시 색 (ScaleColor 기준)
	FVector4 Color     = FVector4::OneVector;
	FVector2 BaseSize  = FVector2(10.0f, 10.0f);
	FVector2 Size      = FVector2(10.0f, 10.0f);
	float    Rotation     = 0.0f; // 도
	float    RotationRate = 0.0f;
	float    Age          = 0.0f;
	float    Lifetime     = 1.0f;
	float    Mass         = 1.0f;
	float    SubImage     = 0.0f; // 플립북 프레임 (실수 — 보간용)
	uint32   SpawnIndex   = 0;    // 이미터 안 생성 순번 (리본 순서, 난수 씨앗)
};

// GPU 계산 상태 (Renderer 모듈이 구현, 이미터 인스턴스가 소유)
class IParticleGpuState
{
public:
	virtual ~IParticleGpuState() = default;
};

// GPU 이미터의 한 프레임 계산 요청 (Update가 쌓고 렌더러가 소비)
struct FParticleGpuStep
{
	float      DeltaSeconds = 0.0f;
	float      EmitterAlpha = 0.0f; // 주기 안 시간 비율 (이미터 곡선용)
	uint32     SpawnStart   = 0;    // 이번에 만들 입자의 첫 생성 순번
	uint32     SpawnCount   = 0;
	FMatrix4x4 EmitterWorld;
};

struct FParticleEmitterInstance
{
	std::vector<FParticle> Particles; // CPU 이미터
	float                  EmitterTime      = 0.0f;
	float                  SpawnAccumulator = 0.0f;
	uint32                 SpawnCounter     = 0; // 누적 생성 수 (= 다음 SpawnIndex)
	uint32                 LoopIndex        = 0;
	uint32                 FiredBurstMask   = 0; // 이번 주기에 이미 낸 버스트 (모듈 순서 비트)
	bool                   bFinished        = false;

	std::vector<FParticleGpuStep>      PendingGpuSteps;
	std::shared_ptr<IParticleGpuState> GpuState;
	uint32                             GpuEstimatedAlive = 0; // 표시용 추정치
};

// 이미터 인스턴스들의 시뮬레이션 상태 (직렬화/리플렉션 제외)
struct FParticleRuntime
{
	std::shared_ptr<const FParticleSystemAsset> System;        // 에셋 캐시 공유 (편집기가 고치면 즉시 반영)
	std::string                                 ResolvedAsset; // System을 얻은 에셋 경로 (바뀌면 다시 해석)
	std::vector<FParticleEmitterInstance>       Emitters;      // System->Emitters와 같은 개수 (Update가 맞춘다)
	FMatrix4x4                                  LastWorld;     // 로컬 공간 이미터 렌더용

	void   Restart();
	uint32 CountAlive() const; // CPU 입자 + GPU 추정
};

// 파티클 컴포넌트. Asset = .eparticle 경로 (Content 기준)
struct FParticleSystemComponent
{
	std::string Asset;
	bool        bPlaying = true;
	float       Speed    = 1.0f;

	FParticleRuntime Runtime;
};

// 입자 하나를 렌더할 때의 최종 값 (CPU 렌더러/테스트 공용)
struct FParticleSimulation
{
	// 이미터 하나 진행 (CPU: 입자 적분/생성, GPU: 생성 수만 계산해 요청을 쌓음)
	static void Update(const FParticleEmitter& Emitter, FParticleEmitterInstance& Instance, const FMatrix4x4& EmitterWorld, float DeltaSeconds);
	// 입자 생성 순번으로 만드는 결정적 난수 (GPU와 같은 해시)
	static uint32 Hash(uint32 Value);
	static float  Random01(uint32 SpawnIndex, uint32 Seed, uint32 Stream);
};

class FParticleSystem
{
public:
	static void   Update(FScene& Scene, float DeltaSeconds);
	static void   Restart(FScene& Scene, FEntity Entity);
	static uint32 CountParticles(FScene& Scene);
};
