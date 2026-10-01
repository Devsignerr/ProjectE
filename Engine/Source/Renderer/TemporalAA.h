#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"

#include <memory>

class FD3D12RHI;
class FShaderLibrary;
class FScreenPassRootSignature;

// TAA 해상 입력 (FSceneRenderer가 채운다)
struct FTemporalAAInputs
{
	const FD3D12RenderTarget* SceneColor = nullptr; // HDR (+ 깊이), 알파 = 반응형 마스크
	const FD3D12RenderTarget* Velocity   = nullptr; // 움직임 벡터 (ScreenSpace.hlsli 규약)
	FMatrix4x4                Reprojection;         // 현재 클립(지터 없음) → 이전 클립 (카메라만)
	bool                      bHistoryValid  = false;
	float                     CurrentWeight  = 0.1f;
	float                     ReactiveWeight = 0.6f;
	float                     VarianceGamma  = 1.25f;
};

// TAA: 톤매핑 전 HDR 이력 버퍼 2개(번갈아 읽기/쓰기). Resolve 결과 = 이번 프레임 이력 = 포스트 입력.
//   이력 리셋: 크기 변경(버퍼 재생성), bHistoryValid = false(카메라 컷/첫 프레임/여러 뷰 렌더러) → 현재 프레임 그대로
//   씬 깊이는 DEPTH_WRITE 상태로 받아 읽는 동안만 셰이더 리소스로 전이한다
class FTemporalAA
{
public:
	~FTemporalAA();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 해상 결과 (PIXEL_SHADER_RESOURCE, HDR). 반환 타깃은 다음 Resolve까지 유효
	const FD3D12RenderTarget& Resolve(const FTemporalAAInputs& Inputs);

	// 이력을 버린다 (다음 Resolve는 현재 프레임 그대로)
	void ResetHistory() { bHasHistory = false; }

private:
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	void EnsureTargets(uint32 Width, uint32 Height);

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12PipelineState             Pipeline;

	std::unique_ptr<FD3D12RenderTarget> HistoryTargets[2];
	uint32                              WriteIndex  = 0;
	bool                                bHasHistory = false;
};
