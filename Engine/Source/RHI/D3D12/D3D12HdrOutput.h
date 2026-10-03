#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <memory>

class FD3D12RHI;
class FD3D12ShaderCompiler;
class FShaderLibrary;

// HDR 출력 모드 (스왑체인 형식)
enum class EHdrSwapChainMode : uint8
{
	Off,   // SDR (R8G8B8A8, sRGB 뷰)
	Hdr10, // R10G10B10A2 + ST.2084 PQ / BT.2020
	ScRgb, // R16G16B16A16_FLOAT + 선형 BT.709 (1 = 80 nits)
};

// HDR 디스플레이 출력 (Phase 49). 켜져 있으면 앱이 아는 "백버퍼"는 겹침 층(R8G8B8A8, sRGB/UNORM 뷰 — UI·ImGui·오버레이가 그대로 그린다,
// 매 프레임 투명 0으로 지움)이 되고, 씬 렌더러는 GetSceneTarget(R16G16B16A16_FLOAT, 선형 BT.709, 1 = 종이 흰색)에 HDR 톤매핑 결과를 쓴다.
// EndFrame에서 HdrComposite.hlsl이 둘을 선형으로 합쳐(겹침 층 = 종이 흰색 밝기) 진짜 스왑체인 백버퍼에 HDR10/scRGB로 인코딩한다.
// 스크린샷은 같은 합성을 SDR 미리보기(sRGB 8비트)로 따로 그려 읽는다. 상태: 겹침 층·씬 타깃은 프레임 사이 PIXEL_SHADER_RESOURCE,
// 프레임 동안 RENDER_TARGET (BeginFrame/Composite가 전이 — 그래프 밖 스왑체인 단계)
class FD3D12HdrOutput
{
public:
	~FD3D12HdrOutput();

	bool Enable(FD3D12RHI& Rhi, EHdrSwapChainMode InMode, float InPaperWhiteNits, uint32 Width, uint32 Height);
	void Disable(FD3D12RHI& Rhi);
	void Shutdown();
	bool IsActive() const { return Mode != EHdrSwapChainMode::Off; }
	EHdrSwapChainMode GetMode() const { return Mode; }
	float GetPaperWhiteNits() const { return PaperWhiteNits; }
	void  SetPaperWhiteNits(float Nits) { PaperWhiteNits = Nits; }
	float GetMaxNits() const { return MaxNits; }
	void  SetMaxNits(float Nits) { MaxNits = Nits; }

	bool Resize(FD3D12RHI& Rhi, uint32 Width, uint32 Height);

	// BeginFrame: 겹침 층·씬 타깃 → RENDER_TARGET, 겹침 층 투명 지우기, 씬 타깃 ClearColor
	void BeginFrame(ID3D12GraphicsCommandList* List, const float ClearColor[4]);
	// 겹침 층·씬 타깃 → SRV 후 백버퍼(RENDER_TARGET 상태)에 합성. bPreview = SDR 미리보기 타깃에도 (스크린샷 — PreviewTarget을 RENDER_TARGET으로 남김)
	void Composite(ID3D12GraphicsCommandList* List, D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv, uint32 Width, uint32 Height, bool bPreview);

	D3D12_CPU_DESCRIPTOR_HANDLE GetOverlayRtv(bool bLinearView) const { return OverlayRtvHeap.GetCpuHandle(bLinearView ? 1 : 0); }
	ID3D12Resource*             GetOverlayResource() const { return Overlay.Get(); }
	FD3D12RenderTarget*         GetSceneTarget() const { return SceneTarget.get(); }
	ID3D12Resource*             GetPreviewResource() const { return Preview.Get(); }

private:
	bool CreatePipelines(FD3D12RHI& Rhi);
	bool CreateTargets(FD3D12RHI& Rhi, uint32 Width, uint32 Height);
	void ReleaseTargets(FD3D12RHI* Rhi);

	EHdrSwapChainMode Mode           = EHdrSwapChainMode::Off;
	float             PaperWhiteNits = 200.0f;
	float             MaxNits        = 1000.0f; // 톤매핑 하이라이트 상한 (씬 렌더러가 읽는다)
	FD3D12RHI*        RhiPtr         = nullptr;

	std::unique_ptr<FD3D12ShaderCompiler> Compiler;
	std::unique_ptr<FShaderLibrary>       Library;
	FD3D12RootSignature                   RootSignature;
	FD3D12PipelineState                   Pipelines[2]; // [0] 스왑체인 형식, [1] SDR 미리보기 (R8G8B8A8_UNORM)
	DXGI_FORMAT                           PipelineFormat = DXGI_FORMAT_UNKNOWN;

	ComPtr<ID3D12Resource>              Overlay;
	FD3D12DescriptorHeap                OverlayRtvHeap; // [0] sRGB, [1] UNORM
	FD3D12DescriptorHandle              OverlaySrv;
	std::unique_ptr<FD3D12RenderTarget> SceneTarget;
	ComPtr<ID3D12Resource>              Preview;
	FD3D12DescriptorHeap                PreviewRtvHeap;
};
