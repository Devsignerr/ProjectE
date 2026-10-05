#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/RenderGraph/RenderGraph.h"

#include <memory>
#include <unordered_map>
#include <vector>

class FD3D12RHI;
class FShaderLibrary;

enum class ETonemapOperator : uint32
{
	None     = 0, // 클램프만
	AcesFit  = 1, // ACES 근사 (Narkowicz)
	Reinhard = 2,
};

// 포스트 프로세싱 설정 (씬 렌더러가 소유, 에디터/런타임이 조정)
struct FPostProcessSettings
{
	// 노출. 수동 EV는 자동 노출이 켜져 있으면 보정값(스톱)으로 더해진다. 배율 = 2^EV
	float            ExposureEV = 0.0f;
	ETonemapOperator Tonemapper = ETonemapOperator::AcesFit;

	// 자동 노출: 로그 휘도 히스토그램 평균을 18% 회색으로 맞춘다
	bool  bAutoExposure     = false;
	float AutoExposureMinEV = -4.0f; // 자동으로 적용되는 EV 범위
	float AutoExposureMaxEV = 6.0f;
	float AdaptationSpeed   = 1.5f;  // 1/초 (클수록 빨리 적응)

	// 블룸: 임계값 이상의 밝은 영역을 번지게 해 톤매핑 전에 더한다
	bool  bBloomEnabled  = true;
	float BloomThreshold = 1.0f;  // HDR 밝기(최대 채널) 기준
	float BloomKnee      = 0.5f;  // 임계값 부근 부드러운 전환 폭 (임계값 대비 비율)
	float BloomIntensity = 0.08f;

	// TAA: 서브픽셀 지터 + 이력 누적 (톤매핑 전 HDR). 픽셀 아트/와이어프레임/한 렌더러로 여러 뷰를 그릴 때는 자동으로 꺼진다
	bool  bTemporalAA             = true;
	float TemporalAACurrentWeight = 0.1f;  // 현재 프레임 비중 (작을수록 부드럽지만 고스팅 위험)
	float TemporalAASharpness     = 0.25f; // TAA 흐림 보정 샤프닝 (톤매핑 패스, 0 = 끔, TAA일 때만)

	// SSAO (GTAO, 반해상도 + 양방향 블러): 간접광(IBL/하늘광)에만 적용. 깊이 사전 패스가 있어야 한다 (와이어프레임에서는 꺼짐)
	bool  bAmbientOcclusion         = true;
	float AmbientOcclusionIntensity = 1.0f;  // 가시도^세기
	float AmbientOcclusionRadius    = 80.0f; // cm

	// SSR (Hi-Z 레이마칭, 이전 프레임 색 재투영): 반사 우선순위 SSR → 반사 캡처 → 하늘. 사전 패스·시간 이력이 있어야 한다
	bool  bScreenSpaceReflections = true;
	float SsrIntensity            = 1.0f;
	float SsrMaxRoughness         = 0.6f;    // 이 거칠기에서 SSR 0 (절반부터 페이드)
	float SsrMaxDistance          = 2000.0f; // cm
	float SsrThickness            = 40.0f;   // cm (교차 뒤 허용 두께)
};

// 픽셀 아트 합성 입력 (FSceneRenderer가 FPixelArtComponent + 카메라로 채운다). 식은 PixelArtMath.h / PixelArt.hlsl
struct FPixelArtCompositeParams
{
	uint32   PixelSize = 1;
	FVector2 SubPixelOffset;            // 소스 텍셀 단위
	uint32   DitherOrigin[2]   = {};    // 0~3, 월드 격자 기준 디더 무늬 원점
	int32    GridOrigin[2]     = {};    // 소스 픽셀 (0,0)의 월드 격자 번호 (Right, -Up) — 화면 효과 노이즈를 월드에 고정
	float    OutlineStrength   = 0.0f;
	float    HighlightStrength = 0.0f;
	float    DepthThreshold    = 25.0f; // cm
	int32    ColorLevels       = 0;
	float    DitherStrength    = 0.0f;
	bool     bOrthographic     = false;
	float    NearZ             = 10.0f;
	float    FarZ              = 100000.0f;
	float    PixelViewScale    = 1.0f;  // 직교: 텍셀 월드 크기(cm), 원근: 깊이 1당 텍셀 크기
};

