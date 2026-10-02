#pragma once

#include "Core/Math/Math.h"
#include "Scene/ResourceHandles.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct FResourceRoots;

class FCamera;
class FMeshInstanceList;
class FResourceManager;
class FScene;
class FStaticMesh;
struct FFoliageAsset;
struct FMaterial;

// 풀·나무 렌더 (Phase 34-3): 폴리지 인스턴스를 프레임 메시 인스턴스 목록에 더해 기존 GPU 인스턴싱 경로(메인/그림자/로컬 그림자)로 그린다.
//   - 에셋별 캐시: 인스턴스 월드 행렬/경계 + 20m 셀 격자 (ChangeCounter가 바뀌면 다시 만든다)
//   - 셀 컬링: 컬링 거리 밖 셀 제외, 메인 프러스텀 ∪ (그림자 거리 안 ∩ 그림자 캐스터 볼륨) 셀만 순회
//   - 인스턴스: 컬링 거리 끝 15%에서 크기를 줄여 사라짐(페이드), LOD는 화면 크기로 직접 (bFixedLod), 그림자 거리 밖은 bCastShadow = false
//   호출: FSceneRenderer가 MeshInstances.Gather 뒤, Upload 전에 Gather (그림자 캐스케이드 준비 뒤)
class FFoliageRenderer
{
public:
	void Init(FResourceManager& InResources) { Resources = &InResources; }
	void Shutdown();
	// 리소스 수거 루트: 직전 Gather에서 쓴 폴리지 타입의 머티리얼 (Phase 37)
	void CollectResourceRoots(FResourceRoots& Roots) const;

	// ShadowCaster(경계): 그림자 캐스터 볼륨과 겹치나 (방향광 캐스케이드 ∪ 로컬 그림자 장)
	void Gather(FScene& Scene, const FCamera& Camera, const FFrustum& Frustum, const std::function<bool(const FBox&)>& ShadowCaster, FMeshInstanceList& OutInstances);

	bool  bEnabled          = true;
	float CullDistanceScale = 1.0f; // 타입 컬링 거리 배율 (--foliage-distance X)

	uint32 GetTotalInstances() const { return TotalInstances; }
	uint32 GetDrawnInstances() const { return DrawnInstances; }       // 메인 프러스텀 안 (그림자 전용 제외)
	uint32 GetShadowOnlyInstances() const { return ShadowOnlyInstances; }
	uint32 GetVisitedCells() const { return VisitedCells; }

private:
	struct FCell
	{
		FBox                Bounds;
		std::vector<uint32> Instances;
	};
	struct FTypeCache
	{
		FMeshHandle             Mesh;
		FMaterialHandle         Material;
		std::vector<FMatrix4x4> Worlds; // 페이드 없는 월드 행렬
		std::vector<FBox>       Bounds;
		std::vector<FCell>      Cells;
		uint64                  Counter = 0; // 만든 때의 FFoliageAsset::TypeCounters 값
	};
	struct FAssetCache
	{
		uint64                  ChangeCounter = 0;
		std::vector<FTypeCache> Types;
		uint64                  LastUsedFrame = 0;
	};

	void            Rebuild(const FFoliageAsset& Asset, FAssetCache& Cache);
	FMeshHandle     ResolveMesh(const std::string& Name);
	FMaterialHandle ResolveMaterial(const std::string& Asset);

	FResourceManager* Resources = nullptr;
	std::unordered_map<const FFoliageAsset*, FAssetCache> Caches;
	std::unordered_map<std::string, FMeshHandle>          Meshes;    // 이름 → 메시 (내장 절차 메시는 렌더러가 만든다)
	std::unordered_map<std::string, FMaterialHandle>      Materials; // 경로 → 머티리얼
	uint64                                                FrameCounter = 0;

	uint32 TotalInstances      = 0;
	uint32 DrawnInstances      = 0;
	uint32 ShadowOnlyInstances = 0;
	uint32 VisitedCells        = 0;
};
