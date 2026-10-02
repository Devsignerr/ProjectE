#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Building/BuildingGenerator.h"
#include "Scene/DataLibrary.h"
#include "Scene/DataTable.h"
#include "Scene/Prefab.h"

#include <climits>
#include <cmath>
#include <filesystem>
#include <format>
#include <tuple>
#include <memory>
#include <set>

namespace
{
	FBuildingRoomType MakeType(const char* Name, float Weight, int32 MinArea, bool bRequired)
	{
		FBuildingRoomType Type;
		Type.Name      = Name;
		Type.Weight    = Weight;
		Type.MinArea   = MinArea;
		Type.bRequired = bRequired;
		return Type;
	}

	// 20 × 11, 가운데 복도(4~5줄), 계단실 (9, 6) 3 × 5, 3층. 1층(인덱스 1) 첫 호실은 템플릿 "T"(5 × 4)로 고정
	FBuildingConfig MakeTestConfig()
	{
		FBuildingConfig Config;
		Config.CellSize      = 100.0f;
		Config.Width         = 20;
		Config.Depth         = 11;
		Config.Floors        = 3;
		Config.FloorHeight   = 300.0f;
		Config.Core          = { 9, 6, 3, 5 };
		Config.Corridor      = EBuildingCorridor::Center;
		Config.CorridorWidth = 2;
		Config.UnitMinWidth  = 5;
		Config.UnitMaxWidth  = 7;
		Config.MinRoomSide   = 2;

		FBuildingRoomType Entry = MakeType("Entry", 0.8f, 4, true);
		Entry.bEntry            = true;
		FBuildingRoomType Living = MakeType("Living", 4.0f, 6, true);
		Living.bHub              = true;
		Living.bNeedsWindow      = true;
		Living.WindowChance      = 0.6f;
		Living.OpenTo            = { "Kitchen" };
		Living.Adjacent          = { "Entry" };
		FBuildingRoomType Bath   = MakeType("Bathroom", 1.2f, 4, true);
		Bath.FloorPiece          = "K/Tile.eprefab";
		FBuildingRoomType Kitchen = MakeType("Kitchen", 2.0f, 4, true);
		FBuildingRoomType Bedroom = MakeType("Bedroom", 3.0f, 4, true);
		Bedroom.bNeedsWindow      = true;
		Bedroom.WindowChance      = 0.5f;
		FBuildingRoomType Study   = MakeType("Study", 1.5f, 4, false);
		Study.Chance              = 0.5f;
		FBuildingRoomType Corridor = MakeType("Corridor", 1.0f, 1, false);
		Corridor.WindowChance      = 0.5f;
		Config.Rooms               = { Entry, Living, Bath, Kitchen, Bedroom, Study, Corridor };

		FBuildingUnitTemplate Template;
		Template.Name  = "T";
		Template.Width = 5;
		Template.Depth = 4;
		Template.Rooms = { { "Entry", { 0, 0, 2, 2 } }, { "Bathroom", { 0, 2, 2, 2 } }, { "Living", { 2, 0, 3, 4 } } };
		Config.Templates.push_back(Template);
		Config.FixedUnits.push_back({ 1, 0, "T" });

		for (size_t Piece = 0; Piece < static_cast<size_t>(EBuildingPiece::Prop); ++Piece)
		{
			Config.Kit[Piece] = std::string("K/") + ToString(static_cast<EBuildingPiece>(Piece)) + ".eprefab";
		}
		return Config;
	}

	FBuildingPropRule MakeRule(const char* Name, const char* Room, EBuildingPropPlacement Placement, float Width, float Depth, float Clearance, int32 Min,
	                           int32 Max, bool bBlocking = true)
	{
		FBuildingPropRule Rule;
		Rule.Name      = Name;
		Rule.RoomType  = Room;
		Rule.Prefab    = std::string("P/") + Name + ".eprefab";
		Rule.Placement = Placement;
		Rule.Width     = Width;
		Rule.Depth     = Depth;
		Rule.Clearance = Clearance;
		Rule.MinCount  = Min;
		Rule.MaxCount  = Max;
		Rule.bBlocking = bBlocking;
		return Rule;
	}