// 피사계 심도 입력 (FSceneRenderer가 FDepthOfFieldComponent + 카메라로 채운다). 식은 PostProcessMath.h ComputeCircleOfConfusion ↔ DepthOfField.hlsl
struct FDepthOfFieldParams
{
	float FocusDistance  = 1000.0f; // cm
	float FocalRegion    = 200.0f;  // cm
	float NearTransition = 500.0f;  // cm
	float FarTransition  = 2000.0f; // cm
	float NearBlur       = 0.01f;   // 화면 높이 비율 (컴포넌트 % / 100)
	float FarBlur        = 0.01f;
	float NearZ          = 10.0f;
	float FarZ           = 100000.0f;
	bool  bOrthographic  = false;
	// 틸트시프트 (FDepthOfFieldComponent와 같은 뜻, 각은 라디안)
	int32 Mode                = 0;
	float TiltShiftCenter     = 0.5f;
	float TiltShiftBand       = 0.12f;
	float TiltShiftTransition = 0.3f;
	float TiltShiftAngle      = 0.0f;
	// 보케 (각은 라디안)
	int32 BokehBladeCount         = 0;
	float BokehRotation           = 0.0f;
	float BokehHighlightBoost     = 0.0f;
	float BokehHighlightThreshold = 1.0f;

	static constexpr float MaxBlur = 0.04f; // 보케 반경 상한 (화면 높이 4% — 반해상도 43탭 원반의 표본 간격 한계)
};

// 톤매핑 뒤 화면 모양 (FSceneRenderer가 씬 FColorGradingComponent·FVignetteComponent로 매 프레임 SetLook). 기본 = 없음 (예전 화면)
struct FPostProcessLook
{
	bool                   bColorGrading = false;
	FD3D12DescriptorHandle GradingLut;              // 1024x32 RGBA16 UNORM 띠 (ColorGradingMath::BakeLut)
	float                  VignetteIntensity  = 0.0f;
	float                  VignetteSize       = 0.45f;
	float                  VignetteSmoothness = 0.55f;
	float                  VignetteRoundness  = 1.0f;
	FVector3               VignetteColor      = FVector3::ZeroVector; // 선형
};

// 포스트 패스 입력: 그래프 참조 + 셰이더가 읽을 SRV
struct FPostProcessGraphInput
{
	FRGResourceRef         Ref;
	FD3D12DescriptorHandle Srv;
};
// 포스트 패스 출력: RTV 정보 + 그래프 참조 (Ref가 무효면 추적하지 않는 외부 RTV — 패스는 부수 효과로 남는다)
struct FPostProcessGraphOutput
{
	FRGResourceRef Ref;
	FRenderOutput  Output;
};

// HDR 씬 컬러 → 출력 대상(LDR, sRGB RTV).
//   [피사계 심도] (별도 호출 AddDepthOfFieldPasses) 반해상도 축소·CoC → 43탭 원반 보케 → 텐트 → 전체 해상도 합성 (HDR, TAA 뒤·블룸 앞)
//   [블룸] 13탭 다운샘플 체인(첫 단계 Karis 평균 + 임계값) → 텐트 업샘플 가산 합성 (절반 해상도부터 최대 6단계)
//   [자동 노출] 1/4 해상도 로그 휘도 히스토그램(픽셀 셰이더 UAV) → 컴퓨트 평균 + 시간 적응 (GPU 버퍼에 유지)
//   [톤매핑] 씬 + 블룸 * 강도 → 노출 → 연산자
// 씬 렌더러는 그래프마다 AddPasses 한 번. 노출 버퍼는 그래프 시작·끝 COMMON (명령 목록 사이 버퍼 감쇠와 맞춤).
class FPostProcessor
{
public:
	FPostProcessor();
	~FPostProcessor();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// 렌더 그래프 패스 등록: [블룸 다운샘플/업샘플] → [자동 노출 히스토그램/평균] → 톤매핑(Output).
	// SceneColor = HDR 씬(그래프 참조 + SRV), 크기는 Output과 같다고 가정. Sharpness > 0이면 톤매핑 직전 4이웃 샤프닝 (TAA 결과일 때만)
	void AddPasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, const FPostProcessGraphOutput& Output, const FPostProcessSettings& Settings,
	               float Sharpness, int32 Timer);

	// 피사계 심도: HDR 씬(SceneColor, Width x Height) + 깊이(SceneDepth = 깊이 참조 + 깊이 SRV, 해상도가 달라도 UV로 읽음) →
	// 흐린 HDR 색(그래프 풀 텍스처). 반환값을 AddPasses의 SceneColor로 넘긴다. 흐림 반경이 둘 다 0이면 입력 그대로
	FPostProcessGraphInput AddDepthOfFieldPasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, const FPostProcessGraphInput& SceneDepth,
	                                             uint32 Width, uint32 Height, const FDepthOfFieldParams& Params, int32 Timer);

	// 픽셀 아트: 저해상도 톤매핑 결과(SourceColor, 선형) + 저해상도 깊이(SourceDepth = 깊이 참조 + 깊이 SRV)를
	// 서브픽셀 보정 최근접 확대 + 1px 외곽선/모서리 하이라이트 + 양자화/디더로 Output에 합성한다
	void AddPixelArtCompositePass(FRenderGraph& Graph, const FPostProcessGraphInput& SourceColor, const FPostProcessGraphInput& SourceDepth,
	                              const FPostProcessGraphOutput& Output, const FPixelArtCompositeParams& Params, int32 Timer);

	// 화면 공간 버퍼 확인 (ScreenDebug.hlsl): Source를 Mode(1 법선, 2 움직임, 3 깊이, 4 단일 채널)로 Output에 그린다
	void AddDebugViewPass(FRenderGraph& Graph, const FPostProcessGraphInput& Source, const FPostProcessGraphOutput& Output, uint32 Mode, int32 Timer);

	// 핫 리로드: 모든 PSO를 새 셰이더로 재생성 (하나라도 실패하면 해당 PSO는 기존 유지, false)
	bool ReloadShaders(bool bForceRecompile);

	uint32 GetBloomMipCount() const { return static_cast<uint32>(BloomTargets.size()); }

	// HDR 출력 (Phase 49): 이번 프레임 톤매핑을 HDR로 (최대 밝기 / 종이 흰색, 0 = SDR). 씬 렌더러가 출력이 RHI HDR 씬 타깃일 때 프레임마다 설정
	void SetHdrPeakRatio(float Ratio) { HdrPeakRatio = Ratio; }
	// 색 보정 LUT + 비네트 (다음 AddPasses들에 적용 — 씬 렌더러가 매 프레임)
	void SetLook(const FPostProcessLook& InLook) { Look = InLook; }

