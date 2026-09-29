#pragma once

#include "Core/CoreTypes.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <unordered_map>

class FD3D12RHI;
class FShaderLibrary;
struct FD3D12DescriptorHandle;

enum class ETonemapOperator : uint32
{
	None     = 0, // 클램프만
	AcesFit  = 1, // ACES 근사 (Narkowicz)
	Reinhard = 2,
};

// 포스트 프로세싱 설정 (씬 렌더러가 소유, 에디터/런타임이 조정)
struct FPostProcessSettings
{
	float            ExposureEV = 0.0f; // 노출 보정 (스톱). 배율 = 2^EV
	ETonemapOperator Tonemapper = ETonemapOperator::AcesFit;
};

// HDR 씬 컬러 → 출력 대상(LDR, sRGB RTV). 현재 단계: 노출 + 톤매핑.
// 이후 블룸 등은 이 클래스 안에서 확장한다 (씬 렌더러는 Render 한 번만 호출).
class FPostProcessor
{
public:
	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// HdrSceneColor: PIXEL_SHADER_RESOURCE 상태의 HDR 텍스처 SRV (셰이더 가시 힙)
	void Render(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& HdrSceneColor, const FRenderOutput& Output,
	            const FPostProcessSettings& Settings);

	// 핫 리로드: 모든 PSO를 새 셰이더로 재생성 (실패 시 기존 유지)
	bool ReloadShaders(bool bForceRecompile);

private:
	FD3D12PipelineState* GetTonemapPipeline(DXGI_FORMAT OutputFormat);
	bool                 CreateTonemapPipeline(FD3D12PipelineState& OutPipeline, DXGI_FORMAT OutputFormat, bool bForceRecompile);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature                                   RootSignature;
	std::unordered_map<DXGI_FORMAT, FD3D12PipelineState> TonemapPipelines; // 출력 포맷별
};