	std::vector<FBuildingPropRule> MakeTestProps()
	{
		return {
			MakeRule("Sofa", "Living", EBuildingPropPlacement::Wall, 176.0f, 74.0f, 60.0f, 1, 1),
			MakeRule("Rug", "Living", EBuildingPropPlacement::Center, 200.0f, 150.0f, 0.0f, 1, 1, false),
			MakeRule("Table", "Living", EBuildingPropPlacement::Center, 110.0f, 70.0f, 0.0f, 1, 1),
			MakeRule("Plant", "Living", EBuildingPropPlacement::Corner, 40.0f, 40.0f, 0.0f, 1, 2),
			MakeRule("Bed", "Bedroom", EBuildingPropPlacement::Wall, 172.0f, 202.0f, 40.0f, 1, 1),
			MakeRule("Wardrobe", "Bedroom", EBuildingPropPlacement::Wall, 72.0f, 45.0f, 50.0f, 0, 2),
			MakeRule("Counter", "Kitchen", EBuildingPropPlacement::Wall, 78.0f, 81.0f, 60.0f, 1, 3),
			MakeRule("Toilet", "Bathroom", EBuildingPropPlacement::Wall, 55.0f, 70.0f, 40.0f, 1, 1),
			MakeRule("Shower", "Bathroom", EBuildingPropPlacement::Corner, 90.0f, 90.0f, 0.0f, 0, 1),
			MakeRule("CoatRack", "Entry", EBuildingPropPlacement::Corner, 45.0f, 45.0f, 0.0f, 0, 1),
			MakeRule("Desk", "Study", EBuildingPropPlacement::Wall, 132.0f, 114.0f, 30.0f, 1, 1),
			MakeRule("CorridorPlant", "Corridor", EBuildingPropPlacement::Corner, 40.0f, 40.0f, 0.0f, 0, 2),
		};
	}

	bool IsWallPiece(EBuildingPiece Piece)
	{
		return Piece == EBuildingPiece::Wall || Piece == EBuildingPiece::ExteriorWall || Piece == EBuildingPiece::Door || Piece == EBuildingPiece::Window ||
		       Piece == EBuildingPiece::EntranceDoor;
	}

	bool HasWallPiece(EBuildingEdge Edge)
	{
		return Edge == EBuildingEdge::Wall || Edge == EBuildingEdge::Door || Edge == EBuildingEdge::Window || Edge == EBuildingEdge::EntranceDoor;
	}

	bool IsIntegral(float Value) { return std::abs(Value - std::round(Value)) < 1.0e-3f; }

	bool SamePlacements(const FBuildingResult& A, const FBuildingResult& B)
	{
		if (A.Placements.size() != B.Placements.size())
		{
			return false;
		}
		for (size_t Index = 0; Index < A.Placements.size(); ++Index)
		{
			const FBuildingPlacement& X = A.Placements[Index];
			const FBuildingPlacement& Y = B.Placements[Index];
			if (X.Piece != Y.Piece || X.Asset != Y.Asset || !X.Position.Equals(Y.Position, 1.0e-4f) || X.Yaw != Y.Yaw || !X.Scale.Equals(Y.Scale, 1.0e-6f) ||
			    X.Floor != Y.Floor || X.Unit != Y.Unit || X.Room != Y.Room || X.Tag != Y.Tag)
			{
				return false;
			}
		}
		return true;
	}

	FBuildingResult Generate(const FBuildingConfig& Config, uint32 Seed, const std::vector<FBuildingPropRule>& Props = {}, int32 Section = -1)
	{
		FBuildingGenerateOptions Options;
		Options.Seed         = Seed;
		Options.SectionFloor = Section;
		return GenerateBuilding(Config, Props, Options);
	}
} // namespace

E_TEST(Building_SameSeedSameResult)
{
	const FBuildingConfig                Config = MakeTestConfig();
	const std::vector<FBuildingPropRule> Props  = MakeTestProps();
	const FBuildingResult                A      = Generate(Config, 42, Props);
	const FBuildingResult                B      = Generate(Config, 42, Props);
	E_EXPECT_TRUE(!A.Placements.empty());
	E_EXPECT_TRUE(SamePlacements(A, B));
	for (size_t Floor = 0; Floor < A.Floors.size(); ++Floor)
	{
		E_EXPECT_TRUE(A.Floors[Floor].CellRoom == B.Floors[Floor].CellRoom);
		E_EXPECT_TRUE(A.Floors[Floor].EdgesX == B.Floors[Floor].EdgesX);
		E_EXPECT_TRUE(A.Floors[Floor].EdgesY == B.Floors[Floor].EdgesY);
	}
	// 다른 시드면 배치가 달라진다 (여러 시드 중 하나라도 같으면 실패)
	int32 Different = 0;
	for (uint32 Seed = 43; Seed < 48; ++Seed)
	{
		Different += SamePlacements(A, Generate(Config, Seed, Props)) ? 0 : 1;
	}
	E_EXPECT_EQ(Different, 5);
	// 층마다 시드가 다르면 층 평면도 달라진다 (VaryFloors)
	E_EXPECT_TRUE(A.Floors[0].Seed != A.Floors[2].Seed);
}

