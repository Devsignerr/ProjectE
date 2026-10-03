#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/RenderGraph/RenderGraph.h"

#include <memory>
#include <unordered_map>

class FD3D12RHI;
class FRayTracingScene;
struct FRayTracingGraphVariant;
class FShaderLibrary;

// 레이 트레이싱 화면 패스 공용 루트 시그니처 (그래픽스 — 전체 화면 픽셀 셰이더에서 인라인 RayQuery).
//   계산 셰이더가 아니라 픽셀 셰이더인 이유: 머티리얼/IBL/캡처 텍스처의 상태 불변식이 PIXEL_SHADER_RESOURCE라(업로드 큐·RHI가 그 상태로 둔다)
//   계산 셰이더(NON_PIXEL)에서 읽으려면 모든 텍스처 상태 규칙을 바꿔야 한다. 픽셀 셰이더 RayQuery(SM 6.5)는 같은 하드웨어 경로다.
//   PSO는 일반 그래픽스 PSO(InitGraphics)라 PSO 캐시·워밍 대상이다 (DXR 상태 객체는 쓰지 않는다 — 상태 객체를 도입하면 캐시 밖, 이유: 라이브러리 키 미지원).
//   b0 패스 뷰 상수(RayTracingView.hlsli), b1 히트 조명 상수, t0 TLAS, t1 인스턴스 정보, t2 머티리얼 표, t3 로컬 라이트, t4 반사 캡처 목록 (루트 SRV),
//   t5~t12 화면 입력 표 8개(1칸씩), t13~t15 IBL(확산/반사/BRDF), t16 캡처 큐브 배열, 공간 1/2 무제한 표 = 셰이더 가시 힙 전체(Texture2D[] / ByteAddressBuffer[]),
//   s0 선형 반복, s1 선형 클램프, s2 점 클램프
class FRayTracingPassRoot
{
public:
	enum ERootParameter : uint32
	{
		Root_View             = 0,
		Root_Lighting         = 1,
		Root_Tlas             = 2,
		Root_Instances        = 3,
		Root_Materials        = 4,
		Root_LocalLights      = 5,
		Root_Captures         = 6,
		Root_Screen0          = 7, // t5 + i
		Root_Ibl              = Root_Screen0 + 8,
		Root_CaptureAtlas     = Root_Ibl + 1,
		Root_BindlessTextures = Root_CaptureAtlas + 1,
		Root_BindlessBuffers  = Root_BindlessTextures + 1,
		Root_GraphParams      = Root_BindlessBuffers + 1, // t17 그래프 머티리얼 파라미터 (루트 SRV, float4)
	};
	static constexpr uint32 ScreenCount = 8;

	bool                 Init(ID3D12Device* Device);
	void                 Shutdown() { RootSignature.Shutdown(); }
	ID3D12RootSignature* Get() const { return RootSignature.Get(); }

private:
	FD3D12RootSignature RootSignature;
};

// 히트 조명 입력 (RayTracingLighting.hlsli b1 + IBL/캡처/로컬 라이트 바인딩)
struct FRayTracingLightingInputs
{
	FVector3                  LightDirection = FVector3(0.0f, 0.0f, -1.0f); // 빛 진행 방향
	FVector3                  LightRadiance;                                // 색 × 강도 (0이면 방향광 없음)
	float                     AmbientIntensity = 1.0f;
	D3D12_GPU_VIRTUAL_ADDRESS LocalLights      = 0;
	uint32                    LocalLightCount  = 0;
	D3D12_GPU_VIRTUAL_ADDRESS Captures         = 0;
	uint32                    CaptureCount     = 0;
	FD3D12DescriptorHandle    IblTable;     // 연속 3칸 (확산/반사/BRDF — FIblRenderer::GetLightingTable)
	FD3D12DescriptorHandle    CaptureAtlas;
};

