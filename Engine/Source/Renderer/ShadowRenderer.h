#pragma once

#include "Renderer/SkinCache.h"

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/MaterialRender.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShadowCacheMath.h"
#include "Renderer/ShadowCasterHook.h"
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
	// 아래 셋은 콘솔 변수가 프레임마다 채운다 (FSceneRenderer::ApplyConsoleVariables, 규칙은 ShadowCacheMath.h 머리 주석)
	bool   bCacheStatic       = true;  // r.Shadow.Cache: 정적 캐스터 캐시
	float  LodBias            = 0.0f;  // r.Shadow.LodBias: 캐스케이드 c의 LOD += floor(c × 값)
	float  MinCasterTexels    = 0.0f;  // r.Shadow.MinCasterTexels: 경계 구 지름이 텍셀 이만큼보다 작은 캐스터는 그 캐스케이드에서 뺀다 (0 = 끔)
	float  CacheQuantize      = 0.0f;  // r.Shadow.Cache.Quantize: 캐시 캐스케이드 중심 격자 양자화 (ShadowMath::ComputeCascade, 캐시가 켜졌을 때만)
	uint32 CacheQuantizeFirst = 1;     // r.Shadow.Cache.QuantizeFirstCascade: 이 번호부터 양자화
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
// 정적 캐스터 캐시(ShadowCacheMath.h): 같은 배열 모양의 캐시(평소 COPY_SOURCE)에 정적 캐스터만 그려 두고, 캐스케이드 키가 그대로면
// 캐시 → 섀도우 맵 복사 + 동적 캐스터만 그린다.
class FShadowRenderer
{
public:
	~FShadowRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();

	// 1) 캐스케이드 계산 (CPU만, 상수 채움). 비활성이면 캐스케이드 0개 (ShadowEnabled = 0)
	void PrepareCascades(const FCamera& Camera, const FVector3& LightDirection, const FShadowSettings& Settings);
	// 캐스케이드 캐스터 볼륨(라이트 프러스텀) 중 하나라도 겹치면 true — 스킨 팔레트 가시성 판정용 (PrepareCascades 뒤)
	bool IntersectsCasterVolume(const FBox& WorldBounds) const;
	// 2) 섀도우 패스 등록 (캐스케이드가 있을 때만, 그림자 맵 깊이 쓰기). 그래프 실행 뒤 섀도우 맵은 평소 상태(PIXEL_SHADER_RESOURCE).
	// 캐스터 = 프레임 메시 인스턴스 목록 (Upload 완료). 캐스케이드마다 (정적/스킨)·메시·LOD별 인스턴싱, 스킨은 프레임 팔레트(SkinPalettes, t15)
	// SkinSource = 스킨 정점 원본 (t15 — 프레임 팔레트 또는 스킨 캐시, 캐시면 그림자 패스가 읽기 선언)
	void AddPass(FRenderGraph& Graph, FRGResourceRef ShadowMapRef, const FMeshInstanceList& Instances, const FSkinDrawSource& SkinSource, int32 Timer);
	// 섀도우 맵 가져오기 (없으면 무효 참조). 평소 상태 PIXEL_SHADER_RESOURCE (캐스케이드 = 배열 장)
	FRGResourceRef ImportShadowMap(FRenderGraph& Graph) const;

	const FShadowConstants&        GetConstants() const { return Constants; }
	const FD3D12DescriptorHandle& GetShadowMapSrv() const { return Srv; }
	ID3D12Resource*               GetShadowMapResource() const { return ShadowMap.Get(); } // 평소 PIXEL_SHADER_RESOURCE

	bool ReloadShaders(bool bForceRecompile);
	// 스킨 변형 정점 셰이더 = 스킨 캐시 경로(E_SKIN_CACHE, 슬롯 1 없음). Init 전 또는 바꾼 뒤 ReloadShaders (FSceneRenderer가 맞춘다)
	bool bSkinCache = false;