E_TEST(Building_AllRoomsReachable)
{
	const FBuildingConfig Config = MakeTestConfig();
	for (uint32 Seed = 1; Seed <= 40; ++Seed)
	{
		const FBuildingResult Result = Generate(Config, Seed);
		E_EXPECT_TRUE(Result.Warnings.empty());
		for (const std::string& Warning : Result.Warnings)
		{
			FTestRegistry::ReportFailure(__FILE__, __LINE__, std::format("시드 {}: {}", Seed, Warning));
		}
		for (const FBuildingFloorPlan& Plan : Result.Floors)
		{
			std::string Problem;
			E_EXPECT_TRUE(AreAllRoomsReachable(Plan, &Problem));
			E_EXPECT_TRUE(Problem.empty());
			// 호실마다 복도로 난 문이 정확히 하나
			std::vector<int32> EntranceDoors(static_cast<size_t>(Plan.UnitCount), 0);
			const auto Count = [&](EBuildingEdge Edge, int32 A, int32 B) {
				if (Edge != EBuildingEdge::Door || A < 0 || B < 0)
				{
					return;
				}
				const FBuildingRoom& RoomA = Plan.Rooms[static_cast<size_t>(A)];
				const FBuildingRoom& RoomB = Plan.Rooms[static_cast<size_t>(B)];
				if (A == Plan.CorridorRoom && RoomB.Unit >= 0)
				{
					++EntranceDoors[static_cast<size_t>(RoomB.Unit)];
				}
				if (B == Plan.CorridorRoom && RoomA.Unit >= 0)
				{
					++EntranceDoors[static_cast<size_t>(RoomA.Unit)];
				}
			};
			for (int32 Y = 0; Y < Plan.Depth; ++Y)
			{
				for (int32 X = 0; X <= Plan.Width; ++X)
				{
					Count(Plan.GetEdgeX(X, Y), Plan.GetRoom(X - 1, Y), Plan.GetRoom(X, Y));
				}
			}
			for (int32 Y = 0; Y <= Plan.Depth; ++Y)
			{
				for (int32 X = 0; X < Plan.Width; ++X)
				{
					Count(Plan.GetEdgeY(X, Y), Plan.GetRoom(X, Y - 1), Plan.GetRoom(X, Y));
				}
			}
			for (int32 Doors : EntranceDoors)
			{
				E_EXPECT_EQ(Doors, 1);
			}
		}
	}
}

E_TEST(Building_ReachabilityDetectsWalledRoom)
{
	FBuildingResult     Result = Generate(MakeTestConfig(), 7);
	FBuildingFloorPlan& Plan   = Result.Floors[0];
	E_EXPECT_TRUE(AreAllRoomsReachable(Plan));
	// 첫 호실 방 하나의 경계를 모두 벽으로 막으면 실패해야 한다
	int32 Target = -1;
	for (int32 Room = 0; Room < static_cast<int32>(Plan.Rooms.size()) && Target < 0; ++Room)
	{
		Target = Plan.Rooms[static_cast<size_t>(Room)].Unit >= 0 ? Room : -1;
	}
	for (int32 Cell : Plan.Rooms[static_cast<size_t>(Target)].Cells)
	{
		const int32 X = Cell % Plan.Width;
		const int32 Y = Cell / Plan.Width;
		for (auto [EdgeX, EdgeY, NeighborX, NeighborY, bX] : { std::tuple{ X, Y, X - 1, Y, true }, std::tuple{ X + 1, Y, X + 1, Y, true },
		                                                        std::tuple{ X, Y, X, Y - 1, false }, std::tuple{ X, Y + 1, X, Y + 1, false } })
		{
			if (Plan.GetRoom(NeighborX, NeighborY) != Target)
			{
				(bX ? Plan.EdgesX[static_cast<size_t>(EdgeY * (Plan.Width + 1) + EdgeX)] : Plan.EdgesY[static_cast<size_t>(EdgeY * Plan.Width + EdgeX)]) =
					EBuildingEdge::Wall;
			}
		}
	}
	std::string Problem;
	E_EXPECT_FALSE(AreAllRoomsReachable(Plan, &Problem));
	E_EXPECT_FALSE(Problem.empty());
}