// 화면 입력 (씬 렌더러가 소유한 사전 패스 버퍼)
struct FRayTracingViewInputs
{
	const FD3D12RenderTarget* SceneColor    = nullptr; // 깊이 (사전 패스)
	const FD3D12RenderTarget* SceneNormal   = nullptr;
	const FD3D12RenderTarget* Velocity      = nullptr;
	const FD3D12RenderTarget* DecalNormal   = nullptr; // DBufferB (반사 — 항상 바인딩)
	const FD3D12RenderTarget* DecalMaterial = nullptr; // DBufferC
	bool                      bDecals       = false;
	FMatrix4x4                InvViewProjection;        // 지터 포함 투영의 역
	FMatrix4x4                ViewProjection;           // 지터 없음
	FMatrix4x4                PrevViewProjection;       // 지터 없음 (이력 없으면 현재)
	FVector3                  CameraPosition;
	FVector3                  CameraForward = FVector3::ForwardVector;
	bool                      bOrthographic   = false;
	float                     ProjectionScale = 1.0f;   // 투영[1][1] × 높이/2
	uint64                    FrameIndex      = 0;
	bool                      bHistoryValid   = false;
};

struct FRayTracingViewRefs
{
	FRGResourceRef Tlas;
	FRGResourceRef Depth;
	FRGResourceRef Normal;
	FRGResourceRef Velocity;
	FRGResourceRef DecalNormal;
	FRGResourceRef DecalMaterial;
};

struct FRayTracedShadowSettings
{
	float SunAngleDegrees = 0.5f;    // 태양 원반 지름 (도) — 0이면 단단한 그림자
	float NormalBias      = 1.0f;    // RayTracingMath::ComputeSurfaceBias 배율
	float MaxDistance     = 100000.0f; // cm
	float MinFilterRadius = 2.0f;    // 픽셀 (교차 표본 4x4 패턴을 지운다)
	float MaxFilterRadius = 12.0f;
	float HistoryWeight   = 0.2f;    // 이번 프레임 비중
};

struct FRayTracedReflectionSettings
{
	float MaxRoughness  = 0.6f;     // 이보다 거친 픽셀은 캡처/하늘 (Mesh.hlsl SsrMaxRoughness 페이드와 같은 값)
	float MaxDistance   = 100000.0f; // cm (빗나가면 하늘)
	float MaxBlurRadius = 16.0f;
	float NormalBias    = 1.0f;
	uint32 MaxHitLocalLights = 16;  // 히트 로컬 라이트 상한 (클러스터 없이 목록 앞부터 — 비용 상한)
	bool   bHitShadows       = true; // 히트 방향광 그림자 광선
};

// 레이 트레이싱 화면 효과 (Phase 50): RT 방향광 그림자, RT 반사 추적, TLAS 디버그 보기. 패스는 렌더 그래프로 등록하고
// FRayTracingScene(AddBuildPasses)이 만든 TLAS를 읽는다. 셰이더: RayTracedShadows.hlsl / RayTracedReflections.hlsl / RayTracingDebug.hlsl
//   그림자: 추적(교차 표본) → 공간 필터 → 시간 누적(이력 2장 핑퐁) → GetShadowMask가 메인 패스 t24 (PerFrame RayTracedShadows = 1일 때 읽음)
//   반사: 추적 → (SsrTrace와 같은 출력) → FScreenSpaceReflections::AddResolvePasses가 흐림·누적 (씬 렌더러가 연결)
class FRayTracingEffects
{
public:
	static constexpr DXGI_FORMAT ShadowTraceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // 가시도, 차폐물 거리(cm), 뷰 깊이(m)
	static constexpr DXGI_FORMAT ShadowMaskFormat  = DXGI_FORMAT_R16_FLOAT;
	static constexpr DXGI_FORMAT DebugFormat       = DXGI_FORMAT_R16G16B16A16_FLOAT;

