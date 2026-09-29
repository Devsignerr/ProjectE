#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <memory>
#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FShaderLibrary;

// 에디터 선택 아웃라인: 선택 엔티티(와 하위)의 메시를 마스크에 그린 뒤, 마스크 경계를 출력 위에 합성한다.
class FSelectionOutline
{
public:
	~FSelectionOutline();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// Output: 씬 렌더 결과가 담긴 대상(렌더 타깃 상태). 선택이 없으면 아무것도 하지 않는다.
	void Render(FScene& Scene, FResourceManager& Resources, const FCamera& Camera, FEntity Selected, const FRenderOutput& Output);

	bool ReloadShaders(bool bForceRecompile);

	FVector4 OutlineColor = FVector4(1.0f, 0.45f, 0.05f, 1.0f);  // 선형 주황
	FVector4 FillColor    = FVector4(1.0f, 0.45f, 0.05f, 0.08f); // 내부 틴트
	int32    Thickness    = 2;

	// 선택 엔티티와 모든 하위 엔티티 중 보이는 정적 메시를 가진 것 (테스트용 공개)
	static void CollectOutlinedEntities(FScene& Scene, FEntity Root, std::vector<FEntity>& OutEntities);

private:
	bool CreatePipelines(FD3D12PipelineState& OutMask, FD3D12PipelineState& OutComposite, DXGI_FORMAT OutputFormat, bool bForceRecompile);
	void EnsureMask(uint32 Width, uint32 Height);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature MaskRootSignature;
	FD3D12RootSignature CompositeRootSignature;
	FD3D12PipelineState MaskPipeline;
	FD3D12PipelineState CompositePipeline;
	DXGI_FORMAT         CompositeFormat = DXGI_FORMAT_UNKNOWN;

	std::unique_ptr<FD3D12RenderTarget> Mask;
	std::vector<FEntity>                Entities; // 프레임 간 재사용
};
