#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/DdgiMath.h"
#include "Renderer/RenderGraph/RenderGraph.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

class FD3D12RHI;
class FRayTracingEffects;
class FRayTracingScene;
class FScene;
class FScreenPassRootSignature;
class FShaderLibrary;
struct FRayTracingGraphVariant;
struct FRayTracingLightingInputs;

// DdgiCommon.hlsli FDdgiVolume와 1:1 (128바이트)
struct alignas(16) FDdgiVolumeGpu
{
	FVector3 Origin;
	float    Intensity = 1.0f;
	FVector3 Spacing;
	float    NormalBias = 0.0f;
	uint32   Counts[3]  = {};
	uint32   ProbeOffset = 0;
	float    ViewBias       = 0.0f;
	float    FadeDistance   = 0.0f;
	float    DistanceClamp  = 1.0f;
	float    MaxRayDistance = 0.0f;
	uint32   RaysPerProbe   = 0;
	uint32   FixedRays      = 0;
	uint32   UpdateStart    = 0;
	uint32   UpdateCount    = 0;
	uint32   RowOffset      = 0;
	float    Hysteresis     = 0.97f;
	float    ChangeThreshold = 0.3f;
	float    MinFrontfaceDistance = 0.0f;
	float    BackfaceThreshold    = 0.25f;
	uint32   Flags         = 0;
	uint32   DebugProbes   = 0;
	float    DebugRadius   = 0.0f;
	float    Padding[4]    = {};
};
static_assert(sizeof(FDdgiVolumeGpu) == 128);

// DdgiCommon.hlsli DdgiConstants와 1:1 (메시 패스 b9, 프로브 광선 b0, 누적·디버그 b0 — 같은 내용)
struct alignas(16) FDdgiConstants
{
	uint32         VolumeCount = 0;
	uint32         DebugView   = 0;
	uint32         Reset       = 0;
	uint32         TotalProbes = 0;
	FVector4       RayRotation[3];
	FMatrix4x4     DebugViewProjection;
	FVector3       DebugCameraPosition;
	float          BounceIntensity = 1.0f;
	FVector2       IrradianceTexelSize;
	FVector2       DistanceTexelSize;
	FDdgiVolumeGpu Volumes[DdgiMath::MaxVolumes];
};
static_assert(sizeof(FDdgiConstants) == 160 + 128 * DdgiMath::MaxVolumes);
// DdgiCommon.hlsli와 같은 배치 (16바이트 패킹)
static_assert(offsetof(FDdgiVolumeGpu, Counts) == 32 && offsetof(FDdgiVolumeGpu, ViewBias) == 48 && offsetof(FDdgiVolumeGpu, RaysPerProbe) == 64 &&
              offsetof(FDdgiVolumeGpu, RowOffset) == 80 && offsetof(FDdgiVolumeGpu, BackfaceThreshold) == 96);
static_assert(offsetof(FDdgiConstants, RayRotation) == 16 && offsetof(FDdgiConstants, DebugViewProjection) == 64 &&
              offsetof(FDdgiConstants, DebugCameraPosition) == 128 && offsetof(FDdgiConstants, IrradianceTexelSize) == 144 &&
              offsetof(FDdgiConstants, Volumes) == 160);

// 프레임 설정 (CVar r.DDGI.* — 씬 렌더러가 채운다)
struct FDdgiSettings
{
	uint32 ProbeBudget       = 0;     // 프레임 전체 갱신 프로브 상한 (0 = 무제한)
	float  BounceIntensity   = 1.0f;  // 다중 반사 배율 (0 = 한 번 반사만)
	float  ChangeThreshold   = 1.0f;  // 텍셀 급변 판정 (상대 변화, 1 이상 = 끔 — 잡음에 걸려 깜빡여 기본 끔)
	uint32 MaxHitLocalLights = 16;    // 히트 로컬 라이트 상한 (그림자 없음)
	bool   bHitShadows       = true;  // 히트 방향광 그림자 광선
	int32  ShowProbes        = -1;    // 0 이상이면 모든 볼륨 프로브 표시 모드 (-1 = 컴포넌트 값)
	bool   bDebugView        = false; // --debug-view gi (메시가 간접 확산만)
	// 조명 변화 가속: 방향광(방향·복사)·하늘 배율이 기준에서 크게 바뀌면 BoostFrames 동안 히스테리시스 상한을 BoostHysteresis로
	// (다중 반사는 h + (1 - h)·반사율 비율로 수렴해 흰 방에서는 수백 프레임 걸린다 — DdgiMath::ComputeLightChange)
	FVector3 LightDirection  = FVector3(0.0f, 0.0f, -1.0f);
	FVector3 LightRadiance;
	float    AmbientIntensity = 1.0f;
	uint32   BoostFrames      = 30;
	float    BoostHysteresis  = 0.7f;
	// 정착: 가속이 끝난 뒤(와 이력을 처음 채울 때) SettleFrames 동안 히스테리시스 상한 SettleHysteresis — 볼륨 Hysteresis를 높여
	// (0.99) 광선 잡음 깜빡임을 줄여도 다중 반사가 예전처럼 빨리 차게 (DdgiMath::ComputeFrameHysteresis)
	uint32   SettleFrames     = 300;
	float    SettleHysteresis = 0.97f;
};

