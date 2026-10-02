#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"

#include <memory>

class FD3D12RHI;
class FShaderLibrary;
class FScreenPassRootSignature;

// SSAO 입력 (FSceneRenderer가 채운다)
struct FAmbientOcclusionInputs
{
	const FD3D12RenderTarget* SceneDepth  = nullptr; // 깊이가 있는 씬 컬러 (깊이 DEPTH_WRITE 상태로 받는다)
	const FD3D12RenderTarget* SceneNormal = nullptr;
	FMatrix4x4                Projection;            // 깊이를 그린 투영 (지터 포함)
	FMatrix4x4                View;
	bool                      bOrthographic = false;
	float                     Radius        = 80.0f; // cm
	float                     Intensity     = 1.0f;
	uint32                    FrameIndex    = 0;     // 방향 회전 (TAA가 누적), TAA 없으면 0 고정
	uint32                    ResolutionDivisor = 2;  // 2 = 반해상도(기본), 1 = 전체 해상도 (픽셀 아트: 도트 한 칸 이동에 반 칸씩 어긋나지 않게)
	bool                      bGridNoise        = false; // 노이즈를 화면 대신 월드 도트 격자에 고정 (픽셀 아트 카메라 스냅)
	int32                     GridOrigin[2]     = {};    // 소스 픽셀 (0,0)의 격자 번호 (Right, -Up)
};

// SSAO (GTAO 방식, AmbientOcclusion.hlsl): 반해상도 계산 → 양방향 블러 가로/세로 → 결과(R = 가시도, G = 뷰 깊이).
//   메인 패스가 t16으로 읽어 간접광(IBL)에만 곱한다 (직접광 제외). 결과 타깃은 씬 컬러 크기에 맞춰 항상 있다 (끄면 읽지 않음)
class FAmbientOcclusion
{
public:
	static constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16_FLOAT;

	~FAmbientOcclusion();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 전체 해상도 ÷ ResolutionDivisor(1 또는 2) 크기 버퍼를 만든다 (씬 컬러를 맞출 때 같이)
	void EnsureTargets(uint32 FullWidth, uint32 FullHeight, uint32 ResolutionDivisor);
	void Render(const FAmbientOcclusionInputs& Inputs);

	// 결과 SRV (PIXEL_SHADER_RESOURCE). EnsureTargets 이후 유효
	const FD3D12DescriptorHandle& GetResultSrv() const { return Targets[0]->GetSrv(); }
	const FD3D12RenderTarget*     GetResult() const { return Targets[0].get(); }

private:
	bool CreatePipelines(FD3D12PipelineState& OutCompute, FD3D12PipelineState& OutBlur, bool bForceRecompile);

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12PipelineState             ComputePipeline;
	FD3D12PipelineState             BlurPipeline;

	std::unique_ptr<FD3D12RenderTarget> Targets[2]; // [0] = 결과(계산 → 세로 블러), [1] = 가로 블러 중간
};
