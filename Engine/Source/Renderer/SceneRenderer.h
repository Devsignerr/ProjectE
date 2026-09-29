#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <vector>

class FCamera;
class FD3D12RHI;
struct FMaterial;
class FResourceManager;
class FScene;
class FStaticMesh;

struct FSceneRenderStats
{
	uint32 TotalMeshes   = 0; // 씬의 정적 메시 컴포넌트 수
	uint32 VisibleMeshes = 0; // 컬링 통과
	uint32 DrawCalls     = 0;
};

// 씬의 정적 메시를 수집 → 프러스텀 컬링 → 정렬 → 드로우.
// 호출 순서: Rhi.BeginFrame() → Render() → Rhi.EndFrame()
class FSceneRenderer
{
public:
	bool Init(FD3D12RHI& InRhi, FResourceManager& InResources);
	void Shutdown();

	void Render(FScene& Scene, const FCamera& Camera);

	// 핫 리로드: 셰이더를 라이브러리에서 다시 얻어 PSO를 재생성한다. 성공 시 교체(이전 PSO는 지연 해제),
	// 실패 시 기존 PSO를 유지하고 false. bForceRecompile이면 캐시·쿠킹 파일을 무시하고 컴파일한다.
	bool ReloadShaders(bool bForceRecompile = false);

	FShaderLibrary& GetShaderLibrary() { return ShaderLibrary; }

	// 컬링 프러스텀 고정 (컬링 동작 확인용). 켜면 이후 카메라를 움직여도 컬링은 고정 시점 기준
	void SetFreezeCulling(bool bFreeze);
	bool IsCullingFrozen() const { return bCullingFrozen; }

	const FSceneRenderStats& GetStats() const { return Stats; }

	// 씬에 조명 컴포넌트가 없을 때 사용하는 기본값
	FVector3 AmbientColor = FVector3(0.08f, 0.09f, 0.12f);

private:
	struct FMeshDrawCommand
	{
		const FStaticMesh* Mesh     = nullptr;
		const FMaterial*   Material = nullptr;
		FMeshHandle        MeshHandle;
		FMaterialHandle    MaterialHandle;
		FMatrix4x4         World;
		float              DistanceSquared = 0.0f;
	};

	// 현재 라이브러리 셰이더로 메시 PSO 생성 (Init/ReloadShaders 공용)
	bool CreateMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);

	void               CollectDrawCommands(FScene& Scene, const FFrustum& Frustum, const FVector3& CameraPosition);
	FPerFrameConstants BuildPerFrameConstants(FScene& Scene, const FCamera& Camera) const;

	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;

	FD3D12ShaderCompiler ShaderCompiler;
	FShaderLibrary       ShaderLibrary; // 쿠킹된 DXIL 우선, 없으면 컴파일
	FD3D12RootSignature  RootSignature;
	FD3D12PipelineState  PipelineState;

	std::vector<FMeshDrawCommand> DrawCommands;
	FSceneRenderStats             Stats;

	FFrustum FrozenFrustum;
	bool     bCullingFrozen = false;
};