E_TEST(Building_ModulesMatchGrid)
{
	const FBuildingConfig Config = MakeTestConfig();
	const float           Half   = static_cast<float>(Config.Width) * Config.CellSize * 0.5f;
	const float           HalfD  = static_cast<float>(Config.Depth) * Config.CellSize * 0.5f;
	for (uint32 Seed : { 3u, 11u, 29u })
	{
		const FBuildingResult Result = Generate(Config, Seed, MakeTestProps());
		for (int32 Floor = 0; Floor < static_cast<int32>(Result.Floors.size()); ++Floor)
		{
			const FBuildingFloorPlan& Plan = Result.Floors[static_cast<size_t>(Floor)];
			// 벽 조각: 경계 가운데에 하나씩, 경계 수와 같다
			size_t WallEdges = 0;
			for (EBuildingEdge Edge : Plan.EdgesX)
			{
				WallEdges += HasWallPiece(Edge) ? 1 : 0;
			}
			for (EBuildingEdge Edge : Plan.EdgesY)
			{
				WallEdges += HasWallPiece(Edge) ? 1 : 0;
			}
			size_t WallPieces = 0;
			float  FloorArea  = 0.0f;
			std::set<std::pair<int32, int32>> EdgeKeys;
			for (const FBuildingPlacement& Placement : Result.Placements)
			{
				if (Placement.Floor != Floor)
				{
					continue;
				}
				E_EXPECT_NEAR(Placement.Position.Z, static_cast<float>(Floor) * Config.FloorHeight + ((Placement.Piece == EBuildingPiece::Ceiling || Placement.Piece == EBuildingPiece::Roof) ? Config.FloorHeight : 0.0f), 1.0e-3f);
				const float Gx = (Placement.Position.X + Half) / Config.CellSize;
				const float Gy = (Placement.Position.Y + HalfD) / Config.CellSize;
				if (IsWallPiece(Placement.Piece))
				{
					++WallPieces;
					// 세로 경계: x 정수 + y 반 칸, 가로 경계: 그 반대. 앞(Yaw)도 경계에 수직
					const bool bVertical = IsIntegral(Gx) && IsIntegral(Gy - 0.5f);
					const bool bHorizontal = IsIntegral(Gy) && IsIntegral(Gx - 0.5f);
					E_EXPECT_TRUE(bVertical != bHorizontal);
					const float Yaw = std::abs(Placement.Yaw);
					E_EXPECT_TRUE(bVertical ? (Yaw == 0.0f || Yaw == 180.0f) : Yaw == 90.0f);
					E_EXPECT_TRUE(EdgeKeys.insert({ static_cast<int32>(std::lround(Gx * 2.0f)), static_cast<int32>(std::lround(Gy * 2.0f)) }).second); // 겹친 벽 없음
				}
				if (Placement.Piece == EBuildingPiece::Floor || Placement.Piece == EBuildingPiece::CorridorFloor)
				{
					FloorArea += Placement.Scale.X * Placement.Scale.Y;
					// 사각형 가운데: 칸 수가 홀수면 반 칸, 짝수면 정수 격자
					E_EXPECT_TRUE(IsIntegral(Gx - Placement.Scale.X * 0.5f) && IsIntegral(Gy - Placement.Scale.Y * 0.5f));
				}
			}
			E_EXPECT_EQ(WallPieces, WallEdges);
			// 바닥은 외곽 칸 전체를 한 번씩 (위층은 계단 빈칸 제외)
			int32 Expected = 0;
			for (size_t Cell = 0; Cell < Plan.CellRoom.size(); ++Cell)
			{
				Expected += (Plan.CellRoom[Cell] >= 0 && !(Floor > 0 && Plan.CoreVoid[Cell])) ? 1 : 0;
			}
			E_EXPECT_NEAR(FloorArea, static_cast<float>(Expected), 1.0e-3f);
		}
		// 계단: 꼭대기 층 제외 층마다 하나, 난간은 위층에만
		E_EXPECT_EQ(Result.CountPieces(EBuildingPiece::Stairs), Result.Floors.size() - 1);
		E_EXPECT_TRUE(Result.CountPieces(EBuildingPiece::Railing) > 0);
		E_EXPECT_TRUE(Result.CountPieces(EBuildingPiece::Window) > 0);
		E_EXPECT_EQ(Result.CountPieces(EBuildingPiece::EntranceDoor), size_t(2)); // 1층 복도 양 끝
		// 모서리 기둥: 키트에 있으면 두 축 벽이 만나는 꼭짓점마다
		E_EXPECT_TRUE(Result.CountPieces(EBuildingPiece::Corner) > 0);
	}
}

