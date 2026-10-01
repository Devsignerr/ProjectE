#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <memory>
#include <vector>

class FD3D12RHI;
class FScreenPassRootSignature;
class FShaderLibrary;

// SSR 입력 (FSceneRenderer)
struct FScreenSpaceReflectionInputs
{
	const FD3D12RenderTarget* SceneColor  = nullptr; // 깊이(사전 패스) + 이전 프레임 색 (메인 패스 전이라 지난 프레임 내용)
	const FD3D12RenderTarget* SceneNormal = nullptr;
	FMatrix4x4                Projection;   // 지터 포함
	FMatrix4x4                View;
	FMatrix4x4                Reprojection; // 현재 클립 → 이전 클립
	float                     NearZ         = 10.0f;
	bool                      bOrthographic = false;
	float                     MaxDistance   = 2000.0f;
	float                     Thickness     = 40.0f;
	float                     MaxRoughness  = 0.6f;
	uint32                    FrameIndex    = 0;
	bool                      bStochastic   = false; // 거칠기만큼 반사 방향을 흔든다 (TAA가 누적할 때만)
};

// 화면 공간 반사 (ScreenSpaceReflections.hlsl Hi-Z + SsrTrace.hlsl, 식은 Renderer/ReflectionMath.h).
//   사전 패스 뒤·메인 패스 전: 사전 패스 깊이로 최소 깊이 밉 체인 → 픽셀마다 거울 반사 광선을 계층 추적 → (색, 신뢰도).
//   색은 이전 프레임 씬 컬러를 재투영해 읽으므로 이력이 없는 프레임(첫 프레임/크기 변경/카메라 컷)은 끈다.
//   메인 패스가 t22로 읽어 거칠기 페이드 후 캡처/하늘 IBL 반사 대신 섞는다. 결과 타깃은 항상 있다 (끄면 읽지 않음)
class FScreenSpaceReflections
{
public:
	static constexpr DXGI_FORMAT ResultFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	~FScreenSpaceReflections();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	void EnsureTargets(uint32 Width, uint32 Height);
	void Render(const FScreenSpaceReflectionInputs& Inputs);

	const FD3D12DescriptorHandle& GetResultSrv() const { return Result->GetSrv(); }

private:
	bool CreatePipelines(FD3D12PipelineState& OutCopy, FD3D12PipelineState& OutDownsample, FD3D12PipelineState& OutTrace, bool bForceRecompile);
	void ReleaseHiz();

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12RootSignature             HizRoot;           // b0 상수 4개, t0 표, u0 표, u1 표
	FD3D12PipelineState             HizCopyPipeline;
	FD3D12PipelineState             HizDownsamplePipeline;
	FD3D12PipelineState             TracePipeline;

	std::unique_ptr<FD3D12RenderTarget> Result;
	ComPtr<ID3D12Resource>              Hiz; // R32_FLOAT 밉 체인 (평소 PIXEL_SHADER_RESOURCE)
	FD3D12DescriptorHandle              HizSrv;
	std::vector<FD3D12DescriptorHandle> HizUavs;
	uint32                              HizWidth    = 0;
	uint32                              HizHeight   = 0;
	uint32                              HizMipCount = 0;
};
