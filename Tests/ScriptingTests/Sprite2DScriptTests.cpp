// Lua 2D 바인딩 (Scripting/ScriptSprite2DBindings.cpp) + 플립북 갱신·이벤트 배달 (FGameWorld::TickPresentation → 다음 게임플레이 틱)
#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	std::filesystem::path GetDirectory() { return FTestRegistry::GetTempDirectory() / L"ProjectESprite2DScriptTests"; }

	// 아틀라스(슬라이스 3개) + Once 플립북 (10fps, 이벤트 0 = Start, 2 = Hit) + 이벤트 기록 스크립트. 반환: 플립북 절대 경로
	std::string WriteAssets()
	{
		const std::filesystem::path Directory = GetDirectory();
		std::filesystem::create_directories(Directory / L"Scripts");
		FSpriteAsset Atlas;
		Atlas.Texture       = "Atlas.png";
		Atlas.TextureWidth  = 48;
		Atlas.TextureHeight = 16;
		Atlas.Slices        = SpriteMath::SliceGrid(48, 16, 16, 16, 0, 0, "F");
		E_EXPECT_TRUE(FSprite2DLibrary::Get().SaveSprite((Directory / L"Atlas.esprite").generic_string(), Atlas));

		FFlipbookAsset Flipbook;
		Flipbook.Sprite = "Atlas.esprite";
		Flipbook.Fps    = 10.0f;
		Flipbook.Loop   = EFlipbookLoopMode::Once;
		for (const FSpriteSlice& Slice : Atlas.Slices)
		{
			Flipbook.Frames.push_back({ Slice.Name, 0.0f, 1.0f });
		}
		Flipbook.Events = { { 0, "Start" }, { 2, "Hit" } };
		const std::string FlipbookPath = (Directory / L"Once.eflipbook").generic_string();
		E_EXPECT_TRUE(FSprite2DLibrary::Get().SaveFlipbook(FlipbookPath, Flipbook));

		std::ofstream File(Directory / L"Scripts/FlipbookRecorder.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Log = "" } }
function T:OnFlipbookEvent_Start(frame) self.Properties.Log = self.Properties.Log .. "S" .. frame .. ";" end
function T:OnFlipbookEvent_Hit(frame) self.Properties.Log = self.Properties.Log .. "H" .. frame .. ";" end
function T:OnFlipbookFinished() self.Properties.Log = self.Properties.Log .. "F;" end
return T
)";
		return FlipbookPath;
	}

	struct FFlipbookModule final : IGameModule
	{
		std::string Log;
		void OnFlipbookEvent(FScene&, FEntity, const std::string& Name, int32 Frame) override { Log += Name + std::to_string(Frame) + ";"; }
		void OnFlipbookFinished(FScene&, FEntity) override { Log += "F;"; }
	};

	void TickFrames(FGameWorld& World, FScene& Scene, int32 Frames)
	{
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			World.TickGameplay(Step, nullptr);
			World.TickPresentation(Scene, Step);
		}
	}
} // namespace