E_TEST(Building_PropsDoNotOverlap)
{
	const FBuildingConfig Config = MakeTestConfig();
	const float           Half   = static_cast<float>(Config.Width) * Config.CellSize * 0.5f;
	const float           HalfD  = static_cast<float>(Config.Depth) * Config.CellSize * 0.5f;
	size_t                Total  = 0;
	for (uint32 Seed = 1; Seed <= 12; ++Seed)
	{
		const FBuildingResult Result = Generate(Config, Seed, MakeTestProps());
		for (int32 Floor = 0; Floor < static_cast<int32>(Result.Floors.size()); ++Floor)
		{
			const FBuildingFloorPlan&              Plan = Result.Floors[static_cast<size_t>(Floor)];
			std::vector<const FBuildingPlacement*> Props;
			for (const FBuildingPlacement& Placement : Result.Placements)
			{
				if (Placement.Floor == Floor && Placement.Piece == EBuildingPiece::Prop)
				{
					Props.push_back(&Placement);
					// 바닥 자리는 자기 방 칸 안
					for (float X = Placement.FootprintMin.X + 1.0f; X < Placement.FootprintMax.X; X += 10.0f)
					{
						for (float Y = Placement.FootprintMin.Y + 1.0f; Y < Placement.FootprintMax.Y; Y += 10.0f)
						{
							const int32 Cx = static_cast<int32>(std::floor((X + Half) / Config.CellSize));
							const int32 Cy = static_cast<int32>(std::floor((Y + HalfD) / Config.CellSize));
							E_EXPECT_EQ(Plan.GetRoom(Cx, Cy), Placement.Room);
						}
					}
				}
			}
			Total += Props.size();
			for (size_t A = 0; A < Props.size(); ++A)
			{
				if (!Props[A]->bBlocking)
				{
					continue;
				}
				for (size_t B = A + 1; B < Props.size(); ++B)
				{
					if (!Props[B]->bBlocking)
					{
						continue;
					}
					const bool bOverlap = Props[A]->FootprintMin.X < Props[B]->FootprintMax.X - 0.5f && Props[B]->FootprintMin.X < Props[A]->FootprintMax.X - 0.5f &&
					                      Props[A]->FootprintMin.Y < Props[B]->FootprintMax.Y - 0.5f && Props[B]->FootprintMin.Y < Props[A]->FootprintMax.Y - 0.5f;
					E_EXPECT_FALSE(bOverlap);
				}
				// 문 앞 칸(문 양쪽 칸) 비움
				const auto CheckDoorCell = [&](int32 Cx, int32 Cy) {
					if (Plan.GetRoom(Cx, Cy) < 0)
					{
						return;
					}
					const float MinX = static_cast<float>(Cx) * Config.CellSize - Half + 1.0f;
					const float MinY = static_cast<float>(Cy) * Config.CellSize - HalfD + 1.0f;
					const float MaxX = MinX + Config.CellSize - 2.0f;
					const float MaxY = MinY + Config.CellSize - 2.0f;
					const bool  bHit = Props[A]->FootprintMin.X < MaxX && MinX < Props[A]->FootprintMax.X && Props[A]->FootprintMin.Y < MaxY && MinY < Props[A]->FootprintMax.Y;
					E_EXPECT_FALSE(bHit);
				};
				for (int32 Y = 0; Y < Plan.Depth; ++Y)
				{
					for (int32 X = 0; X <= Plan.Width; ++X)
					{
						if (Plan.GetEdgeX(X, Y) == EBuildingEdge::Door || Plan.GetEdgeX(X, Y) == EBuildingEdge::EntranceDoor)
						{
							CheckDoorCell(X - 1, Y);
							CheckDoorCell(X, Y);
						}
					}
				}
				for (int32 Y = 0; Y <= Plan.Depth; ++Y)
				{
					for (int32 X = 0; X < Plan.Width; ++X)
					{
						if (Plan.GetEdgeY(X, Y) == EBuildingEdge::Door || Plan.GetEdgeY(X, Y) == EBuildingEdge::EntranceDoor)
						{
							CheckDoorCell(X, Y - 1);
							CheckDoorCell(X, Y);
						}
					}
				}
			}
		}
	}
	E_EXPECT_TRUE(Total > 50); // 실제로 소품이 놓였다
}

