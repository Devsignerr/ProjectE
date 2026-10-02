#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Building/BuildingGenerator.h"
#include "Scene/Building/BuildingScene.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	void WriteText(const fs::path& Path, const std::string& Text)
	{
		fs::create_directories(Path.parent_path());
		std::ofstream(Path, std::ios::binary | std::ios::trunc) << Text;
	}

	// 임시 Content: 조각마다 큐브 하나짜리 프리팹 + 작은 건물 설정 (12 × 9, 2층, 계단실)
	fs::path PrepareContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = FTestRegistry::GetTempDirectory() / L"ProjectEBuildingTests" / L"Content";
			std::error_code ErrorCode;
			fs::remove_all(Path.parent_path(), ErrorCode);
			const std::string Prefab = R"({"Version":1,"NextId":2,"Entities":[{"Name":"Piece","Parent":-1,"Components":{
				"TransformComponent":{"Position":[0,0,0],"Rotation":[0,0,0,1],"Scale":[1,1,1]},
				"StaticMeshComponent":{"MeshAsset":"primitive:cube","MaterialAsset":"","Visible":true},
				"PrefabLinkComponent":{"Id":"1","Root":-1}}}]})";
			FBuildingConfig Config;
			Config.Width         = 12;
			Config.Depth         = 9;
			Config.Floors        = 2;
			Config.Core          = { 5, 5, 2, 4 };
			Config.UnitMinWidth  = 4;
			Config.UnitMaxWidth  = 6;
			FBuildingRoomType Entry;
			Entry.Name      = "Entry";
			Entry.bEntry    = true;
			Entry.bRequired = true;
			FBuildingRoomType Living;
			Living.Name         = "Living";
			Living.bRequired    = true;
			Living.Weight       = 3.0f;
			Living.WindowChance = 0.5f;
			Config.Rooms        = { Entry, Living };
			for (size_t Piece = 0; Piece < static_cast<size_t>(EBuildingPiece::Prop); ++Piece)
			{
				const std::string Name = ToString(static_cast<EBuildingPiece>(Piece));
				Config.Kit[Piece]      = "Kit/" + Name + ".eprefab";
				WriteText(Path / L"Kit" / (Name + ".eprefab"), Prefab);
			}
			Config.Kit[static_cast<size_t>(EBuildingPiece::Corner)].clear();
			WriteText(Path / L"Test.ebuilding", Config.ToJsonString());
			return Path;
		}();
		FPrefabLibrary::Get().SetContentDirectory(Directory);
		FPrefabLibrary::Get().Invalidate();
		FBuildingLibrary::Get().Invalidate();
		return Directory;
	}

	std::vector<FEntity> GroupsOf(FScene& Scene, FEntity Building)
	{
		std::vector<FEntity> Groups;
		for (const FEntity Child : Scene.GetChildren(Building))
		{
			const FBuildingPartComponent* Part = Scene.GetRegistry().TryGet<FBuildingPartComponent>(Child);
			if (Part != nullptr && Part->Floor >= 0)
			{
				Groups.push_back(Child);
			}
		}
		return Groups;
	}

	FEntity MakeBuilding(FScene& Scene, int32 Seed)
	{
		const FEntity Building = Scene.CreateEntity("Building");
		FProceduralBuildingComponent& Component = Scene.GetRegistry().Emplace<FProceduralBuildingComponent>(Building);
		Component.Config                        = "Test.ebuilding";
		Component.Seed                          = Seed;
		return Building;
	}

	// 첫 호실 그룹의 첫 자식
	FEntity FirstUnitPiece(FScene& Scene, FEntity Building, FEntity* OutGroup = nullptr)
	{
		for (const FEntity FloorGroup : GroupsOf(Scene, Building))
		{
			for (const FEntity Child : Scene.GetChildren(FloorGroup))
			{
				const FBuildingPartComponent* Group = Scene.GetRegistry().TryGet<FBuildingPartComponent>(Child);
				if (Group != nullptr && Group->Unit >= 0 && !Scene.GetChildren(Child).empty())
				{
					if (OutGroup != nullptr)
					{
						*OutGroup = Child;
					}
					return Scene.GetChildren(Child).front();
				}
			}
		}
		return NullEntity;
	}
} // namespace