struct FDdgiStats
{
	bool   bActive       = false; // 이번 프레임 프로브를 갱신했고 메시가 볼륨을 읽는가
	uint32 Volumes       = 0;
	uint32 Probes        = 0;
	uint32 UpdatedProbes = 0;
	uint32 Rays          = 0;     // 이번 프레임 추적 광선 (갱신 프로브 × 볼륨 광선 수)
	bool   bLightBoost   = false; // 조명 변화 가속 중
	uint64 AtlasBytes    = 0;     // 아틀라스 이력 2장씩 (조도 + 거리 + 상태)
	uint32 IrradianceWidth  = 0;
	uint32 IrradianceHeight = 0;
	uint32 DistanceWidth    = 0;
	uint32 DistanceHeight   = 0;
};

// 동적 GI — DDGI 프로브 볼륨 (Phase 51). 씬의 FIrradianceVolumeComponent(Scene/IrradianceVolume.h)마다 프로브 격자를 두고
// 프레임마다 일부 프로브를 레이 트레이싱으로 갱신 → 메시 패스가 간접 확산광으로 읽는다. 식은 DdgiMath.h ↔ DdgiCommon.hlsli.
//   프레임 순서 (FSceneRenderer::RenderSceneColor): TLAS 빌드 뒤·사전 패스 전에 Prepare + ImportFrame + AddUpdatePasses
//     1) 추적 (DdgiTrace.hlsl, FRayTracingPassRoot 픽셀 셰이더 — "광선 × 갱신 프로브" 2D 타깃, 히트 = EvaluateHitLighting + 이전 프로브 조도(다중 반사))
//     2) 누적 (DdgiBlend.hlsl, 화면 패스 루트) 조도/거리/상태 아틀라스 전체를 이전 장에서 다음 장으로 (갱신 프로브만 새 값 섞기, 나머지 복사, 테두리 직접 계산)
//     3) 메시 패스(사전·메인·반투명, 지형·폴리지·그래프 머티리얼 공통)는 b9 상수 + t40~t42(이번 프레임 장)를 읽는다 — Mesh.hlsl EvaluateImageBasedLightingEx
//   볼륨이 없거나 RT를 쓸 수 없는 렌더(미리보기·캡처 굽기·픽셀 아트·RT 미지원)는 VolumeCount = 0 → 메시 셰이더는 예전 식 그대로 (화면 비트 동일)
//   이력: 볼륨 배치(순서·격자 수)가 바뀌면 아틀라스를 다시 만들고 처음부터 (Reset). 위치·바이어스·세기 변경은 이력을 유지한다
class FDdgiRenderer
{
public:
	// 조도 이력은 32비트: 16비트 float는 히스테리시스 누적 한 걸음 (1-h)·(새 값 - 이전)이 반올림에 먹혀 정상 상태가 어두운 쪽으로
	// 치우쳤다 (Tests/GI 화면 평균 h 0.97 −2.3%, 0.99 −6.4% — 2026-10-04). 거리 모멘트는 16비트로도 차이 없음 (측정)
	static constexpr DXGI_FORMAT IrradianceFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
	static constexpr DXGI_FORMAT DistanceFormat   = DXGI_FORMAT_R16G16_FLOAT;
	static constexpr DXGI_FORMAT ProbeDataFormat  = DXGI_FORMAT_R32G32B32A32_FLOAT;
	static constexpr DXGI_FORMAT RayDataFormat    = DXGI_FORMAT_R32G32B32A32_FLOAT;

	~FDdgiRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InScreenRoot, const FRayTracingEffects& InRayTracing);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);
	bool IsSupported() const { return bSupported; }

	// 프레임 CPU 준비 (사전 패스 전): 볼륨 수집 → 배치·갱신 일정 → 상수 업로드. bAllowUpdate = 이번 렌더가 RT를 쓸 수 있다
	// (TLAS를 만든다). 반환 = 이번 프레임 DDGI 활성 (볼륨이 있고 갱신 가능) — 활성이 아니어도 메시 패스용 상수는 올린다 (VolumeCount 0)
	bool Prepare(FScene& Scene, const FDdgiSettings& Settings, bool bAllowUpdate, const FMatrix4x4& DebugViewProjection, const FVector3& CameraPosition);
	// 씬 볼륨 유무만 (TLAS를 만들지 정할 때 — Prepare 전)
	static bool SceneHasVolumes(FScene& Scene);

	// 그래프: 이번 프레임 아틀라스 가져오기 (Prepare 뒤, 메시 패스 등록 전). 비활성이면 1x1 기본 텍스처
	void ImportFrame(FRenderGraph& Graph);
	// 메시 패스가 묶는 아틀라스 읽기 선언 (사전·메인·반투명 — 루트에 항상 묶이므로)
	void DeclareShadingReads(FRenderGraph::FPassBuilder& Pass) const;
	// 추적 + 누적 (활성일 때만, ImportFrame 뒤). 누적은 계산 셰이더만 → BlendQueue = AsyncCompute 후보 (r.RenderGraph.AsyncCompute)
	void AddUpdatePasses(FRenderGraph& Graph, const FRayTracingScene& Scene, FRGResourceRef Tlas, const FRayTracingLightingInputs& Lighting, int32 TraceTimer,
	                     int32 BlendTimer, ERGQueue BlendQueue = ERGQueue::Graphics);
	// 프로브 구 표시 (메인 패스 뒤, 씬 컬러 + 깊이). 표시할 볼륨이 없으면 아무것도 안 한다
	void AddProbeDebugPass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef ColorRef, FRGResourceRef DepthRef, int32 Timer);

	// 메시 패스 바인딩 (b9 상수 + t40 조도, t41 거리, t42 상태)
	D3D12_GPU_VIRTUAL_ADDRESS     GetShadingConstants() const { return ShadingConstants; }
	const FD3D12DescriptorHandle& GetIrradianceSrv() const;
	const FD3D12DescriptorHandle& GetDistanceSrv() const;
	const FD3D12DescriptorHandle& GetProbeDataSrv() const;
	const FDdgiStats&             GetStats() const { return Stats; }

