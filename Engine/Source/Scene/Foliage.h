#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class FScene;

// ---- 풀·나무 배치 (Phase 34-3) ------------------------------------------------------------------------------------
// 규칙:
//   - 인스턴스는 월드 좌표로 저장한다 (컴포넌트 엔티티 트랜스폼과 무관 — 지형 위에 칠한 그대로). 렌더러가 GPU 인스턴싱으로 그린다
//   - .efoliage(JSON) 하나 = 폴리지 타입 목록(메시/머티리얼/밀도/크기/경사·높이 제한/지면 정렬/거리) + 타입별 인스턴스(base64 실수 배열)
//   - 데이터는 FFoliageLibrary가 경로별로 공유, 바꾸면 FFoliageAsset::MarkChanged (렌더러 셀 캐시/충돌이 다시 만든다)
//   - 편집 되돌리기: 컴포넌트 EditRevision ↔ 에셋 Revision (지형과 같은 방식, Editor/FoliageEditHistory)
struct FFoliageComponent
{
	std::string Asset;            // .efoliage (Content 기준)
	bool        bVisible     = true;
	uint32      EditRevision = 0; // 편집 되돌리기용 (에디터만 바꾼다)
};

struct FFoliageType
{
	std::string Name           = "Type";
	std::string Mesh           = "foliage:grass"; // "foliage:grass|bush|tree|pine|rock" 내장 절차 메시 또는 "primitive:cube"
	std::string Material;                          // .emat (비면 기본 머티리얼 — 내장 메시는 정점 색)
	float       Density        = 20.0f;            // 100 m²(10m x 10m)당 개수
	float       MinScale       = 0.8f;
	float       MaxScale       = 1.2f;
	float       MaxSlope       = 35.0f;            // 도: 이보다 가파르면 안 심는다
	float       MinHeight      = -1.0e7f;          // 월드 Z (cm)
	float       MaxHeight      = 1.0e7f;
	bool        bAlignToNormal = true;             // 지면 법선에 맞춰 기울이기
	bool        bRandomYaw     = true;
	float       ZOffset        = 0.0f;             // cm: 심은 뒤 위아래로 (뿌리를 묻을 때 음수)
	float       CullDistance   = 6000.0f;          // cm: 이보다 멀면 안 그린다 (끝 15%에서 크기를 줄여 사라진다)
	float       ShadowDistance = 3000.0f;          // cm: 이보다 가까운 것만 그림자 (0 = 그림자 없음)
	bool        bCollision     = false;            // 플레이 중 캡슐 충돌 (나무)
	float       CollisionRadius = 25.0f;           // cm (크기 배율 적용)
	float       CollisionHeight = 400.0f;          // cm: 캡슐 전체 높이 (크기 배율 적용)
};

// 인스턴스 하나 (32바이트, 파일에는 실수 8개 그대로)
struct FFoliageInstance
{
	FVector3 Position;           // 월드 (지면)
	float    Yaw   = 0.0f;       // 도
	float    Scale = 1.0f;
	FVector3 Normal = FVector3::UpVector; // 지면 법선 (정렬용)
};
static_assert(sizeof(FFoliageInstance) == 32);

uint64 NextFoliageChangeCounter();

struct FFoliageAsset
{
	std::vector<FFoliageType>                  Types;
	std::vector<std::vector<FFoliageInstance>> Instances; // 타입별 (Types와 같은 길이)
	uint32                                     Revision = 0;

	// ---- 런타임
	uint64              ChangeCounter = 1;
	std::vector<uint64> TypeCounters; // 타입별 변경 번호 (렌더러가 바뀐 타입만 다시 만든다)
	bool                bUnsaved      = false;
	// TypeIndex < 0 이면 모든 타입 (타입 목록 자체가 바뀌었을 때 포함)
	void MarkChanged(int32 TypeIndex = -1)
	{
		ChangeCounter = NextFoliageChangeCounter(); // 모든 에셋 공용 단조 번호 (해제된 주소가 재사용돼도 캐시가 착각하지 않게)
		bUnsaved = true;
		EnsureInstanceLists();
		for (size_t Index = 0; Index < TypeCounters.size(); ++Index)
		{
			if (TypeIndex < 0 || static_cast<size_t>(TypeIndex) == Index)
			{
				TypeCounters[Index] = ChangeCounter;
			}
		}
	}
	size_t GetInstanceCount() const;
	void   EnsureInstanceLists()
	{
		Instances.resize(Types.size());
		TypeCounters.resize(Types.size(), ChangeCounter);
	}
};