	~FRayTracingEffects();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);
	bool IsSupported() const { return bSupported; }

	// 그림자 마스크 이력 버퍼 (메인 패스 t24 — RT 그림자가 꺼져도 바인딩은 하므로 항상 있다, 최소 1x1)
	void                      EnsureShadowTargets(uint32 Width, uint32 Height);
	const FD3D12RenderTarget& GetShadowMask() const { return *ShadowHistory[ShadowHistoryIndex]; }
	// 프레임 시작(메시 패스 등록 전): 활성이면 크기를 맞추고 이번 누적 칸을 정한다. 반환 = 메시 패스가 t24로 묶는 마스크 참조
	// (사전 패스도 같은 루트를 묶으므로 모든 메시 패스가 SrvPixel 읽기로 선언한다 — RT 그림자 누적 패스가 그 사이에 쓴다)
	FRGResourceRef BeginShadowFrame(FRenderGraph& Graph, uint32 Width, uint32 Height, bool bActive);

	// RT 그림자 패스 등록 (BeginShadowFrame(활성) 뒤): 반환 = 이번 누적 결과 참조 (= BeginShadowFrame 반환)
	FRGResourceRef AddShadowPasses(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View, const FRayTracingViewRefs& Refs,
	                               const FRayTracingLightingInputs& Lighting, const FRayTracedShadowSettings& Settings, int32 Timer);
	// RT 반사 추적 패스 등록: 결과/움직임(그래프 풀, SsrTrace와 같은 형식)을 돌려준다 → FScreenSpaceReflections::AddResolvePasses
	void AddReflectionTracePass(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View, const FRayTracingViewRefs& Refs,
	                            const FRayTracingLightingInputs& Lighting, const FRayTracedReflectionSettings& Settings, int32 Timer,
	                            FRGResourceRef& OutResult, FRGResourceRef& OutMotion);
	// 디버그 (rt-instances): 카메라 광선으로 TLAS 직접 보기 → 씬 크기 텍스처 (DebugMode 0 인스턴스 색, 1 히트 알베도, 2 히트 조명)
	FRGResourceRef AddDebugPass(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View, const FRayTracingViewRefs& Refs,
	                            const FRayTracingLightingInputs& Lighting, uint32 DebugMode, int32 Timer, FD3D12DescriptorHandle& OutSrv);

private:
	struct FPipelines
	{
		FD3D12PipelineState ShadowTrace;
		FD3D12PipelineState ShadowFilter;
		FD3D12PipelineState ShadowResolve;
		FD3D12PipelineState ReflectionTrace;
		FD3D12PipelineState Debug;
	};
	// Variant: 그래프 머티리얼 변형 (디파인 E_RT_GRAPH_MATERIALS + 가상 파일), nullptr = 기본
	bool CreatePipelines(FPipelines& Out, bool bForceRecompile, const FRayTracingGraphVariant* Variant);
	// 이번 프레임 씬의 그래프 변형 파이프라인 (처음 쓸 때 컴파일 — 실패하면 기본 = 그래프 머티리얼 회색 근사)
	const FPipelines& SelectPipelines(const FRayTracingScene& Scene);
	struct FVariantPipelines
	{
		FPipelines Pipelines;
		bool       bFailed       = false;
		uint64     LastUsedFrame = 0;
	};
	std::unordered_map<uint64, std::unique_ptr<FVariantPipelines>> VariantPipelines;
	void ReleaseVariants(bool bAll);
	// 패스 공용 루트 인자 (뷰/조명 상수, TLAS·정보·머티리얼, 로컬 라이트/캡처, IBL/아틀라스, 바인드리스 표)
	void BindRoot(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS ViewConstants, D3D12_GPU_VIRTUAL_ADDRESS LightingConstants,
	              const FRayTracingScene& Scene, const FRayTracingLightingInputs& Lighting) const;
	D3D12_GPU_VIRTUAL_ADDRESS UploadLighting(const FRayTracingLightingInputs& Lighting, uint32 MaxHitLocalLights, bool bHitShadows) const;

	FD3D12RHI*          Rhi        = nullptr;
	FShaderLibrary*     Library    = nullptr;
	bool                bSupported = false;
	FRayTracingPassRoot Root;
	FPipelines          Pipelines; // 기본 (그래프 머티리얼 없음/근사)

	std::unique_ptr<FD3D12RenderTarget> ShadowHistory[2];
	uint32                              ShadowHistoryIndex = 0;
	FRGResourceRef                      FrameShadowRef;     // 이번 그래프의 마스크 참조 (BeginShadowFrame)
	uint64                              LastShadowFrame    = 0; // 마지막 누적 Rhi 프레임 (연속일 때만 이력)
	uint32                              ShadowWidth        = 0;
	uint32                              ShadowHeight       = 0;
};
