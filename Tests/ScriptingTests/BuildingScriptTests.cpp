#include "Core/Testing/TestFramework.h"
#include "Scene/Building/BuildingConfig.h"
#include "Scene/Building/BuildingScene.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <fstream>

// Lua entity:GenerateBuilding/ClearBuilding/GetBuildingSeed (Scripting/ScriptBuildingBindings.cpp 머리 주석) — 런타임 생성 경로
namespace
{
	namespace fs = std::filesystem;

	struct FBuildingScriptContent
	{
		fs::path Content;
		fs::path Previous;

		FBuildingScriptContent()
		{
			Content = FTestRegistry::GetTempDirectory() / L"ProjectEBuildingScriptTests";
			std::error_code ErrorCode;
			fs::remove_all(Content, ErrorCode);
			const std::string Prefab = R"({"Version":1,"NextId":2,"Entities":[{"Name":"Piece","Parent":-1,"Components":{
				"TransformComponent":{"Position":[0,0,0],"Rotation":[0,0,0,1],"Scale":[1,1,1]},"PrefabLinkComponent":{"Id":"1","Root":-1}}}]})";
			FBuildingConfig Config;
			Config.Width        = 10;
			Config.Depth        = 9;
			Config.Floors       = 2;
			Config.Core         = { 4, 5, 2, 4 };
			Config.UnitMinWidth = 4;
			Config.UnitMaxWidth = 5;
			FBuildingRoomType Room;
			Room.Name      = "Room";
			Room.bRequired = true;
			Room.bEntry    = true;
			Config.Rooms   = { Room };
			for (size_t Piece = 0; Piece < static_cast<size_t>(EBuildingPiece::Prop); ++Piece)
			{
				const std::string Name = ToString(static_cast<EBuildingPiece>(Piece));
				Config.Kit[Piece]      = "Kit/" + Name + ".eprefab";
				fs::create_directories(Content / L"Kit");
				std::ofstream(Content / L"Kit" / (Name + ".eprefab"), std::ios::binary | std::ios::trunc) << Prefab;
			}
			std::ofstream(Content / L"Small.ebuilding", std::ios::binary | std::ios::trunc) << Config.ToJsonString();
			Previous = FPrefabLibrary::Get().GetContentDirectory();
			FPrefabLibrary::Get().SetContentDirectory(Content);
			FPrefabLibrary::Get().Invalidate();
			FBuildingLibrary::Get().Invalidate();
		}
		~FBuildingScriptContent()
		{
			FPrefabLibrary::Get().SetContentDirectory(Previous);
			FPrefabLibrary::Get().Invalidate();
			FBuildingLibrary::Get().Invalidate();
		}
	};
} // namespace

E_TEST(BuildingScript_GenerateAndClear)
{
	FBuildingScriptContent Data;
	FScene                 Scene;
	const FEntity          Building                = Scene.CreateEntity("Tower");
	Scene.GetRegistry().Emplace<FProceduralBuildingComponent>(Building).Config = "Small.ebuilding";
	Scene.CreateEntity("Plain");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Data.Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));

	E_EXPECT_TRUE(Scripts.RunString(R"(
local Tower = Scene.Find("Tower")
local Count = Tower:GenerateBuilding(42)
assert(Count > 40, "인스턴스 수 " .. tostring(Count))
assert(Tower:GetBuildingSeed() == 42)
-- 같은 시드로 다시 생성해도 같은 수 (결정적)
assert(Tower:GenerateBuilding() == Count)
-- 건물 컴포넌트가 없는 엔티티는 Lua 오류
local Ok = pcall(function() Scene.Find("Plain"):GenerateBuilding() end)
assert(not Ok)
)"));
	int32 Groups = 0;
	Scene.GetRegistry().View<FBuildingPartComponent>().Each([&](FEntity, FBuildingPartComponent& Part) { Groups += Part.Floor >= 0 ? 1 : 0; });
	E_EXPECT_TRUE(Groups >= 2);
	E_EXPECT_EQ(Scene.GetRegistry().Get<FProceduralBuildingComponent>(Building).Seed, 42);

	E_EXPECT_TRUE(Scripts.RunString(R"(assert(Scene.Find("Tower"):ClearBuilding() == 2))"));
	Groups = 0;
	Scene.GetRegistry().View<FBuildingPartComponent>().Each([&](FEntity, FBuildingPartComponent&) { ++Groups; });
	E_EXPECT_EQ(Groups, 0);
	Scripts.EndPlay();
}
