#pragma once

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/ShadowMath.h"

class FCamera;
class FD3D12RHI;
class FShaderLibrary;

// 방향광 섀도우 설정 (씬 렌더러가 소유, 에디터가 조정)
struct FShadowSettings
{
	bool   bEnabled         = true;
	uint32 CascadeCount     = 4;
	uint32 Resolution       = 2048;  // 캐스케이드 한 장 크기 (정사각형)
	float  ShadowDistance   = 6000.0f; // cm: 카메라에서 그림자를 그리는 최대 거리 (60m)
	float  SplitLambda      = 0.75f; // 0 = 균등 분할, 1 = 로그 분할
	float  CasterExtension  = 5000.0f; // cm: 조각 밖(광원 쪽) 캐스터 포함 거리
	int32  DepthBias        = 1000;  // 래스터라이저 고정 바이어스 (D32 단위)
	float  SlopeBias        = 2.0f;
	float  NormalOffset     = 1.5f;  // 텍셀 크기 배수만큼 법선 방향으로 조회 위치를 민다
	bool   bVisualizeCascades = false;
};

// 셰이더 cbuffer ShadowConstants (Mesh.hlsl b3)와 1:1
struct FShadowConstants
{
	FMatrix4x4 CascadeViewProjection[ShadowMath::MaxCascades];
	FVector4   CascadeSplits;     // 뷰 공간 far 거리
	FVector4   CascadeTexelWorld; // 캐스케이드별 월드 텍셀 크기
	FVector3   CameraForward;
	float      ShadowEnabled = 0.0f;
	float      TexelSize     = 0.0f; // 1 / Resolution
	float      NormalOffset  = 0.0f;
	uint32     CascadeCount  = 0;
	uint32     VisualizeCascades = 0;
};
static_assert(sizeof(FShadowConstants) == 4 * 64 + 16 * 2 + 16 + 16);

// 캐스케이드 섀도우 맵: Texture2DArray(D32) 한 장에 캐스케이드별 깊이를 그린다.
class FShadowRenderer
{
public:
	~FShadowRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// 섀도우 패스 기록. 끝나면 섀도우 맵은 PIXEL_SHADER_RESOURCE 상태. 비활성이면 상수만 채운다.
	// 캐스터 = 프레임 메시 인스턴스 목록 (Upload 완료). 정적 메시는 캐스케이드마다 메시·LOD별 인스턴싱, 스킨 메시는 GPU 스키닝
	void Render(const FMeshInstanceList& Instances, const FCamera& Camera, const FVector3& LightDirection, const FShadowSettings& Settings);

	const FShadowConstants&        GetConstants() const { return Constants; }
	const FD3D12DescriptorHandle& GetShadowMapSrv() const { return Srv; }

	bool ReloadShaders(bool bForceRecompile);

	// 지난 Render의 드로우 수 / 삼각형 수 (통계)
	uint32 GetDrawCalls() const { return DrawCalls; }
	uint64 GetTriangles() const { return Triangles; }

private:
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bSkinned = false);
	void EnsureShadowMap(uint32 Resolution, uint32 Cascades);
	void ReleaseShadowMap();

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature RootSignature;
	FD3D12PipelineState Pipeline;
	FD3D12PipelineState SkinnedPipeline; // ShadowSkinnedVS + 스킨 입력 레이아웃

	ComPtr<ID3D12Resource> ShadowMap;
	FD3D12DescriptorHeap   DsvHeap; // 캐스케이드별 DSV
	FD3D12DescriptorHandle Srv;
	uint32                 MapResolution = 0;
	uint32                 MapCascades   = 0;

	FShadowConstants Constants;
	FMeshPassBatches Batches; // 캐스케이드마다 재사용
	uint32           DrawCalls = 0;
	uint64           Triangles = 0;
	int32            BakedDepthBias = FShadowSettings{}.DepthBias; // PSO에 고정된 바이어스
	float            BakedSlopeBias = FShadowSettings{}.SlopeBias;
};
