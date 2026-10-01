#pragma once

#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/ShaderTypes.h"

#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FShaderLibrary;
class FSkinnedMeshPalette;

// 점광원/스포트라이트 + 클러스터드 컬링.
//   Prepare: 씬 라이트 수집(카메라 프러스텀 밖 제외, 가까운 순 MaxLocalLights개) → 목록을 동적 업로드 버퍼에 올림 →
//            클러스터 컬링 계산 셰이더(ClusterCulling.hlsl) → 클러스터 버퍼를 PIXEL_SHADER_RESOURCE로.
//   메시 패스는 GetConstants(b5) / GetLightList(t9) / GetClusterData(t10)를 루트 디스크립터로 바인딩한다.
//   클러스터 화면 크기는 실제 렌더 타깃 크기(픽셀 아트 모드는 저해상도)여야 한다.
class FLocalLightRenderer
{
public:
	~FLocalLightRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 그래픽스 패스 전에 호출 (계산 루트 시그니처를 바꾸므로 이후 패스는 자기 루트 시그니처를 다시 설정한다)
	void Prepare(FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height);

	D3D12_GPU_VIRTUAL_ADDRESS GetConstants() const { return ConstantsAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS GetLightList() const { return LightListAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS GetClusterData() const;
	uint32                    GetLightCount() const { return static_cast<uint32>(Lights.size()); }

private:
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	void CollectLights(FScene& Scene, const FCamera& Camera);
	void TransitionClusters(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature ComputeRootSignature;
	FD3D12PipelineState CullPipeline;

	ComPtr<ID3D12Resource> ClusterBuffer; // uint[ClusterCount * ClusterStride]
	D3D12_RESOURCE_STATES  ClusterState = D3D12_RESOURCE_STATE_COMMON;

	std::vector<FLocalLightGpuData> Lights;      // 이번 프레임 목록
	std::vector<float>              LightScores; // 정렬 키 (카메라 거리 - 반경)
	FClusterConstants               Constants;
	D3D12_GPU_VIRTUAL_ADDRESS       ConstantsAddress = 0;
	D3D12_GPU_VIRTUAL_ADDRESS       LightListAddress = 0;
};
