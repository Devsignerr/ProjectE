#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

class FCamera;
class FD3D12RHI;
class FShaderLibrary;

// 에디터 뷰포트 그리드 + 월드 축 (Z = 0 평면). 톤매핑된 뷰포트 출력 위에 씬 깊이로 가려지게 합성한다.
class FEditorGrid
{
public:
	~FEditorGrid();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// Output: 렌더 타깃 상태의 출력(sRGB). SceneDepthDsv: 같은 크기의 씬 깊이 (DEPTH_WRITE 상태, 읽기 전용으로 사용)
	void Render(const FCamera& Camera, const FRenderOutput& Output, D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv);

	bool ReloadShaders(bool bForceRecompile);

	float MinorStep    = 10.0f;    // cm
	float MajorStep    = 100.0f;   // cm
	float FadeDistance = 20000.0f; // cm

private:
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);

	FD3D12RHI*          Rhi           = nullptr;
	FShaderLibrary*     ShaderLibrary = nullptr;
	FD3D12RootSignature RootSignature;
	FD3D12PipelineState Pipeline;
};
