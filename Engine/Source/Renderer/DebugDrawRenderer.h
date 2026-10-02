#pragma once

#include "Core/CoreTypes.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <vector>

class FCamera;
class FD3D12RHI;
class FDebugDraw;
class FShaderLibrary;
struct FRenderOutput;

// FDebugDraw 선을 화면에 그린다 (Phase 41-3). 셰이더는 에디터 내비메시 표시와 같은 위치+색 선 셰이더(NavMeshDebug.hlsl)를 쓴다.
// 호출: 씬 렌더(FSceneRenderer::Render — 포스트·TAA 끝) 뒤 오버레이 단계, 카메라의 지터 없는 뷰-투영.
//   깊이 테스트 선 = 씬 깊이(같은 크기일 때만, 쓰기 없음, LESS_EQUAL)로 가려진다. 씬 깊이를 쓸 수 없으면(픽셀 아트 모드 —
//   씬이 저해상도) 깊이 테스트 선은 그리지 않고 "항상 위" 선만 그린다 (에디터 그리드와 같은 제약).
// 정점은 프레임마다 동적 업로드 버퍼로 (선 × 32바이트, FDebugDraw::MaxLines로 최대 1MB)
class FDebugDrawRenderer
{
public:
	~FDebugDrawRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);
	bool IsInitialized() const { return Rhi != nullptr; }

	// Output: 렌더 타깃 상태의 출력(sRGB RTV). SceneDepthDsv: 같은 크기의 씬 깊이 (DEPTH_WRITE, 읽기 전용) — ptr 0 = 없음.
	// 끝나면 Output RTV만 (깊이 없이) 바인딩된 상태로 둔다
	void Render(const FDebugDraw& Lines, const FCamera& Camera, const FRenderOutput& Output, D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv);

private:
	struct FLineVertex
	{
		float  Position[3];
		uint32 Color = 0;
	};
	static_assert(sizeof(FLineVertex) == 16);

	bool CreatePipelines(FD3D12PipelineState& OutDepthTested, FD3D12PipelineState& OutOnTop, bool bForceRecompile);
	void DrawBatch(const std::vector<FLineVertex>& Vertices, const FD3D12PipelineState& Pipeline);

	FD3D12RHI*                Rhi           = nullptr; // 비소유
	FShaderLibrary*           ShaderLibrary = nullptr; // 비소유
	FD3D12RootSignature       RootSignature;
	FD3D12PipelineState       DepthTestedPipeline;
	FD3D12PipelineState       OnTopPipeline;
	std::vector<FLineVertex>  DepthTestedVertices; // 프레임마다 재사용
	std::vector<FLineVertex>  OnTopVertices;
};
