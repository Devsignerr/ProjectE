#include "Core/Testing/TestFramework.h"
#include "Scene/DataLibrary.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <fstream>

// Lua Data.* (Scripting/ScriptDataBindings.cpp 머리 주석)
namespace
{
	namespace fs = std::filesystem;

	void WriteText(const fs::path& Path, const char* Text)
	{
		fs::create_directories(Path.parent_path());
		std::ofstream(Path, std::ios::binary | std::ios::trunc) << Text;
	}

	struct FDataScriptContent
	{
		fs::path Content;
		fs::path Previous;

		FDataScriptContent()
		{
			Content = FTestRegistry::GetTempDirectory() / L"ProjectEDataScriptTests";
			std::error_code ErrorCode;
			fs::remove_all(Content, ErrorCode);
			WriteText(Content / L"Data/Item.estruct", R"({ "Version": 1, "Name": "Item", "Fields": [
				{ "Name": "Price", "Type": "Int", "Default": 10 },
				{ "Name": "Weight", "Type": "Float", "Default": 0.5 },
				{ "Name": "Stackable", "Type": "Bool" },
				{ "Name": "Label", "Type": "String" },
				{ "Name": "Kind", "Type": "Enum", "Values": ["Weapon", "Potion"], "Default": "Potion" },
				{ "Name": "Size", "Type": "Vector3", "Default": [1, 2, 3] },
				{ "Name": "UV", "Type": "Vector2" },
				{ "Name": "Tint", "Type": "Color" },
				{ "Name": "Upgrade", "Type": "RowRef", "Table": "Data/Items.etable" },
				{ "Name": "Drops", "Type": "Array", "Element": "RowRef", "Table": "Data/Items.etable" },
				{ "Name": "Tags", "Type": "Array", "Element": "String" } ] })");
			WriteText(Content / L"Data/Items.etable", R"({ "Version": 1, "Struct": "Data/Item.estruct", "Rows": [
				{ "Name": "Sword", "Values": { "Price": 120, "Kind": "Weapon", "Label": "검", "Upgrade": "GreatSword", "Tags": ["melee", "iron"],
				                               "Drops": ["Potion", "Nope", "GreatSword"], "Tint": [1, 0, 0, 1] } },
				{ "Name": "GreatSword", "Values": { "Price": 300, "Kind": "Weapon" } },
				{ "Name": "Potion", "Values": { "Stackable": true, "Weight": 0.25 } } ] })");
			WriteText(Content / L"Data/Config.edata", R"({ "Version": 1, "Struct": "Data/Item.estruct", "Values": { "Price": 7, "UV": [0.5, 1] } })");
			Previous = FPrefabLibrary::Get().GetContentDirectory();
			FPrefabLibrary::Get().SetContentDirectory(Content);
			FDataLibrary::Get().Invalidate();
		}
		~FDataScriptContent()
		{
			FPrefabLibrary::Get().SetContentDirectory(Previous);
			FDataLibrary::Get().Invalidate();
		}
	};
} // namespace