private:
	enum class EPipeline : uint8
	{
		BloomDownsample,
		BloomUpsample,
		Histogram,
		AverageLuminance,
		DofPrefilter,
		DofBokeh,
		DofPostfilter,
		DofCombine,
		Count
	};

	bool CreatePipeline(EPipeline Pipeline, FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	// 출력 포맷마다 PSO가 필요한 풀스크린 패스
	enum class EOutputPass : uint8
	{
		Tonemap,
		PixelArtComposite,
		DebugView,
		Count
	};

	bool CreateOutputPipeline(EOutputPass Pass, FD3D12PipelineState& OutPipeline, DXGI_FORMAT OutputFormat, bool bForceRecompile);
	FD3D12PipelineState* GetOutputPipeline(EOutputPass Pass, DXGI_FORMAT OutputFormat);

	bool CreateExposureBuffers();
	void EnsureBloomTargets(uint32 Width, uint32 Height);

	void AddBloomPasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, uint32 Width, uint32 Height, const FPostProcessSettings& Settings,
	                    int32 Timer, FRGResourceRef& OutBloom);
	void AddAutoExposurePasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, uint32 Width, uint32 Height, float DeltaSeconds,
	                           const FPostProcessSettings& Settings, FRGResourceRef Histogram, FRGResourceRef Luminance, int32 Timer);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;
	float           HdrPeakRatio  = 0.0f;
	FPostProcessLook Look;

	FD3D12RootSignature                                   RootSignature; // 모든 포스트 패스 공용 (그래픽스/컴퓨트)
	FD3D12PipelineState                                   Pipelines[static_cast<size_t>(EPipeline::Count)];
	std::unordered_map<DXGI_FORMAT, FD3D12PipelineState> OutputPipelines[static_cast<size_t>(EOutputPass::Count)]; // 패스·출력 포맷별

	// 블룸 레벨 (0 = 절반 해상도)
	std::vector<std::unique_ptr<FD3D12RenderTarget>> BloomTargets;
	uint32                                           BloomSourceWidth  = 0;
	uint32                                           BloomSourceHeight = 0;

	// 자동 노출 GPU 버퍼: 히스토그램(256 x uint, raw), 적응된 평균 휘도(float 1개, 프레임 간 유지)
	ComPtr<ID3D12Resource> HistogramBuffer;
	ComPtr<ID3D12Resource> LuminanceBuffer;
	FD3D12DescriptorHandle HistogramUav;
	FD3D12DescriptorHandle LuminanceUav;

	double LastRenderTime     = 0.0; // FFrameTime 누적 시간
	bool   bHasLastRenderTime = false;
};
