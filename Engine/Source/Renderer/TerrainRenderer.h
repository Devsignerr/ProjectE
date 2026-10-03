#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Buffer.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct FResourceRoots;

class FCamera;
class FD3D12RHI;
class FIblRenderer;
class FLocalLightRenderer;
class FResourceManager;
class FScene;
class FShaderLibrary;
class FShadowRenderer;
struct FMaterial;
struct FTerrainComponent;
struct FTerrainData;

enum class ETerrainPass : uint8
{
	Main,
	MainDepthEqual,
	Prepass,
};

// Phase 33 화면 효과 입력 (Terrain.hlsl이 Mesh.hlsl을 포함해 t16~t22 공간 0을 쓴다)
struct FTerrainScreenInputs
{
	FD3D12DescriptorHandle    AmbientOcclusion;   // t16
	FD3D12DescriptorHandle    DBuffer[3];         // t17~t19
	D3D12_GPU_VIRTUAL_ADDRESS ReflectionCaptures = 0; // t20
	FD3D12DescriptorHandle    CaptureAtlas;       // t21
	FD3D12DescriptorHandle    ScreenReflection;   // t22
	FD3D12DescriptorHandle    RayTracedShadowMask; // t24 (Phase 50 — PerFrame RayTracedShadows일 때 Terrain.hlsl이 읽음)
};

// 레이 트레이싱용 지형 입력 (Phase 50, FRayTracingScene — 높이장 타일 BLAS). 지난 Prepare의 지형마다
struct FTerrainRayTracingInput
{
	const FTerrainData* Data = nullptr;
	FEntity             Entity;
	FVector3            Origin;           // 정점 (0, 0)의 월드 XY + 높이 0의 Z (FTerrainFrame)
	FVector2            CellSize;
	float               HeightScale  = 0.0f;
	bool                bCastShadows = true;
	const FMaterial*    Material     = nullptr; // 레이어 0 (히트 표면 근사 — 레이어 블렌딩 없음)
	float               Tiling       = 1.0f / 400.0f; // 레이어 0: 1 / 텍스처 한 장 크기 (cm) → 정점 UV = 월드 XY × Tiling (Terrain.hlsl LayerTiling)
	FVector4            Color        = FVector4::OneVector; // 정점 색 (레이어 0 머티리얼이 없으면 기본 레이어 색 — 래스터 LayerBaseColor와 같게)
	uint32              ChunkCells   = 64;      // 셀 수를 나누는 청크 크기 (타일 크기 후보)
};

