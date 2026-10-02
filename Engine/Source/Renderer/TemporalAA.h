#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/RenderGraph/RenderGraph.h"

#include <memory>

class FD3D12RHI;
class FShaderLibrary;
class FScreenPassRootSignature;

// 렌더 그래프에서 TAA가 읽는 씬 리소스 (FSceneRenderer가 가져온 참조)
struct FTemporalAAGraphRefs
{
	FRGResourceRef SceneColor;
	FRGResourceRef SceneDepth;
	FRGResourceRef Velocity;
};

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
//   렌더 그래프 패스 "TAA" 하나 (씬 컬러·움직임·깊이·이전 이력 읽기 → 이번 이력 쓰기). 상태 전이는 그래프가 한다
class FTemporalAA
{
public:
	~FTemporalAA();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 해상 패스 등록. 반환 = 결과 타깃(이번 프레임 이력, 그래프 실행 뒤 PIXEL_SHADER_RESOURCE), OutResult = 그래프 참조.
	// 반환 타깃은 다음 AddPass까지 유효
	const FD3D12RenderTarget& AddPass(FRenderGraph& Graph, const FTemporalAAInputs& Inputs, const FTemporalAAGraphRefs& Refs, int32 Timer,
	                                  FRGResourceRef& OutResult);

	// 마지막 Resolve 결과 (지난 프레임의 안티에일리어싱된 HDR — SSR 반사 색 원본). 이력이 없으면 nullptr
	const FD3D12RenderTarget* GetLastOutput() const { return bHasHistory ? HistoryTargets[WriteIndex ^ 1].get() : nullptr; }

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