// 플레이 중: 표시 틱 플립북 이벤트가 다음 게임플레이 틱에 조상 스크립트(플립북 엔티티에는 스크립트 없음)와 게임 모듈로 간다.
// Lua PlayFlipbook/StopFlipbook/SetFlipbookTime/GetFlipbookFrame/IsFlipbookFinished
E_TEST(Sprite2DScript_FlipbookEventsAndApi)
{
	const std::string FlipbookPath = WriteAssets();
	FScene            Scene;
	const FEntity     Hero = Scene.CreateEntity("Hero");
	Scene.GetRegistry().Emplace<FScriptComponent>(Hero).ScriptAsset = "Scripts/FlipbookRecorder.lua";
	const FEntity HeroSprite = Scene.CreateEntity("HeroSprite");
	Scene.SetParent(HeroSprite, Hero);
	Scene.GetRegistry().Emplace<FSpriteComponent>(HeroSprite);
	Scene.GetRegistry().Emplace<FFlipbookComponent>(HeroSprite).Flipbook = FlipbookPath;
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FFlipbookModule Module;
	FGameModuleHost Host;
	Host.Attach(Module, "FlipbookTestModule");
	FGameWorld World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, GetDirectory() });
	World.BeginPlay(Scene);
	TickFrames(World, Scene, 40); // 0.3초 재생 + 여유
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Log").String == "S0;H2;F;");
	E_EXPECT_TRUE(Module.Log == "Start0;Hit2;F;");
	E_EXPECT_EQ(Scene.GetRegistry().Get<FSpriteComponent>(HeroSprite).Runtime.FlipbookSliceIndex, 2);
	// GetSpriteSlice = 플립북이 지금 보여 주는 슬라이스(마지막 프레임 = 아틀라스 세 번째 슬라이스)와 그 아틀라스 경로, 스프라이트 없으면 nil
	{
		const auto&       Runtime = Scene.GetRegistry().Get<FSpriteComponent>(HeroSprite).Runtime;
		const std::string Expected = Runtime.FlipbookAtlas->Slices[2].Name;
		E_EXPECT_TRUE(Scripts.RunString("local Slice, Atlas = Scene.Find('HeroSprite'):GetSpriteSlice(); assert(Slice == '" + Expected +
		                                "' and Atlas:find('Atlas.esprite') ~= nil); assert(Scene.Find('Hero'):GetSpriteSlice() == nil)"));
	}

	E_EXPECT_TRUE(Scripts.RunString(R"(
local E = Scene.Find('HeroSprite')
assert(E:IsFlipbookFinished() and E:GetFlipbookFrame() == 2)
assert(E:PlayFlipbook())
assert(not Scene.Find('Hero'):PlayFlipbook()) -- 컴포넌트 없음
)"));
	TickFrames(World, Scene, 40);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Log").String == "S0;H2;F;S0;H2;F;");

	E_EXPECT_TRUE(Scripts.RunString(R"(
local E = Scene.Find('HeroSprite')
assert(E:PlayFlipbook() and E:StopFlipbook())
assert(E:SetFlipbookTime(0.15) and E:GetFlipbookFrame() == 1 and not E:IsFlipbookFinished())
)"));
	TickFrames(World, Scene, 30); // 멈춤: 시작 프레임 이벤트도 진행도 없음 (SetFlipbookTime이 시작으로 표시)
	E_EXPECT_EQ(Scene.GetRegistry().Get<FFlipbookComponent>(HeroSprite).Runtime.Frame, 1);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Log").String == "S0;H2;F;S0;H2;F;");
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
	Host.Unload();
}

// 편집 중(플레이 아님) 표시 틱도 플립북을 진행해 미리보기가 되지만 이벤트는 배달하지 않는다
E_TEST(Sprite2DScript_FlipbookPreviewWithoutPlay)
{
	const std::string FlipbookPath = WriteAssets();
	FScene            Scene;
	const FEntity     Entity = Scene.CreateEntity("Preview");
	Scene.GetRegistry().Emplace<FSpriteComponent>(Entity);
	Scene.GetRegistry().Emplace<FFlipbookComponent>(Entity).Flipbook = FlipbookPath;

	FScriptSystem   Scripts;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, GetDirectory() });
	for (int32 Index = 0; Index < 9; ++Index) // 0.15초
	{
		World.TickPresentation(Scene, Step);
	}
	E_EXPECT_FALSE(World.IsPlaying());
	E_EXPECT_EQ(Scene.GetRegistry().Get<FFlipbookComponent>(Entity).Runtime.Frame, 1);
	E_EXPECT_EQ(Scene.GetRegistry().Get<FSpriteComponent>(Entity).Runtime.FlipbookSliceIndex, 1);
}

