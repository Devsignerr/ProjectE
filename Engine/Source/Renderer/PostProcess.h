#pragma once

#include "Core/CoreTypes.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <chrono>
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
};

// HDR 씬 컬러 → 출력 대상(LDR, sRGB RTV).
//   [블룸] 13탭 다운샘플 체인(첫 단계 Karis 평균 + 임계값) → 텐트 업샘플 가산 합성 (절반 해상도부터 최대 6단계)
//   [자동 노출] 1/4 해상도 로그 휘도 히스토그램(픽셀 셰이더 UAV) → 컴퓨트 평균 + 시간 적응 (GPU 버퍼에 유지)
//   [톤매핑] 씬 + 블룸 * 강도 → 노출 → 연산자
// 씬 렌더러는 Render 한 번만 호출한다. 한 커맨드 리스트에서 여러 번 호출해도 안전하다(버퍼 상태를 매번 COMMON으로 복귀).
class FPostProcessor
{
public:
	FPostProcessor();
	~FPostProcessor();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// HdrSceneColor: PIXEL_SHADER_RESOURCE 상태의 HDR 텍스처 SRV (셰이더 가시 힙). 크기는 Output과 같다고 가정
	void Render(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& HdrSceneColor, const FRenderOutput& Output,
	            const FPostProcessSettings& Settings);

	// 핫 리로드: 모든 PSO를 새 셰이더로 재생성 (하나라도 실패하면 해당 PSO는 기존 유지, false)
	bool ReloadShaders(bool bForceRecompile);

	uint32 GetBloomMipCount() const { return static_cast<uint32>(BloomTargets.size()); }

private:
	enum class EPipeline : uint8
	{
		BloomDownsample,
		BloomUpsample,
		Histogram,
		AverageLuminance,
		Count
	};

	bool CreatePipeline(EPipeline Pipeline, FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	bool CreateTonemapPipeline(FD3D12PipelineState& OutPipeline, DXGI_FORMAT OutputFormat, bool bForceRecompile);
	FD3D12PipelineState* GetTonemapPipeline(DXGI_FORMAT OutputFormat);

	bool CreateExposureBuffers();
	void EnsureBloomTargets(uint32 Width, uint32 Height);
	void TransitionExposureBuffers(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After);

	void RenderBloom(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& SceneColor, uint32 Width, uint32 Height,
	                 const FPostProcessSettings& Settings);
	void RenderAutoExposure(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& SceneColor, uint32 Width, uint32 Height,
	                        float DeltaSeconds, const FPostProcessSettings& Settings);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature                                   RootSignature; // 모든 포스트 패스 공용 (그래픽스/컴퓨트)
	FD3D12PipelineState                                   Pipelines[static_cast<size_t>(EPipeline::Count)];
	std::unordered_map<DXGI_FORMAT, FD3D12PipelineState> TonemapPipelines; // 출력 포맷별

	// 블룸 레벨 (0 = 절반 해상도)
	std::vector<std::unique_ptr<FD3D12RenderTarget>> BloomTargets;
	uint32                                           BloomSourceWidth  = 0;
	uint32                                           BloomSourceHeight = 0;

	// 자동 노출 GPU 버퍼: 히스토그램(256 x uint, raw), 적응된 평균 휘도(float 1개, 프레임 간 유지)
	ComPtr<ID3D12Resource> HistogramBuffer;
	ComPtr<ID3D12Resource> LuminanceBuffer;
	FD3D12DescriptorHandle HistogramUav;
	FD3D12DescriptorHandle LuminanceUav;

	std::chrono::steady_clock::time_point LastRenderTime;
	bool                                  bHasLastRenderTime = false;
};
