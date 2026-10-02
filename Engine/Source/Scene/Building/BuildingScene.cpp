#include "Scene/Building/BuildingScene.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/Building/BuildingGenerator.h"
#include "Scene/DataLibrary.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <chrono>
#include <format>
#include <map>
#include <set>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	struct FKeptEntity
	{
		FEntity Entity;
		int32   Floor  = -1;
		int32   Unit   = -1;
		bool    bGroup = false; // 유지를 켠 층/호실 그룹 (그 자리의 새 배치를 만들지 않는다)
	};

	struct FRemovedGroups
	{
		std::vector<FKeptEntity> Kept;
		int32                    Removed = 0;
	};

	bool IsKept(const FRegistry& Registry, FEntity Entity)
	{
		const FBuildingPartComponent* Part = Registry.TryGet<FBuildingPartComponent>(Entity);
		return Part != nullptr && Part->bKeep;
	}

	// 그룹 하위에서 유지 표식 엔티티를 찾는다 (표식이 있으면 그 하위는 더 보지 않는다)
	void CollectKept(const FScene& Scene, FEntity Entity, int32 Floor, int32 Unit, std::vector<FKeptEntity>& Out)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		for (const FEntity Child : Scene.GetChildren(Entity))
		{
			const FBuildingPartComponent* Part = Registry.TryGet<FBuildingPartComponent>(Child);
			const bool                    bGroup = Part != nullptr && Part->Floor >= 0;
			if (Part != nullptr && Part->bKeep)
			{
				Out.push_back({ Child, bGroup ? Part->Floor : Floor, bGroup ? Part->Unit : Unit, bGroup });
				continue;
			}
			CollectKept(Scene, Child, bGroup ? Part->Floor : Floor, bGroup ? Part->Unit : Unit, Out);
		}
	}

	// 기존 생성 그룹을 지운다. 유지 엔티티(그룹 안)는 건물 아래로 옮기고, 유지를 켠 층 그룹은 그대로 둔다(Kept에 bGroup으로 기록)
	FRemovedGroups RemoveGroups(FScene& Scene, FEntity Building)
	{
		FRegistry&           Registry = Scene.GetRegistry();
		FRemovedGroups       Result;
		std::vector<FEntity> Groups;
		for (const FEntity Child : Scene.GetChildren(Building))
		{
			const FBuildingPartComponent* Part = Registry.TryGet<FBuildingPartComponent>(Child);
			if (Part == nullptr || Part->Floor < 0)
			{
				continue; // 그룹 밖 엔티티(유지 표식만 붙은 일반 자식 포함)는 건드리지 않는다
			}
			if (Part->bKeep)
			{
				Result.Kept.push_back({ Child, Part->Floor, Part->Unit, true }); // 고정 층: 제자리
				continue;
			}
			Groups.push_back(Child);
		}
		const size_t FirstMoved = Result.Kept.size();
		for (const FEntity Group : Groups)
		{
			const FBuildingPartComponent& Marker = Registry.Get<FBuildingPartComponent>(Group);
			CollectKept(Scene, Group, Marker.Floor, Marker.Unit, Result.Kept);
		}
		for (size_t Index = FirstMoved; Index < Result.Kept.size(); ++Index)
		{
			Scene.SetParent(Result.Kept[Index].Entity, Building); // 그룹 = 건물 기준 단위 트랜스폼 → 로컬 값 그대로 월드 위치 유지
		}
		for (const FEntity Group : Groups)
		{
			Scene.DestroyEntity(Group);
		}
		Result.Removed = static_cast<int32>(Groups.size());
		return Result;
	}

	FEntity MakeGroup(FScene& Scene, FEntity Parent, const std::string& Name, int32 Floor, int32 Unit)
	{
		const FEntity Group = Scene.CreateEntity(Name);
		Scene.SetParent(Group, Parent);
		FBuildingPartComponent& Marker = Scene.GetRegistry().Emplace<FBuildingPartComponent>(Group);
		Marker.Floor                   = Floor;
		Marker.Unit                    = Unit;
		Marker.bKeep                   = false;
		return Group;
	}
} // namespace

void RegisterBuildingTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	if (Registry.IsRegistered<FProceduralBuildingComponent>())
	{
		return;
	}
	// 실내 절차적 생성 (Scene/Building): 인스펙터의 "생성/다시 생성"이 하위에 층/호실 그룹을 만든다. 생성물은 각 프로세스가 같은 씬 파일로 가진다(복제 안 함)
	Registry.RegisterType<FProceduralBuildingComponent>("ProceduralBuildingComponent", "절차적 건물")
		.Property(&FProceduralBuildingComponent::Config, "Config", "건물 설정").AssetFilter(".ebuilding")
		.Property(&FProceduralBuildingComponent::Seed, "Seed", "시드").Tooltip("같은 시드 + 같은 설정 = 같은 건물")
		.Property(&FProceduralBuildingComponent::Floors, "Floors", "층수 (0 = 설정 값)").Range(0.0f, 100.0f, 1.0f)
		.Property(&FProceduralBuildingComponent::SectionFloor, "SectionFloor", "단면 보기 층 (-1 = 끔)").Range(-1.0f, 99.0f, 1.0f)
		.Tooltip("이 층(0부터)까지만 만들고 그 층 천장과 앞면(-Y) 외벽을 뺀다")
		.NoReplicate()
		.AsComponent();
	Registry.RegisterType<FBuildingPartComponent>("BuildingPartComponent", "건물 생성물 (유지)")
		.Property(&FBuildingPartComponent::bKeep, "Keep", "다시 생성해도 유지")
		.Tooltip("생성 그룹 안 엔티티: 하위 트리째 남긴다. 층/호실 그룹: 그 층/호실을 고정한다 (새로 만들지 않음)")
		.Property(&FBuildingPartComponent::Floor, "Floor", "층 (그룹, 0부터)", PF_ReadOnly)
		.Property(&FBuildingPartComponent::Unit, "Unit", "호실 (그룹, -1 = 층)", PF_ReadOnly)
		.NoReplicate()
		.AsComponent();
}

bool FBuildingSceneBuilder::Generate(FScene& Scene, FEntity Building, FBuildingApplyStats* OutStats, std::string* OutError)
{
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Building) || !Registry.Has<FProceduralBuildingComponent>(Building))
	{
		if (OutError != nullptr)
		{
			*OutError = "절차적 건물 컴포넌트가 없는 엔티티입니다";
		}
		return false;
	}
	const FProceduralBuildingComponent Component = Registry.Get<FProceduralBuildingComponent>(Building);
	if (Component.Config.empty())
	{
		if (OutError != nullptr)
		{
			*OutError = "건물 설정(.ebuilding)이 지정되지 않았습니다";
		}
		return false;
	}
	const std::shared_ptr<const FBuildingConfig> Config = FBuildingLibrary::Get().Load(Component.Config, OutError);
	if (!Config)
	{
		return false;
	}
	FBuildingApplyStats            LocalStats;
	FBuildingApplyStats&           Stats = OutStats != nullptr ? *OutStats : LocalStats;
	Stats                                = FBuildingApplyStats{};
	std::vector<FBuildingPropRule> Props;
	if (!Config->PropTable.empty())
	{
		if (const std::shared_ptr<const FDataTable> Table = FDataLibrary::Get().LoadTable(Config->PropTable))
		{
			Props = MakeBuildingPropRules(*Table, &Stats.Warnings);
		}
		else
		{
			Stats.Warnings.push_back("소품 테이블을 읽지 못했습니다: " + Config->PropTable);
		}
	}
	FBuildingGenerateOptions Options;
	Options.Seed         = static_cast<uint32>(Component.Seed);
	Options.Floors       = Component.Floors;
	Options.SectionFloor = Component.SectionFloor;

	const auto            Start  = std::chrono::steady_clock::now();
	const FBuildingResult Result = GenerateBuilding(*Config, Props, Options);
	Stats.GenerateMs             = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
	Stats.Warnings.insert(Stats.Warnings.end(), Result.Warnings.begin(), Result.Warnings.end());
	Apply(Scene, Building, Result, &Stats);
	for (const std::string& Warning : Stats.Warnings)
	{
		E_LOG(LogScene, Warning, "[건물] {}", Warning);
	}
	E_LOG(LogScene, Display, "[건물] 생성: 시드 {}, {}층, 배치 {}개 → 인스턴스 {}개 (실패 {}, 유지 {}, 고정 자리 {}), 생성 {:.1f}ms + 씬 적용 {:.1f}ms", Component.Seed,
	      Result.Floors.size(), Stats.Placements, Stats.Instances, Stats.Failed, Stats.Kept, Stats.Skipped, Stats.GenerateMs, Stats.ApplyMs);
	return true;
}