	FShadowCasterHook ExtraCasters; // 메시 인스턴스 밖 캐스터 (지형 — FTerrainRenderer::RenderShadow)
	// 추가 캐스터의 그림자 상태 해시 (캐스케이드 프러스텀 안). 있으면 추가 캐스터는 정적 캐스터로 캐시에 그린다 — 해시에는 그리는 결과를
	// 바꾸는 모든 것(데이터 변경 번호, 위치, LOD 등)을 넣는다. 없으면 추가 캐스터는 동적(매 프레임 그림)
	std::function<uint64(const FFrustum& Frustum)> ExtraCasterState;
	// 매 프레임 그리는 추가 캐스터 (캐시에 넣지 않음 — 움직이는 2D 스프라이트, FSpriteShadowRenderer ESet::Dynamic). 캐시 캐스케이드에서도
	// 되살린 정적 깊이 위에 그린다. HasExtraDynamicCasters(캐스케이드 프러스텀, 출력 월드 경계)가 false인 캐스케이드에는 부르지 않는다.
	// 경계 = 훅이 그 프러스텀에 그리는 모든 것을 덮는 월드 AABB — 다음 프레임 그 텍셀 사각형만 캐시에서 되살린다 (ShadowCacheMath "장 되살리기").
	// 훅이 그 프러스텀에 실제로 그리는 것과 같은 판정·경계여야 한다 (밖에 그리면 다음 프레임 그림자가 남는다)
	FShadowCasterHook                                 ExtraDynamicCasters;
	std::function<bool(const FFrustum&, FBox& OutBounds)> HasExtraDynamicCasters;
	// 캐시를 다음 프레임에 다시 그리게 한다 (키에 담기지 않는 변경 — 메시/머티리얼을 같은 핸들로 다시 로드 등)
	void InvalidateCache() { ++CacheEpoch; }