E_TEST(Building_CoreSameOnAllFloors)
{
	const FBuildingConfig Config = MakeTestConfig();
	const FBuildingResult Result = Generate(Config, 5);
	E_EXPECT_EQ(Result.Floors.size(), size_t(3));
	for (const FBuildingFloorPlan& Plan : Result.Floors)
	{
		for (int32 Y = 0; Y < Plan.Depth; ++Y)
		{
			for (int32 X = 0; X < Plan.Width; ++X)
			{
				const int32 Room = Plan.GetRoom(X, Y);
				E_EXPECT_EQ(Config.Core.Contains(X, Y), Room == Plan.CoreRoom);
			}
		}
		E_EXPECT_TRUE(Plan.CoreVoid == Result.Floors[0].CoreVoid);
		E_EXPECT_FALSE(Plan.CoreVoid[static_cast<size_t>(6 * Plan.Width + 10)]); // 복도에 닿은 계단참
		E_EXPECT_TRUE(Plan.CoreVoid[static_cast<size_t>(9 * Plan.Width + 10)]);
	}
}

E_TEST(Building_FixedUnitKeepsTemplate)
{
	const FBuildingConfig        Config   = MakeTestConfig();
	const FBuildingUnitTemplate& Template = Config.Templates.front();
	for (uint32 Seed = 1; Seed <= 10; ++Seed)
	{
		const FBuildingResult     Result = Generate(Config, Seed);
		const FBuildingFloorPlan& Plan   = Result.Floors[1];
		// 고정 호실 0 = 아래 띠(복도 4~5줄 아래, 복도에 닿는 줄 y = 3) 첫 호실, 너비 = 템플릿 너비
		int32 MinX = INT32_MAX, MaxX = INT32_MIN, Cells = 0;
		for (const FBuildingRoom& Room : Plan.Rooms)
		{
			if (Room.Unit != 0)
			{
				continue;
			}
			for (int32 Cell : Room.Cells)
			{
				MinX = std::min(MinX, Cell % Plan.Width);
				MaxX = std::max(MaxX, Cell % Plan.Width);
				++Cells;
			}
		}
		E_EXPECT_EQ(MaxX - MinX + 1, Template.Width);
		E_EXPECT_EQ(Cells, Template.Width * Template.Depth);
		for (const FBuildingRoom& Room : Plan.Rooms)
		{
			if (Room.Unit != 0)
			{
				continue;
			}
			for (int32 Cell : Room.Cells)
			{
				const int32 U = Cell % Plan.Width - MinX;
				const int32 V = 3 - Cell / Plan.Width;
				std::string Expected;
				for (const FBuildingTemplateRoom& TemplateRoom : Template.Rooms)
				{
					if (TemplateRoom.Rect.Contains(U, V))
					{
						Expected = TemplateRoom.Type;
					}
				}
				E_EXPECT_EQ(Room.Type, Expected);
			}
		}
	}
}

E_TEST(Building_SectionAndFloorOverride)
{
	const FBuildingConfig Config = MakeTestConfig();
	const FBuildingResult Result = Generate(Config, 9, {}, 1);
	E_EXPECT_EQ(Result.Floors.size(), size_t(2));
	size_t CeilingsOnSection = 0, FrontWalls = 0, Roofs = 0;
	const float FrontY = -static_cast<float>(Config.Depth) * Config.CellSize * 0.5f;
	for (const FBuildingPlacement& Placement : Result.Placements)
	{
		Roofs += Placement.Piece == EBuildingPiece::Roof ? 1 : 0;
		if (Placement.Floor != 1)
		{
			continue;
		}
		CeilingsOnSection += Placement.Piece == EBuildingPiece::Ceiling ? 1 : 0;
		FrontWalls += (IsWallPiece(Placement.Piece) && std::abs(Placement.Position.Y - FrontY) < 1.0e-3f) ? 1 : 0;
	}
	E_EXPECT_EQ(CeilingsOnSection, size_t(0));
	E_EXPECT_EQ(FrontWalls, size_t(0));
	E_EXPECT_EQ(Roofs, size_t(0));
	// 아래층은 앞면 외벽이 그대로
	size_t LowerFront = 0;
	for (const FBuildingPlacement& Placement : Result.Placements)
	{
		LowerFront += (Placement.Floor == 0 && IsWallPiece(Placement.Piece) && std::abs(Placement.Position.Y - FrontY) < 1.0e-3f) ? 1 : 0;
	}
	E_EXPECT_EQ(LowerFront, static_cast<size_t>(Config.Width));

	// 고정 층 시드: 건물 시드가 달라도 그 층 평면은 같다
	FBuildingConfig Fixed = Config;
	Fixed.FixedFloorSeeds.push_back({ 2, 1234u });
	const FBuildingResult A = Generate(Fixed, 1);
	const FBuildingResult B = Generate(Fixed, 2);
	E_EXPECT_TRUE(A.Floors[2].CellRoom == B.Floors[2].CellRoom);
	E_EXPECT_TRUE(A.Floors[2].EdgesX == B.Floors[2].EdgesX);
	E_EXPECT_FALSE(A.Floors[0].CellRoom == B.Floors[0].CellRoom && A.Floors[0].EdgesX == B.Floors[0].EdgesX && A.Floors[0].EdgesY == B.Floors[0].EdgesY);
}