void FBuildingSceneBuilder::Apply(FScene& Scene, FEntity Building, const FBuildingResult& Result, FBuildingApplyStats* OutStats)
{
	const auto           Start = std::chrono::steady_clock::now();
	FBuildingApplyStats  LocalStats;
	FBuildingApplyStats& Stats   = OutStats != nullptr ? *OutStats : LocalStats;
	FRemovedGroups       Removed = RemoveGroups(Scene, Building);
	FPrefabLibrary&      Library  = FPrefabLibrary::Get();
	FRegistry&           Registry = Scene.GetRegistry();

	// 고정(유지) 그룹: 층 전체 또는 (층, 호실) 자리 — 그 자리의 새 그룹·배치는 만들지 않는다
	std::set<int32>                   FixedFloors;
	std::set<std::pair<int32, int32>> FixedUnits;
	std::map<int32, FEntity>          KeptFloorGroups;
	for (const FKeptEntity& Entry : Removed.Kept)
	{
		if (Entry.bGroup && Entry.Unit < 0)
		{
			FixedFloors.insert(Entry.Floor);
			KeptFloorGroups[Entry.Floor] = Entry.Entity;
		}
		else if (Entry.bGroup)
		{
			FixedUnits.insert({ Entry.Floor, Entry.Unit });
		}
	}

	// 그룹: 층 → 호실
	std::map<std::pair<int32, int32>, FEntity> Groups;
	for (int32 Floor = 0; Floor < static_cast<int32>(Result.Floors.size()); ++Floor)
	{
		if (FixedFloors.contains(Floor))
		{
			Groups[{ Floor, -1 }] = KeptFloorGroups[Floor];
			continue;
		}
		const FEntity FloorGroup = MakeGroup(Scene, Building, std::format("Floor {}", Floor + 1), Floor, -1);
		Groups[{ Floor, -1 }]    = FloorGroup;
		for (int32 Unit = 0; Unit < Result.Floors[static_cast<size_t>(Floor)].UnitCount; ++Unit)
		{
			if (!FixedUnits.contains({ Floor, Unit }))
			{
				Groups[{ Floor, Unit }] = MakeGroup(Scene, FloorGroup, std::format("Unit {}{:02}", Floor + 1, Unit + 1), Floor, Unit);
			}
		}
	}

	std::set<std::string> FailedAssets;
	Stats.Placements = static_cast<int32>(Result.Placements.size());
	for (const FBuildingPlacement& Placement : Result.Placements)
	{
		if (FixedFloors.contains(Placement.Floor) || FixedUnits.contains({ Placement.Floor, Placement.Unit }))
		{
			++Stats.Skipped;
			continue;
		}
		auto Found = Groups.find({ Placement.Floor, Placement.Unit });
		if (Found == Groups.end())
		{
			Found = Groups.find({ Placement.Floor, -1 });
		}
		const FEntity Parent = Found != Groups.end() ? Found->second : Building;
		std::string   Error;
		const FEntity Root   = Library.Instantiate(Scene, Placement.Asset, Parent, &Error);
		if (!Root.IsValid())
		{
			++Stats.Failed;
			if (FailedAssets.insert(Placement.Asset).second)
			{
				Stats.Warnings.push_back(std::format("프리팹을 만들지 못했습니다 ({}): {}", ToString(Placement.Piece), Error));
			}
			continue;
		}
		FTransformComponent& Transform = Registry.Get<FTransformComponent>(Root);
		Transform.Position             = Placement.Position;
		Transform.Rotation             = FQuat::FromEuler(0.0f, Placement.Yaw, 0.0f);
		Transform.Scale                = Placement.Scale;
		++Stats.Instances;
	}

	// 유지 엔티티를 새 그룹으로 (같은 층·호실 → 층 → 건물). 고정 호실 그룹은 그 층 그룹 아래로, 고정 층 그룹은 제자리
	for (const FKeptEntity& Entry : Removed.Kept)
	{
		++Stats.Kept;
		if (!Registry.IsValid(Entry.Entity) || (Entry.bGroup && Entry.Unit < 0))
		{
			continue;
		}
		auto Found = Entry.bGroup ? Groups.end() : Groups.find({ Entry.Floor, Entry.Unit });
		if (Found == Groups.end())
		{
			Found = Groups.find({ Entry.Floor, -1 });
		}
		if (Found != Groups.end() && Found->second != Entry.Entity)
		{
			Scene.SetParent(Entry.Entity, Found->second);
		}
	}
	Scene.UpdateTransforms();
	Stats.ApplyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
}

int32 FBuildingSceneBuilder::Clear(FScene& Scene, FEntity Building)
{
	const FRemovedGroups Removed = RemoveGroups(Scene, Building);
	Scene.UpdateTransforms();
	return Removed.Removed;
}