E_TEST(DataScript_GetRowConvertsTypes)
{
	FDataScriptContent Data;
	FScene             Scene;
	FScriptSystem      Scripts;
	Scripts.SetContentDirectory(Data.Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));

	E_EXPECT_TRUE(Scripts.RunString(R"(
local Row = Data.GetRow("Data/Items.etable", "Sword")
assert(Row.Name == "Sword" and Row.Price == 120 and math.type(Row.Price) == "integer")
assert(math.abs(Row.Weight - 0.5) < 1e-6 and Row.Stackable == false and Row.Label == "검" and Row.Kind == "Weapon")
assert(Row.Size.X == 1 and Row.Size.Y == 2 and Row.Size.Z == 3)       -- Vector3 (구조체 기본값)
assert(Row.UV.X == 0 and Row.UV.Y == 0)                              -- Vector2
assert(Row.Tint.X == 1 and Row.Tint.Y == 0 and Row.Tint.W == 1)       -- Color → Vector4
assert(Row.Upgrade == "GreatSword")                                   -- RowRef = 행 이름
assert(#Row.Tags == 2 and Row.Tags[1] == "melee" and Row.Tags[2] == "iron")
assert(Data.GetRow("Data/Items.etable", "Missing") == nil)
assert(Data.HasRow("Data/Items.etable", "Potion") and not Data.HasRow("Data/Items.etable", "Nope"))
)"));
	// 결과는 매번 새 테이블: 고쳐도 다음 호출에 영향 없음
	E_EXPECT_TRUE(Scripts.RunString(R"(
local A = Data.GetRow("Data/Items.etable", "Potion")
A.Price = 999
A.Size.X = 50
local B = Data.GetRow("Data/Items.etable", "Potion")
assert(B.Price == 10 and B.Size.X == 1 and A ~= B)
)"));
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Rows = Data.GetRows("Data/Items.etable")
assert(#Rows == 3 and Rows[1].Name == "Sword" and Rows[2].Name == "GreatSword" and Rows[3].Name == "Potion")
assert(Rows[3].Stackable == true and math.abs(Rows[3].Weight - 0.25) < 1e-6)
local Names = Data.GetRowNames("Data/Items.etable")
assert(#Names == 3 and Names[3] == "Potion")
local Config = Data.Load("Data/Config.edata")
assert(Config.Price == 7 and Config.UV.X == 0.5 and Config.Kind == "Potion" and Config.Name == nil)
-- RowRef 따라가기: 단일 → 행, 배열 → 있는 행만
local Up = Data.ResolveRef("Data/Items.etable", "Sword", "Upgrade")
assert(Up.Name == "GreatSword" and Up.Price == 300)
assert(Data.ResolveRef("Data/Items.etable", "Potion", "Upgrade") == nil)   -- 빈 참조
local Drops = Data.ResolveRef("Data/Items.etable", "Sword", "Drops")
assert(#Drops == 2 and Drops[1].Name == "Potion" and Drops[2].Name == "GreatSword")
assert(math.type(Data.GetGeneration()) == "integer")
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
}

E_TEST(DataScript_MissingFilesAndMalformedCalls)
{
	FDataScriptContent Data;
	FScene             Scene;
	FScriptSystem      Scripts;
	Scripts.SetContentDirectory(Data.Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	// 없는 경로 → nil (Lua 오류 아님)
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(Data.GetRow("Data/Nope.etable", "A") == nil)
assert(Data.GetRows("Data/Nope.etable") == nil and Data.GetRowNames("Data/Nope.etable") == nil)
assert(Data.HasRow("Data/Nope.etable", "A") == false and Data.Load("Data/Nope.edata") == nil)
)"));
	// 잘못된 호출 → Lua 오류
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(not pcall(Data.GetRow, 5, "A"))
assert(not pcall(Data.GetRow, "Data/Items.etable", 3))
assert(not pcall(Data.GetRow, "Data/Items.etable"))
assert(not pcall(Data.GetRows, "Data/Config.edata"))                  -- 확장자 다름
assert(not pcall(Data.Load, "Data/Items.etable"))
assert(not pcall(Data.ResolveRef, "Data/Items.etable", "Sword", "Price")) -- RowRef 필드가 아님
local Ok, Err = pcall(Data.GetRows, {})
assert(not Ok and tostring(Err):find("Data.GetRows", 1, true))
)"));
	Scripts.EndPlay();
}

E_TEST(DataScript_SeesInvalidatedData)
{
	FDataScriptContent Data;
	FScene             Scene;
	FScriptSystem      Scripts;
	Scripts.SetContentDirectory(Data.Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	E_EXPECT_TRUE(Scripts.RunString("Gen = Data.GetGeneration(); assert(Data.GetRow('Data/Items.etable', 'Sword').Price == 120)"));
	WriteText(Data.Content / L"Data/Items.etable", R"({ "Struct": "Data/Item.estruct", "Rows": [ { "Name": "Sword", "Values": { "Price": 1 } } ] })");
	E_EXPECT_TRUE(Scripts.RunString("assert(Data.GetRow('Data/Items.etable', 'Sword').Price == 120)")); // 캐시
	FDataLibrary::Get().Invalidate("Data/Items.etable"); // 에디터 파일 감시가 하는 일
	E_EXPECT_TRUE(Scripts.RunString("assert(Data.GetGeneration() ~= Gen and Data.GetRow('Data/Items.etable', 'Sword').Price == 1)"));
	Scripts.EndPlay();
}

E_TEST(DataScript_SampleContentLoads)
{
	// 예제 프로젝트 데이터 (Projects/Sample/Content/Data/Samples) — 경고 없이 읽히고 Lua로 조회된다
	fs::path Sample;
	for (fs::path Dir = fs::current_path(); !Dir.empty(); Dir = Dir.parent_path())
	{
		if (fs::exists(Dir / L"Projects/Sample/Content/Data/Samples/Weapons.etable"))
		{
			Sample = Dir / L"Projects/Sample/Content";
			break;
		}
		if (Dir == Dir.parent_path())
		{
			break;
		}
	}
	E_EXPECT_FALSE(Sample.empty());
	if (Sample.empty())
	{
		return;
	}
	const fs::path Previous = FPrefabLibrary::Get().GetContentDirectory();
	FPrefabLibrary::Get().SetContentDirectory(Sample);
	FDataLibrary::Get().Invalidate();
	const auto Table = FDataLibrary::Get().LoadTable("Data/Samples/Weapons.etable");
	E_EXPECT_TRUE(Table != nullptr && Table->Struct != nullptr && Table->GetRowCount() >= 3);
	if (Table != nullptr)
	{
		E_EXPECT_EQ(FDataLibrary::Get().ValidateReferences(*Table, "Data/Samples/Weapons.etable").size(), static_cast<size_t>(0));
	}

	FScene        Scene;
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Sample);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Sword = Data.GetRow("Data/Samples/Weapons.etable", "Sword")
assert(Sword and Sword.Damage > 0 and Sword.Rarity == "Common")
local Up = Data.ResolveRef("Data/Samples/Weapons.etable", "Sword", "UpgradesTo")
assert(Up and Up.Name == "Greatsword")
local Balance = Data.Load("Data/Samples/Balance.edata")
assert(Balance and Balance.StartGold > 0 and Data.HasRow("Data/Samples/Weapons.etable", Balance.StarterWeapon))
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
	FPrefabLibrary::Get().SetContentDirectory(Previous);
	FDataLibrary::Get().Invalidate();
}