E_TEST(Building_NonRectangularFootprint)
{
	FBuildingConfig Config = MakeTestConfig();
	Config.FixedUnits.clear();
	// L자: 오른쪽 위 6 × 5 칸을 뺀다
	for (int32 Y = 0; Y < Config.Depth; ++Y)
	{
		for (int32 X = 0; X < Config.Width; ++X)
		{
			if (!(X >= 14 && Y >= 6))
			{
				Config.Cells.emplace_back(X, Y);
			}
		}
	}
	for (uint32 Seed = 1; Seed <= 15; ++Seed)
	{
		const FBuildingResult Result = Generate(Config, Seed, MakeTestProps());
		for (const FBuildingFloorPlan& Plan : Result.Floors)
		{
			E_EXPECT_TRUE(AreAllRoomsReachable(Plan));
			E_EXPECT_EQ(Plan.GetRoom(15, 8), -1);
		}
	}
}

E_TEST(Building_ConfigJsonRoundTrip)
{
	FBuildingConfig Config = MakeTestConfig();
	Config.Cells           = { { 0, 0 }, { 1, 0 } };
	Config.FixedFloorSeeds = { { 1, 77u } };
	Config.PropTable       = "Data/Building/Props.etable";
	Config.Corridor        = EBuildingCorridor::Single;
	Config.bVaryFloors     = false;
	const std::string Json = Config.ToJsonString();
	FBuildingConfig   Read;
	std::string       Error;
	std::vector<std::string> Warnings;
	E_EXPECT_TRUE(FBuildingConfig::FromJsonString(Json, Read, &Error, &Warnings));
	E_EXPECT_TRUE(Warnings.empty());
	E_EXPECT_TRUE(Read == Config);
	E_EXPECT_EQ(Read.ToJsonString(), Json);

	// 형식 오류는 false, 의미 오류는 경고 후 안전한 값
	E_EXPECT_FALSE(FBuildingConfig::FromJsonString("[1, 2", Read, &Error));
	E_EXPECT_FALSE(Error.empty());
	Warnings.clear();
	E_EXPECT_TRUE(FBuildingConfig::FromJsonString(R"({"CellSize": -5, "Width": "x", "Core": {"X": 50, "Width": 3, "Depth": 3}, "Kit": {"Nope": "a"},
		"FixedUnits": [{"Floor": 0, "Unit": 0, "Template": "Missing"}]})", Read, &Error, &Warnings));
	E_EXPECT_EQ(Read.CellSize, 100.0f);
	E_EXPECT_TRUE(Read.Core.IsEmpty());
	E_EXPECT_TRUE(Warnings.size() >= 5);
	const FBuildingResult Result = Generate(Read, 1); // 경고가 있는 설정도 크래시 없이 생성
	E_EXPECT_FALSE(Result.Floors.empty());
}

