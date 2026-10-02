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
		int32   Floor = -1;
		int32   Unit  = -1;
	};

	// 그룹 하위에서 유지 표식 엔티티를 찾는다 (표식이 있으면 그 하위는 더 보지 않는다)
	void CollectKept(const FScene& Scene, FEntity Entity, int32 Floor, int32 Unit, std::vector<FKeptEntity>& Out)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		for (const FEntity Child : Scene.GetChildren(Entity))
		{
			if (Registry.Has<FBuildingKeepComponent>(Child) && Registry.Get<FBuildingKeepComponent>(Child).bKeep)
			{
				Out.push_back({ Child, Floor, Unit });
				continue;
			}
			int32 ChildFloor = Floor;
			int32 ChildUnit  = Unit;
			if (const FBuildingGroupComponent* Group = Registry.TryGet<FBuildingGroupComponent>(Child))
			{
				ChildFloor = Group->Floor;
				ChildUnit  = Group->Unit;
			}
			CollectKept(Scene, Child, ChildFloor, ChildUnit, Out);
		}
	}

	// 기존 그룹을 지우고 유지 엔티티를 건물 아래로 옮긴다
	std::vector<FKeptEntity> RemoveGroups(FScene& Scene, FEntity Building, int32* OutRemoved)
	{
		FRegistry&           Registry = Scene.GetRegistry();
		std::vector<FEntity> Groups;
		for (const FEntity Child : Scene.GetChildren(Building))
		{
			if (Registry.Has<FBuildingGroupComponent>(Child))
			{
				Groups.push_back(Child);
			}
		}
		std::vector<FKeptEntity> Kept;
		for (const FEntity Group : Groups)
		{
			const FBuildingGroupComponent& Marker = Registry.Get<FBuildingGroupComponent>(Group);
			CollectKept(Scene, Group, Marker.Floor, Marker.Unit, Kept);
		}
		for (const FKeptEntity& Entry : Kept)
		{
			Scene.SetParent(Entry.Entity, Building); // 그룹 = 건물 기준 단위 트랜스폼 → 로컬 값 그대로 월드 위치 유지
		}
		for (const FEntity Group : Groups)
		{
			Scene.DestroyEntity(Group);
		}
		if (OutRemoved != nullptr)
		{
			*OutRemoved = static_cast<int32>(Groups.size());
		}
		return Kept;
	}

	FEntity MakeGroup(FScene& Scene, FEntity Parent, const std::string& Name, int32 Floor, int32 Unit)
	{
		const FEntity Group = Scene.CreateEntity(Name);
		Scene.SetParent(Group, Parent);
		FBuildingGroupComponent& Marker = Scene.GetRegistry().Emplace<FBuildingGroupComponent>(Group);
		Marker.Floor                    = Floor;
		Marker.Unit                     = Unit;
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
	Registry.RegisterType<FBuildingGroupComponent>("BuildingGroupComponent", "건물 생성 그룹")
		.Property(&FBuildingGroupComponent::Floor, "Floor", "층 (0부터)", PF_ReadOnly)
		.Property(&FBuildingGroupComponent::Unit, "Unit", "호실 (-1 = 층 공용)", PF_ReadOnly)
		.NoReplicate()
		.AsComponent();
	Registry.RegisterType<FBuildingKeepComponent>("BuildingKeepComponent", "건물 재생성 유지")
		.Property(&FBuildingKeepComponent::bKeep, "Keep", "유지").Tooltip("다시 생성해도 이 엔티티(하위 포함)를 남긴다")
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
	E_LOG(LogScene, Display, "[건물] 생성: 시드 {}, {}층, 배치 {}개 → 인스턴스 {}개 (실패 {}, 유지 {}), 생성 {:.1f}ms + 씬 적용 {:.1f}ms", Component.Seed,
	      Result.Floors.size(), Stats.Placements, Stats.Instances, Stats.Failed, Stats.Kept, Stats.GenerateMs, Stats.ApplyMs);
	return true;
}

void FBuildingSceneBuilder::Apply(FScene& Scene, FEntity Building, const FBuildingResult& Result, FBuildingApplyStats* OutStats)
{
	const auto                Start = std::chrono::steady_clock::now();
	FBuildingApplyStats       LocalStats;
	FBuildingApplyStats&      Stats = OutStats != nullptr ? *OutStats : LocalStats;
	std::vector<FKeptEntity>  Kept  = RemoveGroups(Scene, Building, nullptr);
	FPrefabLibrary&           Library = FPrefabLibrary::Get();
	FRegistry&                Registry = Scene.GetRegistry();

	// 그룹: 층 → 호실 (배치에 나오는 호실만)
	std::map<std::pair<int32, int32>, FEntity> Groups;
	for (int32 Floor = 0; Floor < static_cast<int32>(Result.Floors.size()); ++Floor)
	{
		const FEntity FloorGroup = MakeGroup(Scene, Building, std::format("Floor {}", Floor + 1), Floor, -1);
		Groups[{ Floor, -1 }]    = FloorGroup;
		for (int32 Unit = 0; Unit < Result.Floors[static_cast<size_t>(Floor)].UnitCount; ++Unit)
		{
			Groups[{ Floor, Unit }] = MakeGroup(Scene, FloorGroup, std::format("Unit {}{:02}", Floor + 1, Unit + 1), Floor, Unit);
		}
	}

	std::set<std::string> FailedAssets;
	Stats.Placements = static_cast<int32>(Result.Placements.size());
	for (const FBuildingPlacement& Placement : Result.Placements)
	{
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

	// 유지 엔티티를 새 그룹으로 (같은 층·호실 → 층 → 건물)
	for (const FKeptEntity& Entry : Kept)
	{
		if (!Registry.IsValid(Entry.Entity))
		{
			continue;
		}
		auto Found = Groups.find({ Entry.Floor, Entry.Unit });
		if (Found == Groups.end())
		{
			Found = Groups.find({ Entry.Floor, -1 });
		}
		if (Found != Groups.end())
		{
			Scene.SetParent(Entry.Entity, Found->second);
		}
		++Stats.Kept;
	}
	Scene.UpdateTransforms();
	Stats.ApplyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
}

int32 FBuildingSceneBuilder::Clear(FScene& Scene, FEntity Building)
{
	int32 Removed = 0;
	RemoveGroups(Scene, Building, &Removed);
	Scene.UpdateTransforms();
	return Removed;
}