E_TEST(BuildingScene_GenerateCreatesGroups)
{
	PrepareContent();
	FScene              Scene;
	const FEntity       Building = MakeBuilding(Scene, 3);
	FBuildingApplyStats Stats;
	std::string         Error;
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, &Stats, &Error));
	E_EXPECT_TRUE(Error.empty());
	E_EXPECT_EQ(Stats.Failed, 0);
	E_EXPECT_TRUE(Stats.Instances > 100);
	E_EXPECT_EQ(Stats.Instances, Stats.Placements);
	E_EXPECT_EQ(GroupsOf(Scene, Building).size(), size_t(2)); // 층 그룹 2개
	// 인스턴스는 프리팹 연결을 가진 일반 엔티티
	const FEntity Piece = FirstUnitPiece(Scene, Building);
	E_EXPECT_TRUE(Piece.IsValid());
	E_EXPECT_TRUE(FPrefabLibrary::IsInstanceRoot(Scene, Piece));

	// 같은 시드로 다시 생성 = 같은 엔티티 수 (이전 생성물은 교체)
	const uint32 Alive = Scene.GetRegistry().GetAliveCount();
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, nullptr, &Error));
	E_EXPECT_EQ(Scene.GetRegistry().GetAliveCount(), Alive);
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Piece));

	// 씬 직렬화 왕복: 그룹 표식과 건물 컴포넌트가 남는다
	const std::string Json = FSceneSerializer::ToJsonString(Scene);
	FScene            Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, Json));
	size_t Groups = 0, Buildings = 0;
	Loaded.GetRegistry().View<FBuildingPartComponent>().Each([&](FEntity, FBuildingPartComponent& Part) { Groups += Part.Floor >= 0 && !Part.bKeep ? 1 : 0; });
	Loaded.GetRegistry().View<FProceduralBuildingComponent>().Each([&](FEntity, FProceduralBuildingComponent& Component) {
		++Buildings;
		E_EXPECT_EQ(Component.Seed, 3);
	});
	E_EXPECT_EQ(Buildings, size_t(1));
	E_EXPECT_TRUE(Groups > 2);
}

E_TEST(BuildingScene_RegenerateKeepsMarkedEntities)
{
	PrepareContent();
	FScene        Scene;
	const FEntity Building = MakeBuilding(Scene, 3);
	std::string   Error;
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, nullptr, &Error));

	// 손으로 고친 생성물 하나에 유지 표식 + 위치 변경, 그룹 밖 사용자 엔티티 하나
	FEntity       UnitGroup;
	const FEntity Kept = FirstUnitPiece(Scene, Building, &UnitGroup);
	const FBuildingPartComponent OldGroup = Scene.GetRegistry().Get<FBuildingPartComponent>(UnitGroup);
	E_EXPECT_TRUE(Scene.GetRegistry().Emplace<FBuildingPartComponent>(Kept).bKeep); // 인스펙터로 붙이면 기본이 유지
	Scene.GetTransform(Kept).Position = FVector3(12.0f, 34.0f, 56.0f);
	const FEntity UserChild = Scene.CreateEntity("UserLamp");
	Scene.SetParent(UserChild, Building);
	const FEntity Discarded = Scene.GetChildren(UnitGroup).back();

	Scene.GetRegistry().Get<FProceduralBuildingComponent>(Building).Seed = 99;
	FBuildingApplyStats Stats;
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, &Stats, &Error));
	E_EXPECT_EQ(Stats.Kept, 1);
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(Kept));
	E_EXPECT_TRUE(Scene.GetTransform(Kept).Position.Equals(FVector3(12.0f, 34.0f, 56.0f), 1.0e-4f));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(UserChild));
	E_EXPECT_TRUE(Scene.GetParent(UserChild) == Building);
	E_EXPECT_FALSE(Discarded != Kept && Scene.GetRegistry().IsValid(Discarded));
	// 같은 (층, 호실) 새 그룹 아래로 옮겨졌다
	const FEntity NewParent = Scene.GetParent(Kept);
	const FBuildingPartComponent* NewGroup = Scene.GetRegistry().TryGet<FBuildingPartComponent>(NewParent);
	E_EXPECT_TRUE(NewGroup != nullptr);
	if (NewGroup != nullptr)
	{
		E_EXPECT_EQ(NewGroup->Floor, OldGroup.Floor);
		E_EXPECT_EQ(NewGroup->Unit, OldGroup.Unit);
	}

	// 지우기: 그룹은 사라지고 유지 엔티티와 사용자 엔티티는 건물 아래에 남는다
	E_EXPECT_TRUE(FBuildingSceneBuilder::Clear(Scene, Building) == 2);
	E_EXPECT_TRUE(GroupsOf(Scene, Building).empty());
	E_EXPECT_TRUE(Scene.GetParent(Kept) == Building);
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(UserChild));
}

