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
	const FD3D12RenderTarget* SceneColor = nullptr; // HDR (+ 깊이), 알파 = 반응형 마스크. 내부 해상도
	const FD3D12RenderTarget* Velocity   = nullptr; // 움직임 벡터 (ScreenSpace.hlsli 규약)
	FMatrix4x4                Reprojection;         // 현재 클립(지터 없음) → 이전 클립 (카메라만)
	bool                      bHistoryValid  = false;
	float                     CurrentWeight  = 0.1f;
	float                     ReactiveWeight = 0.6f;
	float                     VarianceGamma  = 1.25f;
	// TAAU (Phase 48): 출력(이력) 크기. 0이면 씬 컬러와 같은 크기(기존 TAA). 다르면 PSResolveUpsample로 시간 업샘플
	uint32                    OutputWidth  = 0;
	uint32                    OutputHeight = 0;
	FVector2                  JitterNdc;            // 이번 프레임 투영 지터 (업샘플에서 표본 위치 보정)
};

// TAA: 톤매핑 전 HDR 이력 버퍼 2개(번갈아 읽기/쓰기). Resolve 결과 = 이번 프레임 이력 = 포스트 입력.
//   이력 리셋: 크기 변경(버퍼 재생성), bHistoryValid = false(카메라 컷/첫 프레임/여러 뷰 렌더러) → 현재 프레임 그대로
//   렌더 그래프 패스 "TAA" 하나 (씬 컬러·움직임·깊이·이전 이력 읽기 → 이번 이력 쓰기). 상태 전이는 그래프가 한다
//   TAAU: 씬(내부 해상도) ≠ 출력이면 이력은 출력 해상도 — 내부 해상도가 바뀌어도(동적 해상도) 이력은 그대로 이어진다.
//   업샘플일 때는 오버레이 깊이(출력 해상도, AddOverlayDepthPass)도 만든다
//   깜빡임 감지(재구성 경로): 출력 해상도 통계 버퍼 2개(번갈아 읽기/쓰기, 이력과 같은 크기·수명 — TemporalAA.hlsl 머리 주석 5))
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

	// 업샘플 프레임의 오버레이 깊이: 내부 깊이(지터)를 출력 해상도·지터 없음으로 옮긴다 (에디터 그리드/디버그 선이 깊이 테스트에 쓴다).
	// 결과 깊이는 그래프 실행 뒤 DEPTH_WRITE (GetOverlayDepth)
	void AddOverlayDepthPass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef SceneDepth, const FVector2& JitterNdc,
	                         uint32 OutputWidth, uint32 OutputHeight, int32 Timer);
	// 마지막 오버레이 깊이 (없으면 nullptr). 크기 확인은 호출하는 쪽
	const FD3D12RenderTarget* GetOverlayDepth() const { return OverlayDepth.get(); }

	// 마지막 Resolve 결과 (지난 프레임의 안티에일리어싱된 HDR — SSR 반사 색 원본). 이력이 없으면 nullptr
	const FD3D12RenderTarget* GetLastOutput() const { return bHasHistory ? HistoryTargets[WriteIndex ^ 1].get() : nullptr; }

	// 이력을 버린다 (다음 Resolve는 현재 프레임 그대로)
	void ResetHistory() { bHasHistory = false; }

private:
	bool CreatePipelines(FD3D12PipelineState& OutResolve, FD3D12PipelineState& OutUpsample, FD3D12PipelineState& OutDepth, bool bForceRecompile);
	void EnsureTargets(uint32 Width, uint32 Height);

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12PipelineState             Pipeline;          // PSResolve (네이티브)
	FD3D12PipelineState             UpsamplePipeline;  // PSResolveUpsample (TAAU)
	FD3D12PipelineState             DepthPipeline;     // PSUpscaleDepth (오버레이 깊이)

	std::unique_ptr<FD3D12RenderTarget> HistoryTargets[2];
	std::unique_ptr<FD3D12RenderTarget> FlickerTargets[2]; // 깜빡임 통계 (HistoryTargets와 같은 번호로 읽기/쓰기)
	std::unique_ptr<FD3D12RenderTarget> OverlayDepth; // 출력 해상도 깊이 (+ 쓰지 않는 R8 색)
	uint32                              WriteIndex  = 0;
	bool                                bHasHistory = false;
	bool                                bHasFlicker = false; // 직전 해상이 재구성 경로로 통계를 썼다
};