E_TEST(Building_PropRulesFromDataTable)
{
	const std::string StructJson = R"({"Version":1,"Name":"BuildingProp","Fields":[
		{"Name":"RoomType","Type":"String","Default":"Living"},{"Name":"Prefab","Type":"Asset","Filter":".eprefab","Default":""},
		{"Name":"Placement","Type":"Enum","Values":["Wall","Corner","Center"],"Default":"Wall"},{"Name":"Width","Type":"Float","Default":100},
		{"Name":"Depth","Type":"Float","Default":50},{"Name":"Clearance","Type":"Float","Default":0},{"Name":"MinCount","Type":"Int","Default":1},
		{"Name":"MaxCount","Type":"Int","Default":1},{"Name":"Chance","Type":"Float","Default":1},{"Name":"Blocking","Type":"Bool","Default":true}]})";
	auto Struct = std::make_shared<FDataStruct>();
	E_EXPECT_TRUE(FDataStruct::FromJsonString(StructJson, *Struct));
	const std::string TableJson = R"({"Version":1,"Struct":"S.estruct","Rows":[
		{"Name":"Rug","Values":{"RoomType":"Living","Prefab":"P/Rug.eprefab","Placement":"Center","Width":200,"Depth":150,"Blocking":false,"MaxCount":0}},
		{"Name":"Empty","Values":{"RoomType":"Living"}},
		{"Name":"Bed","Values":{"RoomType":"Bedroom","Prefab":"P/Bed.eprefab","Placement":"Corner","MinCount":2,"MaxCount":1,"Chance":3}}]})";
	FDataTable Table;
	E_EXPECT_TRUE(FDataTable::FromJsonString(TableJson, [&](const std::string&) { return std::shared_ptr<const FDataStruct>(Struct); }, Table));
	std::vector<std::string>             Warnings;
	const std::vector<FBuildingPropRule> Rules = MakeBuildingPropRules(Table, &Warnings);
	E_EXPECT_EQ(Rules.size(), size_t(2));
	E_EXPECT_EQ(Warnings.size(), size_t(1)); // 프리팹 없는 행
	E_EXPECT_TRUE(Rules[0].Placement == EBuildingPropPlacement::Center);
	E_EXPECT_FALSE(Rules[0].bBlocking);
	E_EXPECT_EQ(Rules[0].MaxCount, 1); // Max는 Min 이상
	E_EXPECT_TRUE(Rules[1].Placement == EBuildingPropPlacement::Corner);
	E_EXPECT_EQ(Rules[1].MaxCount, 2);
	E_EXPECT_EQ(Rules[1].Chance, 1.0f);
}

// 예제 프로젝트의 실제 아파트 설정 + 소품 테이블 (에셋 데이터가 규칙에 맞는지)
E_TEST(Building_SampleApartmentAsset)
{
	FPrefabLibrary::Get().SetContentDirectory(FPaths::GetProjectContentDirectory());
	FDataLibrary::Get().Invalidate();
	FBuildingLibrary::Get().Invalidate();
	std::string                                  Error;
	const std::shared_ptr<const FBuildingConfig> Config = FBuildingLibrary::Get().Load("Buildings/Apartment.ebuilding", &Error);
	E_EXPECT_TRUE(Config != nullptr);
	if (!Config)
	{
		FTestRegistry::ReportFailure(__FILE__, __LINE__, Error);
		return;
	}
	const std::shared_ptr<const FDataTable> Table = FDataLibrary::Get().LoadTable(Config->PropTable);
	E_EXPECT_TRUE(Table != nullptr && Table->Struct != nullptr);
	std::vector<std::string>             Warnings;
	const std::vector<FBuildingPropRule> Props = Table ? MakeBuildingPropRules(*Table, &Warnings) : std::vector<FBuildingPropRule>{};
	E_EXPECT_TRUE(Warnings.empty());
	E_EXPECT_TRUE(Props.size() > 20);
	for (uint32 Seed = 1; Seed <= 8; ++Seed)
	{
		const FBuildingResult Result = Generate(*Config, Seed, Props);
		E_EXPECT_EQ(Result.Floors.size(), size_t(5));
		for (const std::string& Warning : Result.Warnings)
		{
			FTestRegistry::ReportFailure(__FILE__, __LINE__, std::format("시드 {}: {}", Seed, Warning));
		}
		E_EXPECT_TRUE(Result.CountPieces(EBuildingPiece::Prop) > 60);
		// 키트·소품 프리팹 파일이 모두 있다
		std::set<std::string> Assets;
		for (const FBuildingPlacement& Placement : Result.Placements)
		{
			Assets.insert(Placement.Asset);
		}
		for (const std::string& Asset : Assets)
		{
			if (!std::filesystem::exists(FPrefabLibrary::Get().ResolveAssetPath(Asset)))
			{
				FTestRegistry::ReportFailure(__FILE__, __LINE__, "없는 프리팹: " + Asset);
			}
		}
	}
}