// 타일맵: GetTile/SetTile/EraseTile/GetTileTags/WorldToCell/CellToWorld (엔티티 위치 반영), SetTile은 틱 끝에 한 번 TileData로 인코딩된다. SetSpriteFlip
E_TEST(Sprite2DScript_TilemapApi)
{
	const std::filesystem::path Directory = GetDirectory();
	std::filesystem::create_directories(Directory / L"Scripts");
	FTilesetAsset Tileset;
	Tileset.Texture       = "Tiles.png";
	Tileset.TextureWidth  = 64;
	Tileset.TextureHeight = 16;
	FTileDefinition Grass;
	Grass.Id        = 3;
	Grass.Collision = ETileCollision::Full;
	Grass.Tags      = { "Ground", "Grass" };
	Tileset.Tiles   = { Grass };
	const std::string TilesetPath = (Directory / L"ScriptTiles.etileset").generic_string();
	E_EXPECT_TRUE(FSprite2DLibrary::Get().SaveTileset(TilesetPath, Tileset));

	FScene        Scene;
	const FEntity Map = Scene.CreateEntity("Map");
	Scene.GetTransform(Map).Position = FVector3(1000.0f, 0.0f, 0.0f);
	FTilemapComponent& Tilemap       = Scene.GetRegistry().Emplace<FTilemapComponent>(Map);
	Tilemap.Tileset                  = TilesetPath;
	Tilemap.CellSize                 = FVector2(100.0f, 0.0f); // Z는 타일 px(16) × UnitsPerPixel(1)
	const FEntity Sprite = Scene.CreateEntity("Sprite");
	Scene.GetRegistry().Emplace<FSpriteComponent>(Sprite);
	Scene.CreateEntity("Other");
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Directory });
	World.BeginPlay(Scene);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local M = Scene.Find('Map')
assert(M:GetTile(0, 0) == nil)
M:SetTile(2, 1, 3, true, false, true)
local Id, FlipX, FlipY, Rot = M:GetTile(2, 1)
assert(Id == 3 and FlipX and not FlipY and Rot)
local Tags = M:GetTileTags(2, 1)
assert(#Tags == 2 and Tags[1] == 'Ground' and Tags[2] == 'Grass')
assert(#M:GetTileTags(5, 5) == 0)
local X, Y = M:WorldToCell(Vector3(1250, 40, 20))
assert(X == 2 and Y == 1)
X, Y = M:WorldToCell(Vector2(950, -1))
assert(X == -1 and Y == -1)
local P = M:CellToWorld(2, 1)
assert(math.abs(P.X - 1250) < 1e-3 and math.abs(P.Y) < 1e-3 and math.abs(P.Z - 24) < 1e-3)
M:EraseTile(2, 1)
assert(M:GetTile(2, 1) == nil)
M:SetTile(-4, 7, 0)
assert(not pcall(function() M:SetTile(0, 0, -1) end))
assert(not pcall(function() Scene.Find('Other'):GetTile(0, 0) end))
assert(Scene.Find('Sprite'):SetSpriteFlip(true, false))
assert(not Scene.Find('Other'):SetSpriteFlip(true, true))
)"));
	E_EXPECT_TRUE(Tilemap.TileData.empty()); // 인코딩은 게임플레이 틱 끝 (지연 커밋)
	World.TickGameplay(Step, nullptr);
	E_EXPECT_EQ(Tilemap.Runtime.CommitCount, 1u);
	FTilemapData Decoded;
	E_EXPECT_TRUE(FTilemapData::Decode(Tilemap.TileData, Decoded));
	E_EXPECT_EQ(Decoded.GetCellCount(), static_cast<size_t>(1));
	E_EXPECT_EQ(Decoded.Get(-4, 7), TileCell::Make(0));
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FSpriteComponent>(Sprite).bFlipX);
	E_EXPECT_FALSE(Scene.GetRegistry().Get<FSpriteComponent>(Sprite).bFlipY);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 타일 일괄 편집: 묶음 밖 SetTile 여러 번 = 게임플레이 틱 끝에 인코딩 한 번 (그 전에도 GetTile·2D 물리용 Revision은 바로 바뀜),
