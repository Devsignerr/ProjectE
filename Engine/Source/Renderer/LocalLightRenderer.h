#pragma once

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"
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

// 점광원/스포트라이트 그림자 설정 (씬 렌더러가 소유)
struct FLocalShadowSettings
{
	bool   bEnabled     = true;
	uint32 Resolution   = 1024; // 그림자 타일 한 장 (정사각형)
	uint32 MaxSlices    = 36;   // 타일 배열 상한: 스포트 1장, 점광원 6장. 넘치는 라이트는 그림자 없이 비춘다 (카메라에 가까운 순 배정)
	int32  DepthBias    = 200;  // 래스터라이저 고정 바이어스 (원근 D32)
	float  SlopeBias    = 1.5f;
	float  NormalOffset = 1.5f; // 텍셀 크기 배수
	float  NearZ        = 5.0f; // cm: 그림자 원근 근평면
};

// 점광원/스포트라이트 + 클러스터드 컬링 + 그림자.
//   Prepare: 씬 라이트 수집(카메라 프러스텀 밖 제외, 가까운 순 MaxLocalLights개) → 그림자 타일 배정·깊이 패스 →
//            목록을 동적 업로드 버퍼에 올림 → 클러스터 컬링 계산 셰이더(ClusterCulling.hlsl) → 클러스터 버퍼를 PIXEL_SHADER_RESOURCE로.
//   메시 패스는 GetConstants(b5) / GetLightList(t9) / GetClusterData(t10) / GetShadowMatrices(t11)를 루트 디스크립터로,
//   GetShadowMapSrv(t12)를 테이블로 바인딩한다. 클러스터 화면 크기는 실제 렌더 타깃 크기(픽셀 아트 모드는 저해상도)여야 한다.
//   그림자 타일 배열(Texture2DArray D32)은 그림자 라이트가 처음 나올 때 필요한 장 수만큼 만든다 (그 전엔 1x1 한 장).
class FLocalLightRenderer
{
public:
	~FLocalLightRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 그래픽스 패스 전에 호출 (계산/그림자 루트 시그니처와 뷰포트를 바꾸므로 이후 패스는 자기 상태를 다시 설정한다).
	// SkinPalettes가 있으면 스킨 메시 그림자를 GPU 스키닝으로 그린다
	void Prepare(FScene& Scene, FResourceManager& Resources, const FCamera& Camera, uint32 Width, uint32 Height,
	             const FLocalShadowSettings& ShadowSettings, const FSkinnedMeshPalette* SkinPalettes);

	D3D12_GPU_VIRTUAL_ADDRESS     GetConstants() const { return ConstantsAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS     GetLightList() const { return LightListAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS     GetClusterData() const;
	D3D12_GPU_VIRTUAL_ADDRESS     GetShadowMatrices() const { return ShadowMatricesAddress; }
	const FD3D12DescriptorHandle& GetShadowMapSrv() const { return ShadowSrv; }
	uint32                        GetLightCount() const { return static_cast<uint32>(Lights.size()); }
	uint32                        GetShadowSliceCount() const { return static_cast<uint32>(ShadowMatrices.size()); }

private:
	struct FShadowSlice
	{
		FMatrix4x4 ViewProjection;
		FVector3   LightPosition;
		float      Radius = 0.0f;
	};

	bool CreateCullPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	bool CreateShadowPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bSkinned);
	void CollectLights(FScene& Scene, const FCamera& Camera);
	void AssignShadows(const FLocalShadowSettings& Settings);
	bool EnsureShadowMap(uint32 Resolution, uint32 Slices);
	void ReleaseShadowMap();
	void RenderShadows(FScene& Scene, FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes);
	void TransitionClusters(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature ComputeRootSignature;
	FD3D12PipelineState CullPipeline;

	ComPtr<ID3D12Resource> ClusterBuffer; // uint[ClusterCount * ClusterStride]
	D3D12_RESOURCE_STATES  ClusterState = D3D12_RESOURCE_STATE_COMMON;

	// ---- 그림자 (Shadow.hlsl ShadowVS/ShadowSkinnedVS, 루트: 16 상수 + 스킨 팔레트 CBV)
	FD3D12RootSignature    ShadowRootSignature;
	FD3D12PipelineState    ShadowPipeline;
	FD3D12PipelineState    ShadowSkinnedPipeline;
	ComPtr<ID3D12Resource> ShadowMap;
	FD3D12DescriptorHeap   ShadowDsvHeap; // 장마다 DSV
	FD3D12DescriptorHandle ShadowSrv;
	uint32                 ShadowMapResolution = 0;
	uint32                 ShadowMapSlices     = 0;
	int32                  BakedDepthBias      = FLocalShadowSettings{}.DepthBias; // PSO에 고정된 바이어스
	float                  BakedSlopeBias      = FLocalShadowSettings{}.SlopeBias;

	std::vector<FLocalLightGpuData> Lights;       // 이번 프레임 목록
	std::vector<float>              LightScores;  // 정렬 키 (카메라 거리 - 반경)
	std::vector<float>              LightOuterAngles; // 스포트 외부 원뿔 (도, 그림자 투영용)
	std::vector<uint8>              LightWantsShadow;
	std::vector<FShadowSlice>       ShadowSlices;
	std::vector<FMatrix4x4>         ShadowMatrices; // 장마다 뷰-투영 (t11)
	FClusterConstants               Constants;
	D3D12_GPU_VIRTUAL_ADDRESS       ConstantsAddress      = 0;
	D3D12_GPU_VIRTUAL_ADDRESS       LightListAddress      = 0;
	D3D12_GPU_VIRTUAL_ADDRESS       ShadowMatricesAddress = 0;
};