E_TEST(BuildingScene_KeptGroupsArePinned)
{
	PrepareContent();
	FScene        Scene;
	const FEntity Building = MakeBuilding(Scene, 5);
	std::string   Error;
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, nullptr, &Error));

	// 호실 그룹 하나 고정 + 손으로 하나 지우기 (고정 호실은 손으로 꾸민 그대로 남아야 한다)
	FEntity UnitGroup;
	FirstUnitPiece(Scene, Building, &UnitGroup);
	FBuildingPartComponent& Part = Scene.GetRegistry().Get<FBuildingPartComponent>(UnitGroup);
	Part.bKeep                   = true;
	const int32 Floor            = Part.Floor;
	const int32 Unit             = Part.Unit;
	Scene.DestroyEntity(Scene.GetChildren(UnitGroup).front());
	const size_t Children = Scene.GetChildren(UnitGroup).size();

	Scene.GetRegistry().Get<FProceduralBuildingComponent>(Building).Seed = 77;
	FBuildingApplyStats Stats;
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, &Stats, &Error));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(UnitGroup));
	E_EXPECT_EQ(Scene.GetChildren(UnitGroup).size(), Children);
	E_EXPECT_TRUE(Stats.Skipped > 0);
	// 같은 (층, 호실) 그룹은 고정 그룹 하나뿐이고, 새 층 그룹 아래에 있다
	int32 Same = 0;
	Scene.GetRegistry().View<FBuildingPartComponent>().Each([&](FEntity, FBuildingPartComponent& Other) {
		Same += (Other.Floor == Floor && Other.Unit == Unit) ? 1 : 0;
	});
	E_EXPECT_EQ(Same, 1);
	const FBuildingPartComponent* Parent = Scene.GetRegistry().TryGet<FBuildingPartComponent>(Scene.GetParent(UnitGroup));
	E_EXPECT_TRUE(Parent != nullptr && Parent->Floor == Floor && Parent->Unit == -1 && !Parent->bKeep);

	// 층 그룹 고정: 그 층은 다시 생성해도 그대로 (엔티티 수 유지)
	const FEntity FloorGroup = Scene.GetParent(UnitGroup);
	Scene.GetRegistry().Get<FBuildingPartComponent>(FloorGroup).bKeep = true;
	const uint32 Before = Scene.GetRegistry().GetAliveCount();
	E_EXPECT_TRUE(FBuildingSceneBuilder::Generate(Scene, Building, &Stats, &Error));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(FloorGroup));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(UnitGroup));
	E_EXPECT_EQ(Scene.GetRegistry().GetAliveCount(), Before); // 같은 시드: 다른 층은 같은 수로 교체
}

E_TEST(BuildingScene_MissingConfigFails)
{
	PrepareContent();
	FScene        Scene;
	const FEntity Building = MakeBuilding(Scene, 1);
	Scene.GetRegistry().Get<FProceduralBuildingComponent>(Building).Config = "Missing.ebuilding";
	std::string Error;
	E_EXPECT_FALSE(FBuildingSceneBuilder::Generate(Scene, Building, nullptr, &Error));
	E_EXPECT_FALSE(Error.empty());
	const FEntity Plain = Scene.CreateEntity("Plain");
	E_EXPECT_FALSE(FBuildingSceneBuilder::Generate(Scene, Plain, nullptr, &Error));
	// 예제 프로젝트 Content로 되돌린다 (다른 테스트가 실제 에셋을 읽는다)
	FPrefabLibrary::Get().SetContentDirectory(FPaths::GetProjectContentDirectory());
	FBuildingLibrary::Get().Invalidate();
}