private:
	bool CreateTracePipeline(FD3D12PipelineState& Out, bool bForceRecompile, const FRayTracingGraphVariant* Variant);
	bool CreatePipelines(bool bForceRecompile);
	ID3D12PipelineState* SelectTracePipeline(const FRayTracingScene& Scene);
	void ReleaseVariants(bool bAll);
	void EnsureAtlases(uint32 TotalProbes);
	void ReleaseAtlases();

	FD3D12RHI*                      Rhi         = nullptr;
	FShaderLibrary*                 Library     = nullptr;
	const FScreenPassRootSignature* ScreenRoot  = nullptr;
	const FRayTracingEffects*       RayTracing  = nullptr;
	bool                            bSupported  = false;

	FD3D12PipelineState TracePipeline;
	FD3D12PipelineState IrradiancePipeline;
	FD3D12PipelineState DistancePipeline;
	FD3D12PipelineState ProbeDataPipeline;
	FD3D12PipelineState ProbeDebugPipeline;
	struct FVariant
	{
		FD3D12PipelineState Trace;
		bool                bFailed       = false;
		uint64              LastUsedFrame = 0;
	};
	std::unordered_map<uint64, std::unique_ptr<FVariant>> Variants;

	// 이력 2장 (조도/거리/상태 — UAV + SRV, 평소 상태 PIXEL_SHADER_RESOURCE, 전용 풀) + 비활성용 1x1
	FRGResourcePool   AtlasPool;
	FRGPooledTexture* Irradiance[2] = {};
	FRGPooledTexture* Distance[2]   = {};
	FRGPooledTexture* ProbeData[2]  = {};
	std::unique_ptr<FD3D12RenderTarget> DummyIrradiance;
	std::unique_ptr<FD3D12RenderTarget> DummyDistance;
	std::unique_ptr<FD3D12RenderTarget> DummyProbeData;
	uint32 AtlasProbes = 0; // 지금 아틀라스가 담을 수 있는 프로브 수 (행 단위)
	bool   bHistoryValid = false; // 이전 장에 유효한 값이 있다 (아니면 Reset)
	uint32 ReadIndex  = 0;  // 이번 프레임 이전 장
	uint32 WriteIndex = 1;  // 이번 프레임 쓰는 장 (= 메시 패스가 읽는 장)
	uint64 LastUpdateFrame = 0; // 마지막 갱신 Rhi 프레임 (같은 프레임 두 번 갱신 방지)

	// 이번 프레임
	bool                      bFrameActive = false;
	FDdgiConstants            Constants;
	D3D12_GPU_VIRTUAL_ADDRESS ShadingConstants = 0;
	uint32                    TraceWidth = 0;
	uint32                    TraceRows  = 0;
	FDdgiSettings             FrameSettings;
	FRGResourceRef            IrradianceRefs[2];
	FRGResourceRef            DistanceRefs[2];
	FRGResourceRef            ProbeDataRefs[2];
	bool                      bAnyDebugProbes = false;

	// 볼륨 배치 키 (순서·격자 수) — 바뀌면 이력 무효 + 갱신 커서 초기화
	std::vector<uint32> LayoutKey;
	std::vector<uint32> Cursors; // 볼륨 순서별 다음 갱신 시작 (볼륨 안 번호)
	uint32              RotationFrame = 0;
	// 조명 변화 가속 기준 (마지막으로 가속을 건 조명)
	bool     bHasLightReference = false;
	FVector3 ReferenceLightDirection;
	FVector3 ReferenceLightRadiance;
	float    ReferenceAmbient = 1.0f;
	uint32   BoostFramesLeft  = 0;
	uint32   SettleFramesLeft = 0; // 가속 뒤·이력 처음 정착 구간 (DdgiMath::ComputeFrameHysteresis)

	FDdgiStats Stats;
};