// 지형 렌더러 (Phase 34, Terrain.hlsl). FSceneRenderer가 소유하고 패스 사이에 호출한다:
//   Prepare (메시 인스턴스 수집 뒤, 그림자 전): 높이/가중치 텍스처 생성·바뀐 영역 업로드, 청크 LOD 선택 + 메인 프러스텀 컬링
//   RenderShadow (방향광 캐스케이드 / 로컬 그림자 장마다 — FShadowCasterHook): 장 프러스텀 컬링 후 깊이만
//   RenderMain (HDR 패스, 메시보다 먼저): 레이어 블렌딩 PBR + 방향광 그림자 + 클러스터 로컬 라이트 + IBL
// 청크 = ChunkCells 셀 정사각형(기본 64), LOD k = 정점 간격 2^k (카메라 거리 / 청크 크기). 이웃 LOD 차이의 균열은 스커트로 가린다.
// 같은 패치 크기(LOD) 청크는 인스턴싱 한 번 (청크 목록 구조화 버퍼). 정점 버퍼 없음 — 정점 번호로 격자/스커트 위치를 만든다.
// GPU 텍스처는 데이터 포인터별로 렌더러가 소유하고, FTerrainData::ChangeCounter로 바뀐 영역만 다시 올린다.
class FTerrainRenderer
{
public:
	~FTerrainRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT ColorFormat, DXGI_FORMAT DepthFormat);
	void Shutdown();
	// 리소스 수거 루트: 직전 Prepare에서 쓴 레이어 머티리얼 (Phase 37)
	void CollectResourceRoots(FResourceRoots& Roots) const;
	bool ReloadShaders(bool bForceRecompile);

	void Prepare(FScene& Scene, const FCamera& Camera, const FFrustum& Frustum);
	// 씬 렌더러 패스: Prepass = 깊이 + 법선/거칠기 + 움직임 벡터(MRT, TerrainPrepassPS), MainDepthEqual = 사전 패스 뒤 메인(EQUAL, 쓰기 없음),
	// Main = 사전 패스 없음(LESS + 쓰기). 모두 같은 TerrainVS 바이트코드. Screen = SSAO/DBuffer/캡처/SSR (메시 루트 15~21과 같은 리소스)
	void RenderMain(ETerrainPass Pass, D3D12_GPU_VIRTUAL_ADDRESS PerFrame, D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants, const FShadowRenderer& Shadow,
	                const FIblRenderer& Ibl, const FLocalLightRenderer& LocalLights, const FTerrainScreenInputs& Screen);
	void RenderShadow(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const FFrustum& Frustum, bool bLocalLight);
	// 에디터 선택 아웃라인 마스크 (R8_UNORM 타깃이 바인딩된 상태): 지난 Prepare의 메인 패스 청크로 Entities에 든 지형만
	void RenderMask(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const std::vector<FEntity>& Entities);
	bool HasTerrain(const std::vector<FEntity>& Entities) const; // 지난 Prepare에 Entities 중 지형이 있었나
	// 레이 트레이싱 (Phase 50): 지난 Prepare의 지형 (데이터 + 위치 + 레이어 0 머티리얼)
	void GetRayTracingInputs(std::vector<FTerrainRayTracingInput>& OutInputs) const;

	bool  bEnabled         = true;
	bool  bDebugLod        = false; // LOD별 색 (--terrain-lod-colors)
	float LodDistanceScale = 1.0f;  // 클수록 고해상도 LOD를 더 멀리까지
	int32 ForcedLod        = -1;    // 0 이상이면 모든 청크를 그 LOD로 (확인용)

	uint32 GetDrawCalls() const { return DrawCalls; }       // 지난 RenderMain
	uint64 GetTriangles() const { return Triangles; }
	uint32 GetVisibleChunks() const { return VisibleChunks; }
	uint32 GetTotalChunks() const { return TotalChunks; }
	uint32 GetShadowDrawCalls() const { return ShadowDrawCalls; } // 이번 프레임 그림자 패스 합
	uint64 GetShadowTriangles() const { return ShadowTriangles; }

	// 청크 LOD 선택 (순수 함수, 테스트용): 카메라에서 청크 경계 상자까지 거리 / 청크 월드 크기
	static uint32 SelectChunkLod(float Distance, float ChunkWorldSize, float DistanceScale, uint32 MaxLod);
	// 셀 수(Resolution - 1)를 나누는 청크 크기 (2의 거듭제곱, 최대 64) — 0이면 그릴 수 없음
	static uint32 ComputeChunkCells(uint32 Cells);