	// 지난 Render의 드로우 수 / 삼각형 수 (통계)
	uint32 GetDrawCalls() const { return DrawCalls; }
	uint64 GetTriangles() const { return Triangles; }
	// 지난 AddPass의 캐시 사용 (통계): 캐시를 재사용한 / 다시 그린 캐스케이드 수
	uint32 GetCacheReusedCascades() const { return CacheReused; }
	uint32 GetCacheRebuiltCascades() const { return CacheRebuilt; }
	uint32 GetCacheCopiedCascades() const { return CacheCopied; }     // 캐시 → 섀도우 맵 장 전체 복사한 캐스케이드 수
	uint32 GetCacheRestoredCascades() const { return CacheRestored; } // 텍셀 사각형만 되살린 캐스케이드 수

private:
	// Variant = DepthVariant* (스킨/Masked)
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, uint32 Variant);
	// 캐스케이드별 행동(캐시)과 묶음을 정한다 (CPU, AddPass 안)
	void PrepareBatches(const FMeshInstanceList& Instances, FD3D12DynamicUploadBuffer& DynamicBuffer);
	uint64 ComputeStaticSetHash(const FMeshInstanceList& Instances) const;
	void BindDepthPass(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
	                   FDepthPassBindings& OutBindings);
	void DrawBatches(ID3D12GraphicsCommandList* CommandList, const FMeshPassBatches& CascadeBatches, uint32 Cascade, const FMeshInstanceList& Instances,
	                 const FDepthPassBindings& Bindings);
	void RecordCache(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes);
	void RecordCopy(ID3D12GraphicsCommandList* CommandList);
	void RecordRestore(ID3D12GraphicsCommandList* CommandList);
	bool CreateRestorePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	void Record(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes);
	void EnsureShadowMap(uint32 Resolution, uint32 Cascades);
	void ReleaseShadowMap();
	bool EnsureCache();
	void ReleaseCache();

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature RootSignature;
	FD3D12PipelineState Pipelines[DepthVariantCount]; // [GetDepthVariant]: 정적/스킨 × 불투명/Masked(ShadowMaskedPS)
	FMaterialDepthPipelines MaterialPipelines;          // 그래프 머티리얼 Masked (ShadowMaterialPS, 셰이더 해시별)

	ComPtr<ID3D12Resource> ShadowMap;
	FD3D12DescriptorHeap   DsvHeap; // 캐스케이드별 DSV
	FD3D12DescriptorHandle Srv;
	uint32                 MapResolution = 0;
	uint32                 MapCascades   = 0;

	FShadowConstants Constants;
	ShadowMath::FCascade CascadeData[ShadowMath::MaxCascades];
	FFrustum             CascadeFrustums[ShadowMath::MaxCascades];
	uint32               ActiveCascades = 0; // PrepareCascades 결과 (0 = 그림자 없음)
	// 캐시: 섀도우 맵과 같은 크기·장 수 (평소 COPY_SOURCE)
	ComPtr<ID3D12Resource> CacheMap;
	FD3D12DescriptorHeap   CacheDsvHeap;
	FD3D12DescriptorHandle CacheSliceSrv[ShadowMath::MaxCascades]; // 부분 되살리기가 읽는다 (장마다 하나 — 표가 가리키는 장만 SrvPixel로 전이되므로)
	FD3D12RootSignature    RestoreRootSignature;                   // t0 캐시 장 (픽셀)
	FD3D12PipelineState    RestorePipeline;
	uint64                 CacheEpoch      = 0;
	FShadowSettings        FrameSettings; // PrepareCascades 값 (AddPass가 키·LOD 바이어스에 쓴다)
	ShadowCacheMath::FCascadeCacheState CacheStates[ShadowMath::MaxCascades];
	ShadowCacheMath::ECacheAction       CascadeActions[ShadowMath::MaxCascades] = {};
	bool             bExtraStatic  = false; // 이번 프레임 추가 캐스터를 캐시에 그리나 (ExtraCasterState 있음)
	bool             bExtraDynamic[ShadowMath::MaxCascades] = {}; // 이번 프레임 그 캐스케이드에 동적 추가 캐스터가 있나 (HasExtraDynamicCasters)
	// 캐스터 거르기 병렬 조각 (PrepareBatches — 인스턴스 1024개 이상씩·최대 64조각, 캐스케이드별 정적/동적 항목을 조각 안에서 정렬 → 합친다)
	struct FCasterChunk
	{
		std::vector<FInstanceSortItem> Static[ShadowMath::MaxCascades];
		std::vector<FInstanceSortItem> Dynamic[ShadowMath::MaxCascades];
	};
	std::vector<FCasterChunk> CasterChunks;
	FMeshPassBatches StaticBatches[ShadowMath::MaxCascades];  // Direct = 모든 캐스터, Rebuild = 정적 캐스터(캐시에), Reuse = 비어 있음
	FMeshPassBatches DynamicBatches[ShadowMath::MaxCascades]; // Rebuild/Reuse의 동적 캐스터
	// 섀도우 맵 장이 이미 캐시 내용 그대로인가 (지난 프레임 캐시를 쓰고 동적 캐스터를 그리지 않았음) — 같은 키면 복사도 건너뛴다
	ShadowCacheMath::FSliceCopyState MapSlices[ShadowMath::MaxCascades];
	ShadowCacheMath::ESliceRestore   SliceRestore[ShadowMath::MaxCascades] = {}; // 이번 프레임 되살리기
	ShadowCacheMath::FTexelRect      RestoreRect[ShadowMath::MaxCascades];        // Rect일 때 사각형
	uint32           CacheReused  = 0;
	uint32           CacheRebuilt = 0;
	uint32           CacheCopied  = 0;
	uint32           CacheRestored = 0;
	uint32           DrawCalls = 0;
	uint64           Triangles = 0;
	int32            BakedDepthBias = FShadowSettings{}.DepthBias; // PSO에 고정된 바이어스
	float            BakedSlopeBias = FShadowSettings{}.SlopeBias;
};
