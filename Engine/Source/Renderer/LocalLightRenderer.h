#pragma once

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/MaterialRender.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/ShadowCasterHook.h"
#include "Scene/ResourceHandles.h"

#include <string>
#include <unordered_map>
#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FShaderLibrary;
struct FResourceRoots;

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

// 점광원/스포트라이트/면광원(Phase 52: 사각형·원판 LTC, IES, 쿠키) + 클러스터드 컬링 + 그림자.
//   면광원·IES·쿠키 식은 Renderer/AreaLightMath.h 머리 주석. 셰이더가 읽는 텍스처(LTC 표 2장, IES 프로필, 쿠키)는 셰이더 가시 힙 칸 번호로
//   목록/상수에 넣고 메시 패스가 힙 전체를 공간 3 무제한 표로 묶어 읽는다(바인드리스 — 프레임마다 다시 채우므로 밉 스트리밍·재생성 안전).
//   IES는 경로별로 한 번 파싱해 θ × φ R32F 텍스처로 굽는다(FFileSystem — pak 안전, 쿠킹 없음), 쿠키는 공개 LoadTexture(고정 = 전체 밉).
//   PrepareLights: 씬 라이트 수집(카메라 프러스텀 밖 제외, 가까운 순 MaxLocalLights개) → 그림자 타일 배정 (CPU만 — 이후
//            IntersectsShadowCaster로 스킨 팔레트 가시성 판정에 쓴다)
//   PrepareFrame: 목록을 동적 업로드 버퍼에 올림 (CPU). AddPasses: 그림자 깊이 패스 → 클러스터 컬링 계산 셰이더(ClusterCulling.hlsl)
//            (클러스터 버퍼 상태는 렌더 그래프가 추적 — 메시 패스가 읽기로 선언).
//   메시 패스는 GetConstants(b5) / GetLightList(t9) / GetClusterData(t10) / GetShadowMatrices(t11)를 루트 디스크립터로,
//   GetShadowMapSrv(t12)를 테이블로 바인딩한다. 클러스터 화면 크기는 실제 렌더 타깃 크기(픽셀 아트 모드는 저해상도)여야 한다.
//   그림자 타일 배열(Texture2DArray D32)은 그림자 라이트가 처음 나올 때 필요한 장 수만큼 만든다 (그 전엔 1x1 한 장).
class FLocalLightRenderer
{
public:
	~FLocalLightRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 1) 라이트 수집 + 그림자 장 배정 (CPU만)
	void PrepareLights(FScene& Scene, const FCamera& Camera, const FLocalShadowSettings& ShadowSettings);
	// 그림자 장 하나라도 캐스터로 판정하면 true (라이트 영향 구 ∩ 장 프러스텀) — 스킨 팔레트 가시성 판정용 (PrepareLights 뒤)
	bool IntersectsShadowCaster(const FBox& WorldBounds) const;
	// 2) 클러스터 상수 + 라이트/그림자 행렬 목록 업로드 (CPU — 이후 GetConstants/GetLightList 등이 유효)
	void PrepareFrame(const FCamera& Camera, uint32 Width, uint32 Height, const FLocalShadowSettings& ShadowSettings);
	// 3) 렌더 그래프 패스 등록: 로컬 그림자(장이 있으면, 타일 배열 깊이 쓰기) → 클러스터 컬링(계산, 클러스터 버퍼 UAV).
	// 그림자 캐스터 = 프레임 메시 인스턴스 목록 (Upload 완료): 장마다 (정적/스킨)·메시·LOD별 인스턴싱, 스킨은 프레임 팔레트(t15).
	// BreakRootSignature = 컬링 전에 그래픽스 루트를 바꿔 지난 메시 패스의 클러스터 루트 SRV 묶음을 끊는다 (같은 프레임에 다시 그릴 때 디버그 레이어 1003)
	void AddPasses(FRenderGraph& Graph, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes, ID3D12RootSignature* BreakRootSignature,
	               int32 Timer);
	// 메시 패스가 읽는 리소스 (그림자 타일 배열 / 클러스터 버퍼) 가져오기
	FRGResourceRef ImportShadowMap(FRenderGraph& Graph) const;
	FRGResourceRef ImportClusters(FRenderGraph& Graph);

	D3D12_GPU_VIRTUAL_ADDRESS     GetConstants() const { return ConstantsAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS     GetLightList() const { return LightListAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS     GetClusterData() const;
	D3D12_GPU_VIRTUAL_ADDRESS     GetShadowMatrices() const { return ShadowMatricesAddress; }
	const FD3D12DescriptorHandle& GetShadowMapSrv() const { return ShadowSrv; }
	uint32                        GetLightCount() const { return static_cast<uint32>(Lights.size()); }
	uint32                        GetAreaLightCount() const { return AreaLightCount; }
	// 경로 캐시 쿠키 텍스처를 수거 루트로 (씬 렌더러의 루트 제공자가 부른다)
	void                          CollectResourceRoots(FResourceRoots& Roots) const;
	uint32                        GetShadowSliceCount() const { return static_cast<uint32>(ShadowMatrices.size()); }
	// 지난 Prepare의 그림자 드로우 수 / 삼각형 수 (통계)
	uint32                        GetShadowDrawCalls() const { return ShadowDrawCalls; }
	uint64                        GetShadowTriangles() const { return ShadowTriangles; }

	FShadowCasterHook ExtraCasters; // 메시 인스턴스 밖 캐스터 (지형 — FTerrainRenderer::RenderShadow)

private:
	struct FShadowSlice
	{
		FMatrix4x4 ViewProjection;
		FVector3   LightPosition;
		float      Radius = 0.0f;
		FFrustum   Frustum;     // ViewProjection의 프러스텀
		FBox       LightBounds; // 영향 구의 AABB

		bool IsCaster(const FBox& Bounds) const { return Bounds.Intersects(LightBounds) && Frustum.Intersects(Bounds); }
	};

	bool CreateCullPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	bool CreateShadowPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, uint32 Variant); // Variant = DepthVariant*
	void CollectLights(FScene& Scene, const FCamera& Camera);
	bool CreateLtcTextures();
	struct FIesEntry
	{
		FTextureHandle Texture;
		float          MaxCandela = 0.0f;
		bool           bValid     = false;
	};
	// Content 기준 경로 → 캐시 (실패도 기억해 매 프레임 다시 읽지 않는다)
	const FIesEntry& FindIesProfile(const std::string& Path);
	int32            ResolveIesTexture(const FIesEntry& Entry) const;
	int32            ResolveCookieTexture(const std::string& Path);
	void AssignShadows(const FLocalShadowSettings& Settings);
	bool EnsureShadowMap(uint32 Resolution, uint32 Slices);
	void ReleaseShadowMap();
	void RecordShadows(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes);

	FD3D12RHI*        Rhi           = nullptr;
	FShaderLibrary*   ShaderLibrary = nullptr;
	FResourceManager* Resources     = nullptr; // 비소유 (씬 렌더러와 수명 같음)

	FTextureHandle                                  LtcTextures[2];   // 표 1/2 (RGBA32F 64x64, 직접 만듦 — Shutdown에서 해제)
	std::unordered_map<std::string, FIesEntry>      IesProfiles;      // 키: Content 기준 경로 (소문자, '/')
	std::unordered_map<std::string, FTextureHandle> CookieTextures;   // 키: 같은 규칙 (LoadTexture 경로 캐시 — 루트로 등록)
	uint32                                          AreaLightCount = 0;
	int32                                           DirectionalCookie = -1; // 이번 프레임 방향광 쿠키 힙 칸
	FVector4                                        DirectionalCookieU;
	FVector4                                        DirectionalCookieV;

	FD3D12RootSignature ComputeRootSignature;
	FD3D12PipelineState CullPipeline;

	ComPtr<ID3D12Resource> ClusterBuffer; // uint[ClusterCount * ClusterStride]
	D3D12_RESOURCE_STATES  ClusterState = D3D12_RESOURCE_STATE_COMMON;

	// ---- 그림자 (Shadow.hlsl ShadowVS/ShadowSkinnedVS, 루트: 16 상수 + 스킨 팔레트 CBV)
	FD3D12RootSignature    ShadowRootSignature;
	FD3D12PipelineState    ShadowPipelines[DepthVariantCount]; // [GetDepthVariant]: 정적/스킨 × 불투명/Masked
	FMaterialDepthPipelines MaterialPipelines;                 // 그래프 머티리얼 Masked (ShadowMaterialPS, 셰이더 해시별)
	ComPtr<ID3D12Resource> ShadowMap;
	FD3D12DescriptorHeap   ShadowDsvHeap; // 장마다 DSV
	FD3D12DescriptorHandle ShadowSrv;
	uint32                 ShadowMapResolution = 0;
	uint32                 ShadowMapSlices     = 0;
	int32                  BakedDepthBias      = FLocalShadowSettings{}.DepthBias; // PSO에 고정된 바이어스
	float                  BakedSlopeBias      = FLocalShadowSettings{}.SlopeBias;

	std::vector<FLocalLightGpuData> Lights;       // 이번 프레임 목록
	std::vector<float>              LightScores;  // 정렬 키 (카메라 거리 - 반경)
	std::vector<float>              LightOuterAngles; // 스포트 외부 원뿔 / 면광원 그림자 반각 (도, 그림자 투영용)
	std::vector<float>              LightShadowRadius; // 그림자 원평면·캐스터 판정 반경 (면광원 = 경계 구)
	std::vector<uint8>              LightWantsShadow;
	std::vector<FShadowSlice>       ShadowSlices;
	std::vector<FMatrix4x4>         ShadowMatrices; // 장마다 뷰-투영 (t11)
	D3D12_GPU_VIRTUAL_ADDRESS       ConstantsAddress      = 0;
	D3D12_GPU_VIRTUAL_ADDRESS       LightListAddress      = 0;
	D3D12_GPU_VIRTUAL_ADDRESS       ShadowMatricesAddress = 0;
	uint32                          ShadowDrawCalls       = 0;
	FMeshPassBatches                ShadowBatches; // 장마다 재사용
	uint64                          ShadowTriangles       = 0;
};