// 지면 질의: 월드 XY → 지면 높이/법선. 없으면 false (지형 밖 등)
using FFoliageSurfaceQuery = std::function<bool(float WorldX, float WorldY, FVector3& OutPosition, FVector3& OutNormal)>;

namespace FoliageMath
{
	// 인스턴스 월드 행렬: 크기 → (Yaw → 법선 정렬) 회전 → 위치 (+ZOffset). FadeScale은 거리 페이드 배율
	FMatrix4x4 MakeWorldMatrix(const FFoliageInstance& Instance, const FFoliageType& Type, float FadeScale);
	// 거리 페이드: 컬링 거리의 끝 FadeFraction 구간에서 1 → 0. 컬링 거리 밖이면 0
	float ComputeFade(float Distance, float CullDistance, float FadeFraction = 0.15f);
	// 경사·높이 제한 통과?
	bool AcceptsSurface(const FFoliageType& Type, const FVector3& Position, const FVector3& Normal);
	// 원 안에 목표 밀도까지 무작위로 심는다 (이미 있는 수를 세어 모자란 만큼, 이번 호출 최대 MaxAdd). 반환: 추가 수
	uint32 Paint(FFoliageAsset& Asset, uint32 TypeIndex, const FVector2& Center, float Radius, float DensityScale, uint32 MaxAdd,
	             const FFoliageSurfaceQuery& Surface, std::mt19937& Random);
	// 원 안 인스턴스 지우기 (TypeIndex가 범위 밖이면 모든 타입). 반환: 지운 수
	uint32 Erase(FFoliageAsset& Asset, uint32 TypeIndex, const FVector2& Center, float Radius);
	// 원 안 인스턴스 수
	uint32 CountInCircle(const std::vector<FFoliageInstance>& Instances, const FVector2& Center, float Radius);
}

namespace FoliageIO
{
	constexpr int32 Version = 1;
	std::string ToJsonString(const FFoliageAsset& Asset);
	bool        FromJsonString(std::string_view Json, FFoliageAsset& OutAsset, std::string* OutError);
	bool        SaveToFile(const FFoliageAsset& Asset, const std::filesystem::path& Path);
	bool        LoadFromFile(const std::filesystem::path& Path, FFoliageAsset& OutAsset, std::string* OutError);
}

// 경로별 공유 폴리지 에셋 (엔진 전역). 경로는 Content 기준 (절대 경로도 허용)
class FFoliageLibrary
{
public:
	static constexpr const wchar_t* Extension = L".efoliage";
	static FFoliageLibrary&         Get();

	void                  SetContentDirectory(const std::filesystem::path& Directory);
	std::filesystem::path ResolveAssetPath(const std::string& Asset) const;
	std::string           MakeAssetPath(const std::filesystem::path& AbsolutePath) const;

	std::shared_ptr<FFoliageAsset> Load(const std::string& Asset);
	std::shared_ptr<FFoliageAsset> Find(const std::string& Asset) const;
	std::shared_ptr<FFoliageAsset> Create(const std::string& Asset, FFoliageAsset Initial);
	bool                           Save(const std::string& Asset);
	void                           SaveAllUnsaved();
	void                           OnAssetMoved(const std::filesystem::path& From, const std::filesystem::path& To);
	void                           Invalidate(const std::string& Asset);
	void                           Clear() { Cache.clear(); }

private:
	std::filesystem::path                                           ContentDirectory;
	std::unordered_map<std::string, std::shared_ptr<FFoliageAsset>> Cache;
};

struct FFoliageInstanceSet
{
	FEntity                  Entity;
	const FFoliageComponent* Component = nullptr;
	FFoliageAsset*           Asset     = nullptr;
};
void GatherFoliage(FScene& Scene, std::vector<FFoliageInstanceSet>& OutSets);

// 리플렉션 등록 (RegisterSceneTypes 끝에서 부른다)
void RegisterFoliageTypes();