private:
	struct FPatch
	{
		FD3D12Buffer IndexBuffer;
		uint32       IndexCount = 0;
	};
	// 데이터별 GPU 상태
	struct FTerrainGpu
	{
		std::unique_ptr<FD3D12Texture> Heights; // R16_UNORM
		std::unique_ptr<FD3D12Texture> Weights; // R8G8B8A8_UNORM
		bool                           bShaderReadable = false; // ALL_SHADER_RESOURCE로 바꿨나 (생성 직후는 PIXEL만)
		uint64                         SeenCounter     = 0;
		uint32                         Resolution      = 0;
		uint32                         ChunkCells      = 0;
		uint32                         ChunksPerSide   = 0;
		uint32                         MaxLod          = 0;
		std::vector<uint16>            ChunkMinHeight; // 청크별 높이 범위 (컬링 경계)
		std::vector<uint16>            ChunkMaxHeight;
		uint64                         LastUsedFrame = 0;
	};
	// 이번 프레임 지형 하나
	struct FFrameTerrain
	{
		FTerrainGpu*              Gpu = nullptr;
		const FTerrainData*       Data = nullptr; // 레이 트레이싱 입력 (GetRayTracingInputs)
		const FMaterial*          Layer0Material = nullptr;
		float                     Layer0Tiling   = 1.0f / 400.0f;
		FVector4                  Layer0Color    = FVector4::OneVector;
		FEntity                   Entity;
		D3D12_GPU_VIRTUAL_ADDRESS Constants = 0;
		D3D12_GPU_DESCRIPTOR_HANDLE LayerTables[4] = {};
		FVector3                  Origin;
		FVector2                  CellSize;
		float                     HeightScale  = 0.0f;
		bool                      bCastShadows = true;
		std::vector<uint8>        ChunkLods;  // 청크별 LOD (메인 카메라 기준, 그림자도 같은 값)
		std::vector<FBox>         ChunkBounds; // 월드 경계
		// 메인 패스: 그리기 묶음 (패치 크기별)
		struct FDraw
		{
			uint32 Quads = 0;
			uint32 First = 0; // 프레임 청크 버퍼 안
			uint32 Count = 0;
		};
		std::vector<FDraw>        MainDraws;
		D3D12_GPU_VIRTUAL_ADDRESS MainChunks = 0; // 메인 패스 청크 목록 (동적 업로드 버퍼)
	};

	bool CreatePipelines(FD3D12PipelineState& OutMain, FD3D12PipelineState& OutShadow, FD3D12PipelineState& OutLocalShadow, bool bForceRecompile);
	bool CreatePassPipelines(FD3D12PipelineState& OutMainEqual, FD3D12PipelineState& OutPrepass, bool bForceRecompile);
	FTerrainGpu* EnsureGpu(const FTerrainData& Data);
	void         UploadRegion(FTerrainGpu& Gpu, const FTerrainData& Data, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY);
	void         UpdateChunkHeights(FTerrainGpu& Gpu, const FTerrainData& Data, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY);
	FPatch*      GetPatch(uint32 Quads);
	FMaterialHandle ResolveLayerMaterial(const std::string& Asset);
	// 청크 목록 → 패치 크기별 묶음 (버퍼 업로드). 반환: 청크 버퍼 GPU 주소
	D3D12_GPU_VIRTUAL_ADDRESS BuildDraws(const FFrameTerrain& Terrain, const FFrustum& Frustum, std::vector<FFrameTerrain::FDraw>& OutDraws);
	void DrawPatches(ID3D12GraphicsCommandList* CommandList, const std::vector<FFrameTerrain::FDraw>& Draws, uint32& InOutDrawCalls, uint64& InOutTriangles);

	FD3D12RHI*        Rhi           = nullptr;
	FShaderLibrary*   ShaderLibrary = nullptr;
	FResourceManager* Resources     = nullptr;
	DXGI_FORMAT       ColorFormat   = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT       DepthFormat   = DXGI_FORMAT_UNKNOWN;

	FD3D12RootSignature RootSignature;
	FD3D12PipelineState MainPipeline;
	FD3D12PipelineState MainEqualPipeline; // 사전 패스 뒤 (깊이 EQUAL)
	FD3D12PipelineState PrepassPipeline;   // 깊이 + 법선 + 움직임 벡터
	FD3D12PipelineState ShadowPipeline;      // 방향광 (직교) 바이어스
	FD3D12PipelineState LocalShadowPipeline; // 로컬 라이트 (원근) 바이어스
	FD3D12PipelineState MaskPipeline;        // 선택 아웃라인 마스크 (처음 쓸 때)

	std::unordered_map<uint32, std::unique_ptr<FPatch>>       Patches;   // 키: 패치 한 변 사각형 수
	std::unordered_map<const FTerrainData*, FTerrainGpu>      GpuData;   // 데이터 포인터 → GPU (라이브러리가 데이터를 공유)
	std::unordered_map<std::string, FMaterialHandle>          LayerMaterials;
	std::vector<FFrameTerrain>                                Frame;     // 이번 프레임 지형
	std::vector<FTerrainChunkGpu>                             ChunkScratch;
	uint64                                                    FrameCounter = 0;
	std::vector<FMaterialHandle>                              FrameLayerMaterials; // 직전 Prepare에서 쓴 레이어 머티리얼 (수거 루트)

	uint32 DrawCalls = 0;
	uint64 Triangles = 0;
	uint32 VisibleChunks = 0;
	uint32 TotalChunks   = 0;
	uint32 ShadowDrawCalls = 0;
	uint64 ShadowTriangles = 0;
};
