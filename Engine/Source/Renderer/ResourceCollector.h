#pragma once

#include "Core/CoreTypes.h"
#include "Scene/ResourceHandles.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class FScene;

// ---------------------------------------------------------------- 리소스 수거 (Phase 37, 도달성 표시 → 지연 해제)
//
// 대상 ("수거 가능"): FResourceManager 경로 캐시가 만든 리소스만 —
//   경로 텍스처(LoadTexture), 경로 머티리얼(LoadMaterial), 모델(FModelResources: 메시/머티리얼/모델이 만든 텍스처), 파티클 에셋(LoadParticleSystem).
//   Create*로 직접 만든 리소스(기본 텍스처/머티리얼, 내장 도형, UI 글꼴, 앱이 만든 머티리얼 등)는 만든 쪽이 소유하므로 수거하지 않고,
//   직접 만든 머티리얼은 그 자체가 루트다 (가리키는 경로 텍스처를 살린다).
// 루트: 수거 시점에 누군가 쓰는 핸들. FResourceManager::AddRootProvider로 등록한 제공자가 FResourceRoots에 채운다
//   (씬 — 메인/플레이/서브 씬/미리보기/썸네일, 에셋 편집기, 렌더러 내부 캐시(지형/폴리지/UI)). 파티클 에셋은 shared_ptr 참조 수로 판정한다.
// 표시: 루트 머티리얼 + 직접 만든 머티리얼 → (메시/머티리얼 중 하나라도 루트인) 모델의 머티리얼 전부 → 표시된 머티리얼과
//   참조된 파티클 에셋의 텍스처. 표시되지 않은 수거 대상은 Destroy*(지연 해제) + 경로 캐시에서 제거되어, 다음 Load*는 새로 만든다.
// 시점: 매 프레임이 아니라 요청 때만 — 맵 전환(FGameWorldTravel), 서브 씬 내림, 에디터 씬 열기/플레이 정지, 콘솔 r.CollectResources.
//   요청은 몇 프레임 뒤(FResourceManager::Tick)에 처리해 새 씬이 한 번 그려지며(에셋 해석·렌더러 캐시) 루트를 채운 뒤 수거한다.

// 수거 루트 (제공자가 채운다). 무효 핸들은 무시된다
struct FResourceRoots
{
	std::unordered_set<FMeshHandle>     Meshes;
	std::unordered_set<FMaterialHandle> Materials;
	std::unordered_set<FTextureHandle>  Textures;

	void Add(FMeshHandle Handle);
	void Add(FMaterialHandle Handle);
	void Add(FTextureHandle Handle);
	// 씬 컴포넌트의 핸들: 정적 메시(메시/머티리얼 — 모델 노드 포함), 데칼 머티리얼
	void AddScene(FScene& Scene);
};

using FResourceRootProvider = std::function<void(FResourceRoots&)>;

// 수거 결과 (로그/통계/테스트)
struct FResourceCollectResult
{
	uint32 Meshes    = 0;
	uint32 Materials = 0;
	uint32 Textures  = 0;
	uint32 Models    = 0;
	uint32 Particles = 0;

	uint32 GetTotal() const { return Meshes + Materials + Textures + Models + Particles; }
};

// 리소스 메모리 통계 (FResourceManager::GetMemoryStats — 바이트는 조회 시점에 리소스 설명으로 계산한다)
struct FResourceMemoryStats
{
	uint32 Textures        = 0;
	uint32 Meshes          = 0;
	uint32 Materials       = 0;
	uint32 Models          = 0; // 모델 캐시 항목
	uint32 ParticleSystems = 0; // 파티클 캐시 항목
	uint32 PathTextures    = 0; // 경로 캐시 텍스처 (수거 대상)
	uint32 PathMaterials   = 0; // 경로 캐시 머티리얼 (수거 대상)
	uint64 TextureBytes    = 0; // GPU 할당 크기 (밉·포맷 정렬 포함)
	uint64 MeshBytes       = 0; // 정점 + 인덱스 + 스킨 버퍼

	// 어댑터 비디오 메모리 (IDXGIAdapter3::QueryVideoMemoryInfo) — 프로세스 전체(렌더 타깃 등 포함)
	bool   bHasVideoMemory = false;
	uint64 LocalUsage      = 0;
	uint64 LocalBudget     = 0;
	uint64 NonLocalUsage   = 0;
	uint64 NonLocalBudget  = 0;

	bool IsOverBudget() const { return bHasVideoMemory && LocalBudget > 0 && LocalUsage > LocalBudget; }
};

// 순수 도달성 계산 (GPU 없이 테스트 가능 — RendererTests ResourceCollector_*)
namespace ResourceGc
{
	struct FMaterialNode
	{
		FMaterialHandle             Handle;
		std::vector<FTextureHandle> Textures;
		bool                        bCollectible = false; // false = 직접 만든 머티리얼 (항상 루트)
	};

	struct FModelNode
	{
		std::wstring                 Key;
		std::vector<FMeshHandle>     Meshes;
		std::vector<FMaterialHandle> Materials;
	};

	struct FParticleNode
	{
		std::wstring                Key;
		bool                        bReferenced = false; // 캐시 밖에서 에셋을 들고 있다 (컴포넌트, 편집기)
		std::vector<FTextureHandle> Textures;
	};

	struct FGraph
	{
		std::vector<FMaterialNode>  Materials;           // 살아 있는 모든 머티리얼
		std::vector<FTextureHandle> CollectibleTextures; // 경로 텍스처 + 모델이 만든 텍스처
		std::vector<FModelNode>     Models;
		std::vector<FParticleNode>  Particles;
	};

	struct FGarbage
	{
		std::vector<std::wstring>    Models;    // 캐시 키
		std::vector<std::wstring>    Particles; // 캐시 키
		std::vector<FMaterialHandle> Materials; // 경로 머티리얼 + 버리는 모델의 머티리얼
		std::vector<FMeshHandle>     Meshes;    // 버리는 모델의 메시
		std::vector<FTextureHandle>  Textures;
	};

	FGarbage Compute(const FGraph& Graph, const FResourceRoots& Roots);

	// 표시용 문자열 (로그 / 콘솔 / 화면 통계 / 통계 창 공용)
	std::string FormatBytes(uint64 Bytes);
	std::vector<std::string> FormatMemoryStats(const FResourceMemoryStats& Stats);
} // namespace ResourceGc