// BeginTileEdit ~ EndTileEdit = End에서 바로 한 번, SetTiles/FillTiles/ClearTiles, 잘못된 SetTiles는 아무것도 바꾸지 않음,
// 커밋 전 TileData가 밖에서 바뀌면(복제·Undo) 대기 중인 편집은 버린다
E_TEST(Sprite2DScript_TileBatchEditCommitsOnce)
{
	FScene             Scene;
	const FEntity      Map     = Scene.CreateEntity("Map");
	FTilemapComponent& Tilemap = Scene.GetRegistry().Emplace<FTilemapComponent>(Map);
	Tilemap.CellSize           = FVector2(100.0f, 100.0f);

	FScriptSystem   Scripts;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, GetDirectory() });
	World.BeginPlay(Scene);
	const uint32 RevisionBefore = Tilemap.Runtime.Revision;
	E_EXPECT_TRUE(Scripts.RunString(R"(
local M = Scene.Find('Map')
for X = 0, 99 do M:SetTile(X, 0, 1) end
assert(M:GetTile(42, 0) == 1)
)"));
	E_EXPECT_EQ(Tilemap.Runtime.CommitCount, 0u);
	E_EXPECT_TRUE(Tilemap.TileData.empty());
	E_EXPECT_TRUE(Tilemap.Runtime.Revision > RevisionBefore);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_EQ(Tilemap.Runtime.CommitCount, 1u);
	FTilemapData Decoded;
	E_EXPECT_TRUE(FTilemapData::Decode(Tilemap.TileData, Decoded));
	E_EXPECT_EQ(Decoded.GetCellCount(), static_cast<size_t>(100));
	World.TickGameplay(Step, nullptr); // 바뀐 것 없음 → 인코딩 없음
	E_EXPECT_EQ(Tilemap.Runtime.CommitCount, 1u);

	// 묶음: End에서 바로 한 번 (중첩은 가장 바깥에서)
	E_EXPECT_TRUE(Scripts.RunString(R"(
local M = Scene.Find('Map')
M:BeginTileEdit()
M:BeginTileEdit()
assert(M:SetTiles({ {0, 1, 2}, {1, 1, 2, true, false, true}, {0, 0, 1} }) == 2) -- (0,0)은 이미 1
M:FillTiles(5, 3, 3, 2, 4)
M:EndTileEdit()
)"));
	E_EXPECT_EQ(Tilemap.Runtime.CommitCount, 1u);
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Map'):EndTileEdit()"));
	E_EXPECT_EQ(Tilemap.Runtime.CommitCount, 2u);
	E_EXPECT_TRUE(FTilemapData::Decode(Tilemap.TileData, Decoded));
	E_EXPECT_EQ(Decoded.GetCellCount(), static_cast<size_t>(100 + 2 + 6));
	E_EXPECT_EQ(Decoded.Get(1, 1), TileCell::Make(2, TileCell::FlipXBit | TileCell::Rotate90Bit));
	E_EXPECT_EQ(Decoded.Get(4, 3), TileCell::Make(4));

	// 오류: 짝 없는 End, 잘못된 SetTiles는 원자적 (앞 항목도 쓰지 않음), 너무 큰 FillTiles
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Map'):EndTileEdit()"));
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Map'):SetTiles({ {50, 50, 1}, {51, 50, -3} })"));
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Map'):SetTiles({ {50, 50, 1}, 'x' })"));
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Map'):FillTiles(0, 0, 5000, 5000, 1)"));
	E_EXPECT_TRUE(TileCell::IsEmpty(Tilemap.Runtime.Data.Get(50, 50)));
	E_EXPECT_FALSE(Tilemap.Runtime.bDirty);

	// 커밋 전 밖에서 TileData가 바뀌면(복제·Undo) 그 값이 이긴다
	const std::string Before = Tilemap.TileData;
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Map'):ClearTiles(); assert(Scene.Find('Map'):GetTile(0, 0) == nil)"));
	E_EXPECT_TRUE(Tilemap.Runtime.bDirty);
	FTilemapData External;
	External.Set(7, 7, TileCell::Make(9));
	Tilemap.TileData = External.Encode();
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('Map'):GetTile(7, 7) == 9)"));
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Tilemap.TileData == External.Encode());
	E_EXPECT_TRUE(Tilemap.TileData != Before);

	// 묶음을 열어 둔 채 틱이 끝나면 경고 후 닫고 커밋
	E_EXPECT_TRUE(Scripts.RunString("local M = Scene.Find('Map'); M:BeginTileEdit(); M:ClearTiles()"));
	World.TickGameplay(Step, nullptr);
	E_EXPECT_EQ(Tilemap.Runtime.EditDepth, 0);
	E_EXPECT_TRUE(Tilemap.TileData.empty());
	World.EndPlay();
}
