#include "Scene/Building/BuildingGenerator.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>
#include <map>
#include <numeric>

namespace
{
	// ---------------------------------------------------------------- 난수 (SplitMix64 — 플랫폼/표준 라이브러리와 무관하게 같은 수열)

	uint64 Mix64(uint64 Value)
	{
		Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ull;
		Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBull;
		return Value ^ (Value >> 31);
	}

	struct FRng
	{
		uint64 State = 0;

		explicit FRng(uint64 Seed)
			: State(Mix64(Seed + 0x9E3779B97F4A7C15ull))
		{
		}

		uint64 Next()
		{
			State += 0x9E3779B97F4A7C15ull;
			return Mix64(State);
		}
		float Float() { return static_cast<float>(Next() >> 40) * (1.0f / 16777216.0f); } // [0, 1)
		int32 Range(int32 Min, int32 Max)                                                 // [Min, Max]
		{
			if (Max <= Min)
			{
				return Min;
			}
			return Min + static_cast<int32>(Next() % static_cast<uint64>(Max - Min + 1));
		}
		template <typename T>
		void Shuffle(std::vector<T>& Values)
		{
			for (size_t Index = Values.size(); Index > 1; --Index)
			{
				const size_t Other = static_cast<size_t>(Next() % Index);
				std::swap(Values[Index - 1], Values[Other]);
			}
		}
	};

	// ---------------------------------------------------------------- 공용

	constexpr int32 GRoomSplitTries = 6; // 호실마다 방 분할 시도 수 (가장 높은 배정 점수)
	constexpr int32 GDirX[4]        = { -1, 1, 0, 0 }; // -X, +X, -Y, +Y
	constexpr int32 GDirY[4] = { 0, 0, -1, 1 };

	bool HasWallPiece(EBuildingEdge Edge)
	{
		return Edge == EBuildingEdge::Wall || Edge == EBuildingEdge::Door || Edge == EBuildingEdge::Window || Edge == EBuildingEdge::EntranceDoor;
	}

	bool IsPassable(EBuildingEdge Edge)
	{
		return Edge == EBuildingEdge::None || Edge == EBuildingEdge::Open || Edge == EBuildingEdge::Door;
	}

	float YawFromDirection(int32 Dir) // 앞 방향 → Yaw
	{
		switch (Dir)
		{
		case 0:  return 180.0f;
		case 1:  return 0.0f;
		case 2:  return -90.0f;
		default: return 90.0f;
		}
	}

	struct FCellRect
	{
		int32 X = 0, Y = 0, W = 0, D = 0;
	};

	// 칸 집합(마스크)을 사각형으로 (가로 줄 → 아래로 같은 줄이면 늘림) — 결정적
	std::vector<FCellRect> DecomposeRects(int32 Width, int32 Depth, std::vector<bool> Mask)
	{
		std::vector<FCellRect> Rects;
		for (int32 Y = 0; Y < Depth; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				if (!Mask[static_cast<size_t>(Y * Width + X)])
				{
					continue;
				}
				int32 W = 1;
				while (X + W < Width && Mask[static_cast<size_t>(Y * Width + X + W)])
				{
					++W;
				}
				int32 D = 1;
				for (;; ++D)
				{
					if (Y + D >= Depth)
					{
						break;
					}
					bool bRow = true;
					for (int32 Dx = 0; Dx < W && bRow; ++Dx)
					{
						bRow = Mask[static_cast<size_t>((Y + D) * Width + X + Dx)];
					}
					if (!bRow)
					{
						break;
					}
				}
				for (int32 Dy = 0; Dy < D; ++Dy)
				{
					for (int32 Dx = 0; Dx < W; ++Dx)
					{
						Mask[static_cast<size_t>((Y + Dy) * Width + X + Dx)] = false;
					}
				}
				Rects.push_back({ X, Y, W, D });
			}
		}
		return Rects;
	}

	struct FRect2
	{
		float MinX = 0.0f, MinY = 0.0f, MaxX = 0.0f, MaxY = 0.0f;

		bool Overlaps(const FRect2& Other, float Tolerance = 0.5f) const
		{
			return MinX < Other.MaxX - Tolerance && Other.MinX < MaxX - Tolerance && MinY < Other.MaxY - Tolerance && Other.MinY < MaxY - Tolerance;
		}
		bool ContainsPoint(float X, float Y) const { return X >= MinX && X <= MaxX && Y >= MinY && Y <= MaxY; }
		FRect2 Expanded(float Amount) const { return { MinX - Amount, MinY - Amount, MaxX + Amount, MaxY + Amount }; }
	};

	// ---------------------------------------------------------------- 층 생성

	struct FUnitFrame
	{
		int32 MinX    = 0;
		int32 RowNear = 0; // 복도에 닿는 줄 (v = 0)
		int32 VSign   = 1;
	};

	struct FUnitInfo
	{
		std::vector<int32> Cells;
		FUnitFrame         Frame;
		int32              Columns = 0;
	};

	class FFloorBuilder
	{
	public:
		FFloorBuilder(const FBuildingConfig& InConfig, const std::vector<FBuildingPropRule>& InProps, int32 InFloor, int32 InFloorCount,
		              int32 InSectionFloor, uint32 InSeed, FBuildingResult& InResult)
			: Config(InConfig)
			, Props(InProps)
			, Floor(InFloor)
			, FloorCount(InFloorCount)
			, bSection(InFloor == InSectionFloor)
			, Rng(InSeed)
			, Result(InResult)
			, W(InConfig.Width)
			, D(InConfig.Depth)
		{
			Plan.Width = W;
			Plan.Depth = D;
			Plan.Seed  = InSeed;
		}

		FBuildingFloorPlan Build()
		{
			BuildSkeleton();
			BuildUnits();
			for (int32 Unit = 0; Unit < static_cast<int32>(Units.size()); ++Unit)
			{
				BuildRooms(Unit);
			}
			Plan.UnitCount = static_cast<int32>(Units.size());
			BuildEdges();
			if (std::string Problem; !AreAllRoomsReachable(Plan, &Problem))
			{
				Warn("연결성 검사 실패: " + Problem);
			}
			PlaceModules();
			PlaceAllProps();
			return std::move(Plan);
		}

	private:
		const FBuildingConfig&                Config;
		const std::vector<FBuildingPropRule>& Props;
		int32                                 Floor;
		int32                                 FloorCount;
		bool                                  bSection;
		FRng                                  Rng;
		FBuildingResult&                      Result;
		int32                                 W;
		int32                                 D;
		FBuildingFloorPlan                    Plan;
		std::vector<FUnitInfo>                Units;
		std::vector<int32>                    RoomTypeIndex; // Rooms 번호 → Config.Rooms 번호 (-1 = 없음)
		int32                                 CorridorRow0 = 0;
		int32                                 CorridorRows = 0;

		void Warn(const std::string& Message) { Result.Warnings.push_back(std::format("{}층: {}", Floor + 1, Message)); }

		int32 CellIndex(int32 X, int32 Y) const { return Y * W + X; }
		bool  Inside(int32 X, int32 Y) const { return Config.IsInside(X, Y); }
		int32 RoomAt(int32 X, int32 Y) const { return Plan.GetRoom(X, Y); }

		int32 AddRoom(std::string Type, int32 Unit)
		{
			FBuildingRoom Room;
			Room.Type = std::move(Type);
			Room.Unit = Unit;
			Plan.Rooms.push_back(std::move(Room));
			const FBuildingRoomType* RoomType = Config.FindRoomType(Plan.Rooms.back().Type);
			RoomTypeIndex.push_back(RoomType != nullptr ? static_cast<int32>(RoomType - Config.Rooms.data()) : -1);
			return static_cast<int32>(Plan.Rooms.size()) - 1;
		}

		// 칸 → 방 (방 칸 목록에도 넣는다 — 호출 순서가 칸 번호 오름차순이어야 목록이 정렬된다)
		void AssignCell(int32 Cell, int32 Room)
		{
			Plan.CellRoom[static_cast<size_t>(Cell)] = Room;
			Plan.Rooms[static_cast<size_t>(Room)].Cells.push_back(Cell);
		}

		const FBuildingRoomType* TypeOf(int32 Room) const
		{
			const int32 TypeIndex = RoomTypeIndex[static_cast<size_t>(Room)];
			return TypeIndex >= 0 ? &Config.Rooms[static_cast<size_t>(TypeIndex)] : nullptr;
		}

		// ---- 1. 뼈대
		void BuildSkeleton()
		{
			Plan.CellRoom.assign(static_cast<size_t>(W * D), -1);
			Plan.CoreVoid.assign(static_cast<size_t>(W * D), false);
			if (Config.Corridor != EBuildingCorridor::None)
			{
				CorridorRows = std::min(Config.CorridorWidth, D);
				CorridorRow0 = Config.Corridor == EBuildingCorridor::Center ? (D - CorridorRows) / 2 : 0;
				Plan.CorridorRoom = AddRoom("Corridor", -1);
			}
			if (!Config.Core.IsEmpty())
			{
				Plan.CoreRoom = AddRoom("Core", -1);
			}
			std::vector<int32> Unassigned;
			for (int32 Y = 0; Y < D; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					if (!Inside(X, Y))
					{
						continue;
					}
					if (Plan.CoreRoom >= 0 && Config.Core.Contains(X, Y))
					{
						AssignCell(CellIndex(X, Y), Plan.CoreRoom);
					}
					else if (Plan.CorridorRoom >= 0 && Y >= CorridorRow0 && Y < CorridorRow0 + CorridorRows)
					{
						AssignCell(CellIndex(X, Y), Plan.CorridorRoom);
					}
				}
			}
			// 계단 빈칸: 복도에 닿지 않은 계단실 칸 (복도가 없으면 첫 줄이 계단참)
			if (Plan.CoreRoom >= 0)
			{
				for (int32 Y = Config.Core.Y; Y < Config.Core.Y + Config.Core.Depth; ++Y)
				{
					for (int32 X = Config.Core.X; X < Config.Core.X + Config.Core.Width; ++X)
					{
						if (!Inside(X, Y))
						{
							continue;
						}
						bool bLanding = false;
						for (int32 Dir = 0; Dir < 4; ++Dir)
						{
							const int32 Neighbor = RoomAt(X + GDirX[Dir], Y + GDirY[Dir]);
							bLanding             = bLanding || (Neighbor >= 0 && Neighbor == Plan.CorridorRoom);
						}
						if (Plan.CorridorRoom < 0)
						{
							bLanding = Y == Config.Core.Y;
						}
						Plan.CoreVoid[static_cast<size_t>(CellIndex(X, Y))] = !bLanding;
					}
				}
			}
		}

		// ---- 2. 호실
		const FBuildingUnitTemplate* FindFixedTemplate(int32 Unit) const
		{
			for (const FBuildingFixedUnit& Fixed : Config.FixedUnits)
			{
				if (Fixed.Floor == Floor && Fixed.Unit == Unit)
				{
					return Config.FindTemplate(Fixed.Template);
				}
			}
			return nullptr;
		}

		void BuildUnits()
		{
			struct FBand
			{
				int32 Row0 = 0, Rows = 0;
				FUnitFrame Frame;
			};
			std::vector<FBand> Bands;
			switch (Config.Corridor)
			{
			case EBuildingCorridor::Center:
				Bands.push_back({ 0, CorridorRow0, { 0, CorridorRow0 - 1, -1 } });
				Bands.push_back({ CorridorRow0 + CorridorRows, D - CorridorRow0 - CorridorRows, { 0, CorridorRow0 + CorridorRows, 1 } });
				break;
			case EBuildingCorridor::Single:
				Bands.push_back({ CorridorRows, D - CorridorRows, { 0, CorridorRows, 1 } });
				break;
			default:
				Bands.push_back({ 0, D, { 0, 0, 1 } });
				break;
			}

			for (const FBand& Band : Bands)
			{
				if (Band.Rows <= 0)
				{
					continue;
				}
				const auto ColumnHasCells = [&](int32 X) {
					for (int32 Y = Band.Row0; Y < Band.Row0 + Band.Rows; ++Y)
					{
						if (Inside(X, Y) && RoomAt(X, Y) < 0)
						{
							return true;
						}
					}
					return false;
				};
				int32 X = 0;
				while (X < W)
				{
					if (!ColumnHasCells(X))
					{
						++X;
						continue;
					}
					int32 End = X;
					while (End < W && ColumnHasCells(End))
					{
						++End;
					}
					// 구간 [X, End)를 호실 너비로 나눈다 (복도가 없으면 구간 전체가 호실 하나)
					int32 Start = X;
					while (Start < End)
					{
						const int32 Remaining = End - Start;
						int32       Width     = Remaining;
						if (Config.Corridor != EBuildingCorridor::None)
						{
							const FBuildingUnitTemplate* Fixed = FindFixedTemplate(static_cast<int32>(Units.size()));
							if (Fixed != nullptr && Fixed->Width > 0 && Fixed->Width <= Remaining)
							{
								Width = Fixed->Width;
							}
							else if (Remaining > Config.UnitMaxWidth)
							{
								const int32 High = std::min(Config.UnitMaxWidth, Remaining - Config.UnitMinWidth);
								Width            = High >= Config.UnitMinWidth ? Rng.Range(Config.UnitMinWidth, High) : (Remaining + 1) / 2;
							}
						}
						FUnitInfo Unit;
						Unit.Frame      = Band.Frame;
						Unit.Frame.MinX = Start;
						Unit.Columns    = Width;
						for (int32 Y = Band.Row0; Y < Band.Row0 + Band.Rows; ++Y)
						{
							for (int32 Cx = Start; Cx < Start + Width; ++Cx)
							{
								if (Inside(Cx, Y) && RoomAt(Cx, Y) < 0)
								{
									Unit.Cells.push_back(CellIndex(Cx, Y));
								}
							}
						}
						if (!Unit.Cells.empty())
						{
							Units.push_back(std::move(Unit));
						}
						Start += Width;
					}
					X = End;
				}
			}
			MergeDetachedUnits();
		}

		// 복도/계단실(없으면 외곽)에 닿지 않는 호실은 이웃 호실에 합친다
		void MergeDetachedUnits()
		{
			const bool bHasAccess = Plan.CorridorRoom >= 0 || Plan.CoreRoom >= 0;
			if (!bHasAccess || Units.size() < 2)
			{
				return;
			}
			std::vector<int32> UnitOfCell(static_cast<size_t>(W * D), -1);
			const auto Rebuild = [&]() {
				std::fill(UnitOfCell.begin(), UnitOfCell.end(), -1);
				for (int32 Unit = 0; Unit < static_cast<int32>(Units.size()); ++Unit)
				{
					for (int32 Cell : Units[static_cast<size_t>(Unit)].Cells)
					{
						UnitOfCell[static_cast<size_t>(Cell)] = Unit;
					}
				}
			};
			Rebuild();
			for (bool bChanged = true; bChanged;)
			{
				bChanged = false;
				for (int32 Unit = 0; Unit < static_cast<int32>(Units.size()) && !bChanged; ++Unit)
				{
					bool  bTouches = false;
					int32 Neighbor = -1;
					for (int32 Cell : Units[static_cast<size_t>(Unit)].Cells)
					{
						const int32 Cx = Cell % W;
						const int32 Cy = Cell / W;
						for (int32 Dir = 0; Dir < 4; ++Dir)
						{
							const int32 Nx = Cx + GDirX[Dir];
							const int32 Ny = Cy + GDirY[Dir];
							if (!Inside(Nx, Ny))
							{
								continue;
							}
							const int32 Room = RoomAt(Nx, Ny);
							bTouches         = bTouches || (Room >= 0 && (Room == Plan.CorridorRoom || Room == Plan.CoreRoom));
							const int32 Other = UnitOfCell[static_cast<size_t>(CellIndex(Nx, Ny))];
							if (Other >= 0 && Other != Unit && Neighbor < 0)
							{
								Neighbor = Other;
							}
						}
					}
					if (!bTouches && Neighbor >= 0)
					{
						std::vector<int32>& Target = Units[static_cast<size_t>(Neighbor)].Cells;
						Target.insert(Target.end(), Units[static_cast<size_t>(Unit)].Cells.begin(), Units[static_cast<size_t>(Unit)].Cells.end());
						std::sort(Target.begin(), Target.end());
						Units.erase(Units.begin() + Unit);
						Rebuild();
						bChanged = true;
					}
				}
			}
		}

		// ---- 3. 방
		void ToUV(const FUnitFrame& Frame, int32 Cell, int32& U, int32& V) const
		{
			U = Cell % W - Frame.MinX;
			V = (Cell / W - Frame.RowNear) * Frame.VSign;
		}

		bool TouchesAccess(const std::vector<int32>& Cells) const
		{
			for (int32 Cell : Cells)
			{
				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					const int32 Room = RoomAt(Cell % W + GDirX[Dir], Cell / W + GDirY[Dir]);
					if (Room >= 0 && (Room == Plan.CorridorRoom || (Plan.CorridorRoom < 0 && Room == Plan.CoreRoom)))
					{
						return true;
					}
				}
			}
			return false;
		}

		bool TouchesExterior(const std::vector<int32>& Cells) const
		{
			for (int32 Cell : Cells)
			{
				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					if (!Inside(Cell % W + GDirX[Dir], Cell / W + GDirY[Dir]))
					{
						return true;
					}
				}
			}
			return false;
		}

		// Label(-1 = 아직 없음)인 칸을 이웃 라벨로 채운다 (인덱스 순서로 반복 — 결정적)
		static void FillOrphans(int32 Width, const std::vector<int32>& Cells, std::map<int32, int32>& Label)
		{
			for (bool bChanged = true; bChanged;)
			{
				bChanged = false;
				for (int32 Cell : Cells)
				{
					if (Label[Cell] >= 0)
					{
						continue;
					}
					for (int32 Dir = 0; Dir < 4; ++Dir)
					{
						const int32 Nx = Cell % Width + GDirX[Dir];
						const int32 Ny = Cell / Width + GDirY[Dir];
						if (Nx < 0 || Ny < 0 || Nx >= Width)
						{
							continue;
						}
						const auto Found = Label.find(Ny * Width + Nx);
						if (Found != Label.end() && Found->second >= 0)
						{
							Label[Cell] = Found->second;
							bChanged    = true;
							break;
						}
					}
				}
			}
		}

		void BuildRooms(int32 UnitIndex)
		{
			const FUnitInfo& Unit = Units[static_cast<size_t>(UnitIndex)];
			int32            MinU = INT32_MAX, MinV = INT32_MAX, MaxU = INT32_MIN, MaxV = INT32_MIN;
			for (int32 Cell : Unit.Cells)
			{
				int32 U, V;
				ToUV(Unit.Frame, Cell, U, V);
				MinU = std::min(MinU, U);
				MinV = std::min(MinV, V);
				MaxU = std::max(MaxU, U);
				MaxV = std::max(MaxV, V);
			}
			const int32 SpanU = MaxU - MinU + 1;
			const int32 SpanV = MaxV - MinV + 1;

			std::map<int32, int32>   Label; // 칸 → 방(지역) 번호
			std::vector<std::string> LeafTypes;
			for (int32 Cell : Unit.Cells)
			{
				Label[Cell] = -1;
			}

			if (const FBuildingUnitTemplate* Template = FindFixedTemplate(UnitIndex); Template != nullptr && Template->Width > 0 && Template->Depth > 0)
			{
				// 고정 호실: 칸 가운데를 템플릿 좌표로 사상 (크기가 같으면 그대로)
				for (const FBuildingTemplateRoom& Room : Template->Rooms)
				{
					LeafTypes.push_back(Room.Type);
				}
				for (int32 Cell : Unit.Cells)
				{
					int32 U, V;
					ToUV(Unit.Frame, Cell, U, V);
					const int32 Tu = static_cast<int32>(std::floor((static_cast<float>(U - MinU) + 0.5f) * static_cast<float>(Template->Width) / static_cast<float>(SpanU)));
					const int32 Tv = static_cast<int32>(std::floor((static_cast<float>(V - MinV) + 0.5f) * static_cast<float>(Template->Depth) / static_cast<float>(SpanV)));
					for (size_t Room = 0; Room < Template->Rooms.size(); ++Room)
					{
						if (Template->Rooms[Room].Rect.Contains(Tu, Tv))
						{
							Label[Cell] = static_cast<int32>(Room);
							break;
						}
					}
				}
				FillOrphans(W, Unit.Cells, Label);
			}
			else
			{
				LeafTypes = SplitRandomRooms(Unit, MinU, MinV, SpanU, SpanV, Label);
			}

			// 라벨 → 방 (빈 라벨은 버림, 라벨 순서 유지)
			std::vector<std::vector<int32>> LeafCells(LeafTypes.size());
			for (const auto& [Cell, Leaf] : Label)
			{
				if (Leaf >= 0 && Leaf < static_cast<int32>(LeafCells.size()))
				{
					LeafCells[static_cast<size_t>(Leaf)].push_back(Cell);
				}
			}
			for (size_t Leaf = 0; Leaf < LeafCells.size(); ++Leaf)
			{
				if (LeafCells[Leaf].empty())
				{
					continue;
				}
				const int32 Room = AddRoom(LeafTypes[Leaf], UnitIndex);
				for (int32 Cell : LeafCells[Leaf]) // std::map 순회라 오름차순
				{
					AssignCell(Cell, Room);
				}
			}
		}

		// 무작위 분할 + 종류 배정. 반환 = 라벨별 종류
		std::vector<std::string> SplitRandomRooms(const FUnitInfo& Unit, int32 MinU, int32 MinV, int32 SpanU, int32 SpanV, std::map<int32, int32>& Label)
		{
			// 종류 고르기 (필수 + 확률 — 필수가 아닌 종류마다 난수 하나를 항상 쓴다)
			std::vector<const FBuildingRoomType*> Types;
			for (const FBuildingRoomType& Type : Config.Rooms)
			{
				if (Type.Name == "Corridor" || Type.Name == "Core")
				{
					continue;
				}
				const bool bPick = Type.bRequired || Rng.Float() < Type.Chance;
				if (bPick)
				{
					Types.push_back(&Type);
				}
			}
			const int32 Area      = static_cast<int32>(Unit.Cells.size());
			const int32 Side      = Config.MinRoomSide;
			const auto  MinAreaSum = [&]() {
				int32 Sum = 0;
				for (const FBuildingRoomType* Type : Types)
				{
					Sum += std::max(Type->MinArea, Side * Side);
				}
				return Sum;
			};
			const auto DropOne = [&]() {
				// 필수가 아닌 것부터 뒤에서, 다음은 현관이 아닌 필수를 뒤에서
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					for (size_t Index = Types.size(); Index-- > 0;)
					{
						const FBuildingRoomType* Type = Types[Index];
						if ((Pass == 0 && !Type->bRequired) || (Pass == 1 && !Type->bEntry))
						{
							Types.erase(Types.begin() + static_cast<std::ptrdiff_t>(Index));
							return true;
						}
					}
				}
				return false;
			};
			while (Types.size() > 1 && MinAreaSum() > Area && DropOne())
			{
			}

			// 분할 + 배정을 여러 번 해 보고 점수가 가장 높은 것을 쓴다 (면적 비율·인접 규칙에 더 맞는 배치 — 시도 수가 고정이라 결정적)
			const std::vector<const FBuildingRoomType*> PickedTypes  = Types;
			const std::map<int32, int32>                InitialLabel = Label;
			std::map<int32, int32>                      BestLabel;
			std::vector<std::string>                    BestNames;
			float                                       BestScore = 0.0f;
			for (int32 Try = 0; Try < GRoomSplitTries; ++Try)
			{
				Types = PickedTypes;
				Label = InitialLabel;
				// BSP: 가장 큰 잎을 긴 축으로 (변 ≥ Side)
				std::vector<FCellRect> Leaves{ { MinU, MinV, SpanU, SpanV } };
				const size_t           Target = std::max<size_t>(1, Types.size());
				while (Leaves.size() < Target)
				{
					int32 Best = -1;
					for (int32 Leaf = 0; Leaf < static_cast<int32>(Leaves.size()); ++Leaf)
					{
						const FCellRect& Rect = Leaves[static_cast<size_t>(Leaf)];
						if ((Rect.W >= 2 * Side || Rect.D >= 2 * Side) &&
						    (Best < 0 || Rect.W * Rect.D > Leaves[static_cast<size_t>(Best)].W * Leaves[static_cast<size_t>(Best)].D))
						{
							Best = Leaf;
						}
					}
					if (Best < 0)
					{
						break;
					}
					const FCellRect Rect   = Leaves[static_cast<size_t>(Best)];
					const bool      bCanU  = Rect.W >= 2 * Side;
					const bool      bCanV  = Rect.D >= 2 * Side;
					bool            bAlongU = bCanU && (!bCanV || Rect.W > Rect.D || (Rect.W == Rect.D && (Rng.Next() & 1) != 0));
					if (bAlongU)
					{
						const int32 Cut                       = Rng.Range(Side, Rect.W - Side);
						Leaves[static_cast<size_t>(Best)]     = { Rect.X, Rect.Y, Cut, Rect.D };
						Leaves.push_back({ Rect.X + Cut, Rect.Y, Rect.W - Cut, Rect.D });
					}
					else
					{
						const int32 Cut                   = Rng.Range(Side, Rect.D - Side);
						Leaves[static_cast<size_t>(Best)] = { Rect.X, Rect.Y, Rect.W, Cut };
						Leaves.push_back({ Rect.X, Rect.Y + Cut, Rect.W, Rect.D - Cut });
					}
				}

				// 잎 → 칸 (외곽이 사각형이 아니면 빈 잎/끊긴 조각이 생길 수 있다 — 조각은 이웃에 붙인다)
				std::vector<int32> LeafArea(Leaves.size(), 0);
				for (int32 Cell : Unit.Cells)
				{
					int32 U, V;
					ToUV(Unit.Frame, Cell, U, V);
					for (size_t Leaf = 0; Leaf < Leaves.size(); ++Leaf)
					{
						const FCellRect& Rect = Leaves[Leaf];
						if (U >= Rect.X && U < Rect.X + Rect.W && V >= Rect.Y && V < Rect.Y + Rect.D)
						{
							Label[Cell] = static_cast<int32>(Leaf);
							break;
						}
					}
				}
				KeepLargestComponents(Unit.Cells, Label, static_cast<int32>(Leaves.size()));
				FillOrphans(W, Unit.Cells, Label);

				// 빈 잎 제거 (라벨 다시 매김)
				std::vector<int32> Remap(Leaves.size(), -1);
				int32              LeafCount = 0;
				for (const auto& [Cell, Leaf] : Label)
				{
					if (Leaf >= 0)
					{
						++LeafArea[static_cast<size_t>(Leaf)];
					}
				}
				for (size_t Leaf = 0; Leaf < Leaves.size(); ++Leaf)
				{
					if (LeafArea[Leaf] > 0)
					{
						Remap[Leaf] = LeafCount++;
					}
				}
				for (auto& [Cell, Leaf] : Label)
				{
					Leaf = Leaf >= 0 ? Remap[static_cast<size_t>(Leaf)] : -1;
				}
				while (static_cast<int32>(Types.size()) > LeafCount && DropOne())
				{
				}
				while (static_cast<int32>(Types.size()) > LeafCount)
				{
					Types.pop_back();
				}
				float                    Score = 0.0f;
				std::vector<std::string> Names = Types.empty() ? std::vector<std::string>(static_cast<size_t>(LeafCount), "Room") // 방 종류가 없으면 이름 없는 방
				                                               : AssignTypes(Label, LeafCount, Types, Score);
				if (Try == 0 || Score > BestScore)
				{
					BestScore = Score;
					BestNames = std::move(Names);
					BestLabel = Label;
				}
			}
			Label = std::move(BestLabel);
			return BestNames;
		}

		// 잎마다 가장 큰 연결 조각만 남기고 나머지 칸은 -1
		void KeepLargestComponents(const std::vector<int32>& Cells, std::map<int32, int32>& Label, int32 LeafCount) const
		{
			for (int32 Leaf = 0; Leaf < LeafCount; ++Leaf)
			{
				std::map<int32, int32> Component; // 칸 → 조각
				int32                  ComponentCount = 0;
				std::vector<int32>     Sizes;
				for (int32 Start : Cells)
				{
					if (Label[Start] != Leaf || Component.contains(Start))
					{
						continue;
					}
					std::vector<int32> Stack{ Start };
					Component[Start] = ComponentCount;
					int32 Size       = 0;
					while (!Stack.empty())
					{
						const int32 Cell = Stack.back();
						Stack.pop_back();
						++Size;
						for (int32 Dir = 0; Dir < 4; ++Dir)
						{
							const int32 Nx = Cell % W + GDirX[Dir];
							const int32 Ny = Cell / W + GDirY[Dir];
							if (Nx < 0 || Ny < 0 || Nx >= W || Ny >= D)
							{
								continue;
							}
							const int32 Next  = Ny * W + Nx;
							const auto  Found = Label.find(Next);
							if (Found != Label.end() && Found->second == Leaf && !Component.contains(Next))
							{
								Component[Next] = ComponentCount;
								Stack.push_back(Next);
							}
						}
					}
					Sizes.push_back(Size);
					++ComponentCount;
				}
				if (ComponentCount <= 1)
				{
					continue;
				}
				const int32 Keep = static_cast<int32>(std::max_element(Sizes.begin(), Sizes.end()) - Sizes.begin());
				for (const auto& [Cell, Piece] : Component)
				{
					if (Piece != Keep)
					{
						Label[Cell] = -1;
					}
				}
			}
		}

		std::vector<std::string> AssignTypes(const std::map<int32, int32>& Label, int32 LeafCount, const std::vector<const FBuildingRoomType*>& Types, float& OutScore)
		{
			const size_t                    N = static_cast<size_t>(LeafCount);
			std::vector<std::vector<int32>> Cells(N);
			for (const auto& [Cell, Leaf] : Label)
			{
				if (Leaf >= 0)
				{
					Cells[static_cast<size_t>(Leaf)].push_back(Cell);
				}
			}
			std::vector<bool>              bAccess(N), bExterior(N);
			std::vector<std::vector<bool>> bAdjacent(N, std::vector<bool>(N, false));
			for (size_t Leaf = 0; Leaf < N; ++Leaf)
			{
				bAccess[Leaf]   = TouchesAccess(Cells[Leaf]);
				bExterior[Leaf] = TouchesExterior(Cells[Leaf]);
				for (int32 Cell : Cells[Leaf])
				{
					for (int32 Dir = 0; Dir < 4; ++Dir)
					{
						const int32 Nx = Cell % W + GDirX[Dir];
						const int32 Ny = Cell / W + GDirY[Dir];
						if (Nx < 0 || Ny < 0 || Nx >= W || Ny >= D)
						{
							continue;
						}
						const auto Found = Label.find(Ny * W + Nx);
						if (Found != Label.end() && Found->second >= 0 && static_cast<size_t>(Found->second) != Leaf)
						{
							bAdjacent[Leaf][static_cast<size_t>(Found->second)] = true;
						}
					}
				}
			}
			float TotalWeight = 0.0f;
			int32 TotalArea   = 0;
			for (const FBuildingRoomType* Type : Types)
			{
				TotalWeight += std::max(0.01f, Type->Weight);
			}
			for (const std::vector<int32>& LeafCells : Cells)
			{
				TotalArea += static_cast<int32>(LeafCells.size());
			}
			// 동점 깨기 잡음 (시드에 따라 다른 배정)
			std::vector<float> Noise(N * Types.size());
			for (float& Value : Noise)
			{
				Value = Rng.Float() * 0.05f;
			}

			// 인접 선호: (종류, 원하는 이름) → 그 이름의 종류 번호들 (점수 계산에서 문자열 비교를 피한다)
			std::vector<std::vector<std::vector<int32>>> WantedTypes(Types.size());
			for (size_t Type = 0; Type < Types.size(); ++Type)
			{
				for (const std::string& Wanted : Types[Type]->Adjacent)
				{
					std::vector<int32>& Matches = WantedTypes[Type].emplace_back();
					for (size_t Other = 0; Other < Types.size(); ++Other)
					{
						if (Types[Other]->Name == Wanted)
						{
							Matches.push_back(static_cast<int32>(Other));
						}
					}
				}
			}
			std::vector<int32> LeafOfType(Types.size(), -1);

			const auto Score = [&](const std::vector<int32>& Assign) {
				float Sum = 0.0f;
				for (size_t Leaf = 0; Leaf < N; ++Leaf)
				{
					LeafOfType[static_cast<size_t>(Assign[Leaf])] = static_cast<int32>(Leaf);
				}
				for (size_t Leaf = 0; Leaf < N; ++Leaf)
				{
					const FBuildingRoomType& Type    = *Types[static_cast<size_t>(Assign[Leaf])];
					const float              AreaFrac = static_cast<float>(Cells[Leaf].size()) / static_cast<float>(std::max(1, TotalArea));
					Sum -= 6.0f * std::abs(AreaFrac - std::max(0.01f, Type.Weight) / TotalWeight);
					if (static_cast<int32>(Cells[Leaf].size()) < Type.MinArea)
					{
						Sum -= 3.0f;
					}
					if (Type.bEntry)
					{
						Sum += bAccess[Leaf] ? 6.0f : -6.0f;
					}
					if (Type.bHub && bAccess[Leaf])
					{
						Sum += 1.0f;
					}
					if (Type.bNeedsWindow)
					{
						Sum += bExterior[Leaf] ? 2.0f : -2.0f;
					}
					for (const std::vector<int32>& Matches : WantedTypes[static_cast<size_t>(Assign[Leaf])])
					{
						for (const int32 Match : Matches)
						{
							const int32 Other = LeafOfType[static_cast<size_t>(Match)];
							if (Other >= 0 && bAdjacent[Leaf][static_cast<size_t>(Other)])
							{
								Sum += 1.5f;
								break;
							}
						}
					}
					Sum += Noise[Leaf * Types.size() + static_cast<size_t>(Assign[Leaf])];
				}
				return Sum;
			};

			std::vector<int32> Assign(N);
			std::iota(Assign.begin(), Assign.end(), 0);
			std::vector<int32> Best      = Assign;
			float              BestScore = Score(Assign);
			if (N <= 6)
			{
				// 모든 순열 (6! = 720)
				while (std::next_permutation(Assign.begin(), Assign.end()))
				{
					const float Value = Score(Assign);
					if (Value > BestScore)
					{
						BestScore = Value;
						Best      = Assign;
					}
				}
			}
			else
			{
				// 방이 많으면 두 방 맞바꾸기 국소 탐색 (더 나아지는 교환이 없을 때까지)
				for (bool bImproved = true; bImproved;)
				{
					bImproved = false;
					for (size_t A = 0; A < N; ++A)
					{
						for (size_t B = A + 1; B < N; ++B)
						{
							std::swap(Best[A], Best[B]);
							const float Value = Score(Best);
							if (Value > BestScore + 1.0e-4f)
							{
								BestScore = Value;
								bImproved = true;
							}
							else
							{
								std::swap(Best[A], Best[B]);
							}
						}
					}
				}
			}
			std::vector<std::string> Names(N);
			for (size_t Leaf = 0; Leaf < N; ++Leaf)
			{
				Names[Leaf] = Types[static_cast<size_t>(Best[Leaf])]->Name;
			}
			OutScore = BestScore;
			return Names;
		}

		// ---- 4. 경계 (벽/문/창/트임)
		struct FEdgeRef
		{
			bool  bX = true; // EdgesX
			int32 X  = 0;
			int32 Y  = 0;
		};

		EBuildingEdge& EdgeAt(const FEdgeRef& Ref)
		{
			return Ref.bX ? Plan.EdgesX[static_cast<size_t>(Ref.Y * (W + 1) + Ref.X)] : Plan.EdgesY[static_cast<size_t>(Ref.Y * W + Ref.X)];
		}

		// 공유 경계 중 문 자리: 양 끝이 아닌 칸 우선
		FEdgeRef ChooseDoorEdge(const std::vector<FEdgeRef>& Edges)
		{
			std::vector<size_t> Middle;
			for (size_t Index = 0; Index < Edges.size(); ++Index)
			{
				const FEdgeRef& Edge   = Edges[Index];
				int32           Around = 0;
				for (const FEdgeRef& Other : Edges)
				{
					const bool bSameLine = Other.bX == Edge.bX && (Edge.bX ? Other.X == Edge.X : Other.Y == Edge.Y);
					const int32 Delta    = Edge.bX ? Other.Y - Edge.Y : Other.X - Edge.X;
					if (bSameLine && (Delta == 1 || Delta == -1))
					{
						++Around;
					}
				}
				if (Around == 2)
				{
					Middle.push_back(Index);
				}
			}
			if (!Middle.empty())
			{
				return Edges[Middle[static_cast<size_t>(Rng.Range(0, static_cast<int32>(Middle.size()) - 1))]];
			}
			return Edges[static_cast<size_t>(Rng.Range(0, static_cast<int32>(Edges.size()) - 1))];
		}

		bool IsOpenPair(int32 A, int32 B) const
		{
			const FBuildingRoomType* TypeA = TypeOf(A);
			const FBuildingRoomType* TypeB = TypeOf(B);
			const std::string&       NameA = Plan.Rooms[static_cast<size_t>(A)].Type;
			const std::string&       NameB = Plan.Rooms[static_cast<size_t>(B)].Type;
			return (TypeA != nullptr && std::find(TypeA->OpenTo.begin(), TypeA->OpenTo.end(), NameB) != TypeA->OpenTo.end()) ||
			       (TypeB != nullptr && std::find(TypeB->OpenTo.begin(), TypeB->OpenTo.end(), NameA) != TypeB->OpenTo.end());
		}

		void BuildEdges()
		{
			Plan.EdgesX.assign(static_cast<size_t>((W + 1) * D), EBuildingEdge::None);
			Plan.EdgesY.assign(static_cast<size_t>(W * (D + 1)), EBuildingEdge::None);
			std::map<std::pair<int32, int32>, std::vector<FEdgeRef>> Shared; // (작은 방, 큰 방) → 경계
			std::vector<FEdgeRef>                                    Exterior;
			std::vector<int32>                                       ExteriorRoom;
			std::vector<int32>                                       ExteriorNormal; // 바깥 방향 (GDir 번호)
			const auto Classify = [&](const FEdgeRef& Ref, int32 A, int32 B, int32 OutwardFromA, int32 OutwardFromB) {
				if (A < 0 && B < 0)
				{
					return;
				}
				if (A < 0 || B < 0)
				{
					EdgeAt(Ref) = EBuildingEdge::Wall;
					Exterior.push_back(Ref);
					ExteriorRoom.push_back(A >= 0 ? A : B);
					ExteriorNormal.push_back(A >= 0 ? OutwardFromA : OutwardFromB);
					return;
				}
				if (A != B)
				{
					EdgeAt(Ref) = EBuildingEdge::Wall;
					Shared[{ std::min(A, B), std::max(A, B) }].push_back(Ref);
				}
			};
			for (int32 Y = 0; Y < D; ++Y)
			{
				for (int32 X = 0; X <= W; ++X)
				{
					Classify({ true, X, Y }, RoomAt(X - 1, Y), RoomAt(X, Y), 1, 0);
				}
			}
			for (int32 Y = 0; Y <= D; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					Classify({ false, X, Y }, RoomAt(X, Y - 1), RoomAt(X, Y), 3, 2);
				}
			}

			// 계단실↔복도, OpenTo 종류끼리 = 트임
			for (const auto& [Pair, Edges] : Shared)
			{
				const bool bCoreCorridor = (Pair.first == Plan.CoreRoom && Pair.second == Plan.CorridorRoom) ||
				                           (Pair.second == Plan.CoreRoom && Pair.first == Plan.CorridorRoom);
				const bool bSameUnit = Plan.Rooms[static_cast<size_t>(Pair.first)].Unit >= 0 &&
				                       Plan.Rooms[static_cast<size_t>(Pair.first)].Unit == Plan.Rooms[static_cast<size_t>(Pair.second)].Unit;
				if (bCoreCorridor || (bSameUnit && IsOpenPair(Pair.first, Pair.second)))
				{
					for (const FEdgeRef& Edge : Edges)
					{
						EdgeAt(Edge) = EBuildingEdge::Open;
					}
				}
			}

			// 호실 입구 + 호실 안 최소 신장 트리
			const int32 Access = Plan.CorridorRoom >= 0 ? Plan.CorridorRoom : Plan.CoreRoom;
			for (int32 Unit = 0; Unit < static_cast<int32>(Units.size()); ++Unit)
			{
				std::vector<int32> UnitRooms;
				for (int32 Room = 0; Room < static_cast<int32>(Plan.Rooms.size()); ++Room)
				{
					if (Plan.Rooms[static_cast<size_t>(Room)].Unit == Unit)
					{
						UnitRooms.push_back(Room);
					}
				}
				if (UnitRooms.empty())
				{
					continue;
				}
				int32 Entry      = -1;
				int32 EntryScore = -1;
				for (int32 Room : UnitRooms)
				{
					const auto Found = Access >= 0 ? Shared.find({ std::min(Room, Access), std::max(Room, Access) }) : Shared.end();
					if (Found == Shared.end())
					{
						continue;
					}
					const FBuildingRoomType* Type  = TypeOf(Room);
					const int32              Score = Type != nullptr ? (Type->bEntry ? 3 : (Type->bHub ? 2 : 1)) : 1;
					if (Score > EntryScore)
					{
						Entry      = Room;
						EntryScore = Score;
					}
				}
				if (Entry >= 0)
				{
					EdgeAt(ChooseDoorEdge(Shared[{ std::min(Entry, Access), std::max(Entry, Access) }])) = EBuildingEdge::Door;
				}
				else if (Access < 0 && Floor == 0)
				{
					// 복도도 계단실도 없는 층: 1층은 외벽 출입문으로 들어간다 (첫 방)
					Entry = UnitRooms.front();
				}
				else
				{
					Warn(std::format("호실 {}이 복도/계단실에 닿지 않습니다", Unit + 1));
					Entry = UnitRooms.front();
				}

				// Prim: 허브와 잇는 간선이 싸다 (같은 비용이면 미리 뽑은 잡음 순)
				std::map<std::pair<int32, int32>, float> Cost;
				for (const auto& [Pair, Edges] : Shared)
				{
					if (Plan.Rooms[static_cast<size_t>(Pair.first)].Unit != Unit || Plan.Rooms[static_cast<size_t>(Pair.second)].Unit != Unit)
					{
						continue;
					}
					const FBuildingRoomType* A    = TypeOf(Pair.first);
					const FBuildingRoomType* B    = TypeOf(Pair.second);
					const bool               bHub = (A != nullptr && (A->bHub || A->bEntry)) || (B != nullptr && (B->bHub || B->bEntry));
					const bool               bOpen = EdgeAt(Edges.front()) == EBuildingEdge::Open;
					Cost[Pair]                     = (bOpen ? 0.0f : (bHub ? 1.0f : 3.0f)) + Rng.Float() * 0.5f;
				}
				std::vector<bool> InTree(Plan.Rooms.size(), false);
				InTree[static_cast<size_t>(Entry)] = true;
				for (;;)
				{
					std::pair<int32, int32> BestPair{ -1, -1 };
					float                   BestCost = 1.0e9f;
					for (const auto& [Pair, Value] : Cost)
					{
						if (InTree[static_cast<size_t>(Pair.first)] != InTree[static_cast<size_t>(Pair.second)] && Value < BestCost)
						{
							BestCost = Value;
							BestPair = Pair;
						}
					}
					if (BestPair.first < 0)
					{
						break;
					}
					InTree[static_cast<size_t>(BestPair.first)]  = true;
					InTree[static_cast<size_t>(BestPair.second)] = true;
					std::vector<FEdgeRef>& Edges                 = Shared[BestPair];
					if (EdgeAt(Edges.front()) != EBuildingEdge::Open)
					{
						EdgeAt(ChooseDoorEdge(Edges)) = EBuildingEdge::Door;
					}
				}
				for (int32 Room : UnitRooms)
				{
					if (!InTree[static_cast<size_t>(Room)])
					{
						Warn(std::format("호실 {}의 방 '{}'이 다른 방과 닿지 않습니다", Unit + 1, Plan.Rooms[static_cast<size_t>(Room)].Type));
					}
				}
			}

			// 건물 출입문 (1층): 복도 끝 ±X 외벽 가운데, 없으면 -Y 외벽. 복도가 없으면 계단실, 그것도 없으면 첫 방
			if (Floor == 0 && Config.bEntrance)
			{
				const int32 EntranceRoom = Plan.CorridorRoom >= 0 ? Plan.CorridorRoom : (Plan.CoreRoom >= 0 ? Plan.CoreRoom : (Plan.Rooms.empty() ? -1 : 0));
				for (int32 Normal : { 0, 1 })
				{
					std::vector<FEdgeRef> Side;
					for (size_t Index = 0; Index < Exterior.size(); ++Index)
					{
						if (ExteriorRoom[Index] == EntranceRoom && ExteriorNormal[Index] == Normal)
						{
							Side.push_back(Exterior[Index]);
						}
					}
					if (!Side.empty())
					{
						EdgeAt(Side[Side.size() / 2]) = EBuildingEdge::EntranceDoor;
					}
				}
				bool bAny = false;
				for (size_t Index = 0; Index < Exterior.size(); ++Index)
				{
					bAny = bAny || EdgeAt(Exterior[Index]) == EBuildingEdge::EntranceDoor;
				}
				if (!bAny)
				{
					std::vector<FEdgeRef> Front;
					for (size_t Index = 0; Index < Exterior.size(); ++Index)
					{
						if (ExteriorRoom[Index] == EntranceRoom)
						{
							Front.push_back(Exterior[Index]);
						}
					}
					if (!Front.empty())
					{
						EdgeAt(Front[Front.size() / 2]) = EBuildingEdge::EntranceDoor;
					}
				}
			}

			// 창 (외벽, 방 종류 확률 — 외벽 경계마다 난수 하나)
			for (size_t Index = 0; Index < Exterior.size(); ++Index)
			{
				const float              Roll = Rng.Float();
				const FBuildingRoomType* Type = TypeOf(ExteriorRoom[Index]);
				if (EdgeAt(Exterior[Index]) == EBuildingEdge::Wall && Type != nullptr && Roll < Type->WindowChance)
				{
					EdgeAt(Exterior[Index]) = EBuildingEdge::Window;
				}
			}
			// 단면: 앞면(-Y 바깥) 외벽을 뺀다
			if (bSection)
			{
				for (size_t Index = 0; Index < Exterior.size(); ++Index)
				{
					if (ExteriorNormal[Index] == 2)
					{
						EdgeAt(Exterior[Index]) = EBuildingEdge::None;
					}
				}
			}
		}

		// ---- 6. 모듈 배치
		float    CellSize() const { return Config.CellSize; }
		float    BaseZ() const { return static_cast<float>(Floor) * Config.FloorHeight; }
		FVector2 CellMin(int32 X, int32 Y) const
		{
			return FVector2((static_cast<float>(X) - static_cast<float>(W) * 0.5f) * CellSize(), (static_cast<float>(Y) - static_cast<float>(D) * 0.5f) * CellSize());
		}

		void Emit(EBuildingPiece Piece, const std::string& Asset, const FVector3& Position, float Yaw, const FVector3& Scale, int32 Room, std::string Tag)
		{
			if (Asset.empty())
			{
				return;
			}
			FBuildingPlacement Placement;
			Placement.Piece    = Piece;
			Placement.Asset    = Asset;
			Placement.Position = Position;
			Placement.Yaw      = Yaw;
			Placement.Scale    = Scale;
			Placement.Floor    = Floor;
			Placement.Room     = Room;
			Placement.Unit     = Room >= 0 ? Plan.Rooms[static_cast<size_t>(Room)].Unit : -1;
			Placement.Tag      = std::move(Tag);
			Result.Placements.push_back(std::move(Placement));
		}

		void EmitRects(EBuildingPiece Piece, int32 Room, const std::vector<bool>& Mask, float Z)
		{
			const FBuildingRoomType* Type  = TypeOf(Room);
			const bool               bCustomFloor = Piece == EBuildingPiece::Floor && Type != nullptr && !Type->FloorPiece.empty();
			const std::string&       Asset = bCustomFloor ? Type->FloorPiece : Config.GetKitPiece(Piece);
			for (const FCellRect& Rect : DecomposeRects(W, D, Mask))
			{
				const FVector2 Min = CellMin(Rect.X, Rect.Y);
				const FVector3 Center(Min.X + static_cast<float>(Rect.W) * CellSize() * 0.5f, Min.Y + static_cast<float>(Rect.D) * CellSize() * 0.5f, Z);
				Emit(Piece, Asset, Center, 0.0f, FVector3(static_cast<float>(Rect.W), static_cast<float>(Rect.D), 1.0f), Room,
				     Plan.Rooms[static_cast<size_t>(Room)].Type);
			}
		}

		void PlaceModules()
		{
			const bool bTop      = Floor == FloorCount - 1;
			const bool bCeiling  = !bSection;
			for (int32 Room = 0; Room < static_cast<int32>(Plan.Rooms.size()); ++Room)
			{
				std::vector<bool> FloorMask(static_cast<size_t>(W * D), false);
				std::vector<bool> CeilingMask(static_cast<size_t>(W * D), false);
				for (int32 Cell = 0; Cell < W * D; ++Cell)
				{
					if (Plan.CellRoom[static_cast<size_t>(Cell)] != Room)
					{
						continue;
					}
					const bool bVoid           = Plan.CoreVoid[static_cast<size_t>(Cell)];
					FloorMask[static_cast<size_t>(Cell)]   = !(bVoid && Floor > 0);
					CeilingMask[static_cast<size_t>(Cell)] = !(bVoid && !bTop);
				}
				const bool bCommon = Room == Plan.CorridorRoom || Room == Plan.CoreRoom;
				EmitRects(bCommon ? EBuildingPiece::CorridorFloor : EBuildingPiece::Floor, Room, FloorMask, BaseZ());
				if (bCeiling)
				{
					EmitRects(bTop ? EBuildingPiece::Roof : EBuildingPiece::Ceiling, Room, CeilingMask, BaseZ() + Config.FloorHeight);
				}
			}

			// 경계 조각
			for (int32 Y = 0; Y < D; ++Y)
			{
				for (int32 X = 0; X <= W; ++X)
				{
					EmitEdge(Plan.GetEdgeX(X, Y), RoomAt(X - 1, Y), RoomAt(X, Y), FVector2(CellMin(X, Y).X, CellMin(X, Y).Y + CellSize() * 0.5f), 0, 1);
				}
			}
			for (int32 Y = 0; Y <= D; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					EmitEdge(Plan.GetEdgeY(X, Y), RoomAt(X, Y - 1), RoomAt(X, Y), FVector2(CellMin(X, Y).X + CellSize() * 0.5f, CellMin(X, Y).Y), 2, 3);
				}
			}

			// 모서리 기둥: 꼭짓점에서 세로·가로 벽이 모두 있으면
			const std::string& CornerAsset = Config.GetKitPiece(EBuildingPiece::Corner);
			if (!CornerAsset.empty())
			{
				for (int32 Vy = 0; Vy <= D; ++Vy)
				{
					for (int32 Vx = 0; Vx <= W; ++Vx)
					{
						const bool bVertical = (Vy > 0 && HasWallPiece(Plan.GetEdgeX(Vx, Vy - 1))) || (Vy < D && HasWallPiece(Plan.GetEdgeX(Vx, Vy)));
						const bool bHorizontal = (Vx > 0 && HasWallPiece(Plan.GetEdgeY(Vx - 1, Vy))) || (Vx < W && HasWallPiece(Plan.GetEdgeY(Vx, Vy)));
						if (bVertical && bHorizontal)
						{
							const FVector2 Point = CellMin(Vx, Vy);
							Emit(EBuildingPiece::Corner, CornerAsset, FVector3(Point.X, Point.Y, BaseZ()), 0.0f, FVector3(1.0f, 1.0f, 1.0f), -1, "Corner");
						}
					}
				}
			}
			PlaceStairs();
		}

		// 경계 하나: 앞 = 방 안쪽 (외벽이면 안쪽 칸, 안쪽 경계면 B 쪽 — 단 B가 복도이고 A가 호실 방이면 A 쪽)
		void EmitEdge(EBuildingEdge Edge, int32 A, int32 B, const FVector2& Point, int32 DirToA, int32 DirToB)
		{
			if (!HasWallPiece(Edge))
			{
				return;
			}
			const bool bExterior = A < 0 || B < 0;
			bool       bFrontB   = B >= 0;
			if (!bExterior && Plan.Rooms[static_cast<size_t>(B)].Unit < 0 && Plan.Rooms[static_cast<size_t>(A)].Unit >= 0)
			{
				bFrontB = false;
			}
			const int32    FrontRoom = bFrontB ? B : A;
			EBuildingPiece Piece     = EBuildingPiece::Wall;
			switch (Edge)
			{
			case EBuildingEdge::Door:         Piece = EBuildingPiece::Door; break;
			case EBuildingEdge::Window:       Piece = EBuildingPiece::Window; break;
			case EBuildingEdge::EntranceDoor: Piece = EBuildingPiece::EntranceDoor; break;
			default:                          Piece = bExterior ? EBuildingPiece::ExteriorWall : EBuildingPiece::Wall; break;
			}
			Emit(Piece, Config.GetKitPiece(Piece), FVector3(Point.X, Point.Y, BaseZ()), YawFromDirection(bFrontB ? DirToB : DirToA), FVector3(1.0f, 1.0f, 1.0f),
			     FrontRoom, Plan.Rooms[static_cast<size_t>(FrontRoom)].Type);
		}

		void PlaceStairs()
		{
			if (Plan.CoreRoom < 0)
			{
				return;
			}
			int32 MinX = INT32_MAX, MinY = INT32_MAX, MaxX = INT32_MIN, MaxY = INT32_MIN;
			int32 LandX = 0, LandY = 0, LandCount = 0;
			for (int32 Cell = 0; Cell < W * D; ++Cell)
			{
				if (Plan.CellRoom[static_cast<size_t>(Cell)] != Plan.CoreRoom)
				{
					continue;
				}
				if (Plan.CoreVoid[static_cast<size_t>(Cell)])
				{
					MinX = std::min(MinX, Cell % W);
					MinY = std::min(MinY, Cell / W);
					MaxX = std::max(MaxX, Cell % W);
					MaxY = std::max(MaxY, Cell / W);
				}
				else
				{
					LandX += Cell % W;
					LandY += Cell / W;
					++LandCount;
				}
			}
			if (MinX > MaxX || LandCount == 0)
			{
				return;
			}
			// 계단은 빈칸 사각형 안에서 계단참 쪽으로 오른다. 오름 축에 수직한 폭의 절반(최소 1칸)만 쓰고 나머지는 위층에서 난간
			const float VoidCx  = (static_cast<float>(MinX + MaxX) + 1.0f) * 0.5f;
			const float VoidCy  = (static_cast<float>(MinY + MaxY) + 1.0f) * 0.5f;
			const float LandCx  = static_cast<float>(LandX) / static_cast<float>(LandCount) + 0.5f;
			const float LandCy  = static_cast<float>(LandY) / static_cast<float>(LandCount) + 0.5f;
			const bool  bAlongX = std::abs(LandCx - VoidCx) >= std::abs(LandCy - VoidCy);
			const int32 RiseDir = bAlongX ? (LandCx > VoidCx ? 1 : 0) : (LandCy > VoidCy ? 3 : 2);
			const int32 Length  = bAlongX ? MaxX - MinX + 1 : MaxY - MinY + 1;
			const int32 Across  = bAlongX ? MaxY - MinY + 1 : MaxX - MinX + 1;
			const int32 StairW  = std::max(1, Across / 2);
			// 계단 칸 = 가로 방향 앞쪽 StairW칸
			std::vector<bool> bStairCell(static_cast<size_t>(W * D), false);
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 AcrossIndex = bAlongX ? Y - MinY : X - MinX;
					bStairCell[static_cast<size_t>(CellIndex(X, Y))] = AcrossIndex < StairW;
				}
			}
			if (Floor < FloorCount - 1)
			{
				const FVector2 Min = CellMin(MinX, MinY);
				const float    Cx  = bAlongX ? Min.X + static_cast<float>(Length) * CellSize() * 0.5f : Min.X + static_cast<float>(StairW) * CellSize() * 0.5f;
				const float    Cy  = bAlongX ? Min.Y + static_cast<float>(StairW) * CellSize() * 0.5f : Min.Y + static_cast<float>(Length) * CellSize() * 0.5f;
				Emit(EBuildingPiece::Stairs, Config.GetKitPiece(EBuildingPiece::Stairs), FVector3(Cx, Cy, BaseZ()), YawFromDirection(RiseDir),
				     FVector3(static_cast<float>(Length), static_cast<float>(StairW), 1.0f), Plan.CoreRoom, "Stairs");
			}
			// 위층: 계단참 ↔ 빈칸(계단이 닿지 않는 칸) 경계에 난간
			if (Floor > 0)
			{
				for (int32 Y = MinY; Y <= MaxY + 1; ++Y)
				{
					for (int32 X = MinX; X <= MaxX + 1; ++X)
					{
						for (int32 Dir : { 0, 2 })
						{
							const int32 Ax = X + GDirX[Dir], Ay = Y + GDirY[Dir]; // A = -X/-Y 쪽 칸
							if (RoomAt(X, Y) != Plan.CoreRoom || RoomAt(Ax, Ay) != Plan.CoreRoom)
							{
								continue;
							}
							const bool bVoidB = Plan.CoreVoid[static_cast<size_t>(CellIndex(X, Y))];
							const bool bVoidA = Plan.CoreVoid[static_cast<size_t>(CellIndex(Ax, Ay))];
							if (bVoidA == bVoidB)
							{
								continue;
							}
							const int32 VoidCell = bVoidA ? CellIndex(Ax, Ay) : CellIndex(X, Y);
							if (bStairCell[static_cast<size_t>(VoidCell)])
							{
								continue; // 계단이 올라오는 자리
							}
							const FVector2 Min   = CellMin(X, Y);
							const FVector2 Point = Dir == 0 ? FVector2(Min.X, Min.Y + CellSize() * 0.5f) : FVector2(Min.X + CellSize() * 0.5f, Min.Y);
							const int32    Front = bVoidA ? (Dir == 0 ? 1 : 3) : Dir; // 계단참 쪽을 본다
							Emit(EBuildingPiece::Railing, Config.GetKitPiece(EBuildingPiece::Railing), FVector3(Point.X, Point.Y, BaseZ()), YawFromDirection(Front),
							     FVector3(1.0f, 1.0f, 1.0f), Plan.CoreRoom, "Railing");
						}
					}
				}
			}
		}

		// ---- 7. 소품
		struct FRoomProps
		{
			int32               Room = -1;
			std::vector<FRect2> Blocking;  // 막는 소품
			std::vector<FRect2> KeepFree;  // 문 앞 칸 + 소품 앞 비움
			std::vector<FVector2> DoorSamples; // 문 바로 안쪽 점
		};

		bool IsRectInRoom(const FRect2& Rect, int32 Room) const
		{
			const float Epsilon = 0.5f;
			const FVector2 Origin = CellMin(0, 0);
			const int32 X0 = static_cast<int32>(std::floor((Rect.MinX + Epsilon - Origin.X) / CellSize()));
			const int32 X1 = static_cast<int32>(std::floor((Rect.MaxX - Epsilon - Origin.X) / CellSize()));
			const int32 Y0 = static_cast<int32>(std::floor((Rect.MinY + Epsilon - Origin.Y) / CellSize()));
			const int32 Y1 = static_cast<int32>(std::floor((Rect.MaxY - Epsilon - Origin.Y) / CellSize()));
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					if (RoomAt(X, Y) != Room)
					{
						return false;
					}
				}
			}
			return true;
		}

		// 방 경계 (방 칸 → 방향)의 경계 종류. 같은 방이면 None + bInternal
		EBuildingEdge BoundaryOf(int32 X, int32 Y, int32 Dir, bool& bInternal) const
		{
			const int32 Room = RoomAt(X, Y);
			bInternal        = RoomAt(X + GDirX[Dir], Y + GDirY[Dir]) == Room;
			switch (Dir)
			{
			case 0:  return Plan.GetEdgeX(X, Y);
			case 1:  return Plan.GetEdgeX(X + 1, Y);
			case 2:  return Plan.GetEdgeY(X, Y);
			default: return Plan.GetEdgeY(X, Y + 1);
			}
		}

		FRoomProps PrepareRoom(int32 Room) const
		{
			FRoomProps Props_;
			Props_.Room = Room;
			for (int32 Cell : Plan.Rooms[static_cast<size_t>(Room)].Cells)
			{
				const int32 X = Cell % W;
				const int32 Y = Cell / W;
				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					bool                bInternal = false;
					const EBuildingEdge Edge      = BoundaryOf(X, Y, Dir, bInternal);
					const bool          bPassage  = !bInternal && (Edge == EBuildingEdge::Door || Edge == EBuildingEdge::EntranceDoor ||
					                                               (Edge == EBuildingEdge::Open && RoomAt(X + GDirX[Dir], Y + GDirY[Dir]) >= 0));
					if (!bPassage)
					{
						continue;
					}
					const FVector2 Min = CellMin(X, Y);
					// 문 앞 칸은 비운다. 트임(Open) 경계는 칸 전체를 막지 않고 통로 연결 검사로만 지킨다 (트인 거실·주방 경계 전체가 막히지 않게)
					if (Edge != EBuildingEdge::Open)
					{
						Props_.KeepFree.push_back(FRect2{ Min.X, Min.Y, Min.X + CellSize(), Min.Y + CellSize() }.Expanded(-1.0f));
					}
					const float Inset = CellSize() * 0.125f;
					FVector2    Sample(Min.X + CellSize() * 0.5f, Min.Y + CellSize() * 0.5f);
					Sample.X += static_cast<float>(GDirX[Dir]) * (CellSize() * 0.5f - Inset);
					Sample.Y += static_cast<float>(GDirY[Dir]) * (CellSize() * 0.5f - Inset);
					Props_.DoorSamples.push_back(Sample);
				}
			}
			return Props_;
		}

		// 문 앞 점들이 서로 이어지는가 (25cm 격자, 막는 소품을 반 칸 + 여유만큼 키워 통로 폭 확보)
		bool AreDoorsConnected(const FRoomProps& RoomProps, const std::vector<FRect2>& Blocking) const
		{
			if (RoomProps.DoorSamples.size() < 2 && Blocking.empty())
			{
				return true;
			}
			const float    Step   = CellSize() * 0.25f;
			const FVector2 Origin = CellMin(0, 0);
			const int32    GW     = W * 4;
			const int32    GD     = D * 4;
			const auto     Free   = [&](int32 Gx, int32 Gy) {
                if (Gx < 0 || Gy < 0 || Gx >= GW || Gy >= GD || RoomAt(Gx / 4, Gy / 4) != RoomProps.Room)
                {
                    return false;
                }
                const float Px = Origin.X + (static_cast<float>(Gx) + 0.5f) * Step;
                const float Py = Origin.Y + (static_cast<float>(Gy) + 0.5f) * Step;
                for (const FRect2& Rect : Blocking)
                {
                    if (Rect.Expanded(Step * 0.75f).ContainsPoint(Px, Py))
                    {
                        return false;
                    }
                }
                return true;
			};
			const auto ToGrid = [&](const FVector2& Point, int32& Gx, int32& Gy) {
				Gx = static_cast<int32>(std::floor((Point.X - Origin.X) / Step));
				Gy = static_cast<int32>(std::floor((Point.Y - Origin.Y) / Step));
			};
			if (RoomProps.DoorSamples.empty())
			{
				return true;
			}
			int32 Sx, Sy;
			ToGrid(RoomProps.DoorSamples.front(), Sx, Sy);
			if (!Free(Sx, Sy))
			{
				return false;
			}
			std::vector<bool>  Visited(static_cast<size_t>(GW * GD), false);
			std::vector<int32> Stack{ Sy * GW + Sx };
			Visited[static_cast<size_t>(Sy * GW + Sx)] = true;
			while (!Stack.empty())
			{
				const int32 Cell = Stack.back();
				Stack.pop_back();
				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					const int32 Nx = Cell % GW + GDirX[Dir];
					const int32 Ny = Cell / GW + GDirY[Dir];
					if (Free(Nx, Ny) && !Visited[static_cast<size_t>(Ny * GW + Nx)])
					{
						Visited[static_cast<size_t>(Ny * GW + Nx)] = true;
						Stack.push_back(Ny * GW + Nx);
					}
				}
			}
			for (const FVector2& Sample : RoomProps.DoorSamples)
			{
				int32 Gx, Gy;
				ToGrid(Sample, Gx, Gy);
				if (Gx < 0 || Gy < 0 || Gx >= GW || Gy >= GD || !Visited[static_cast<size_t>(Gy * GW + Gx)])
				{
					return false;
				}
			}
			return true;
		}

		struct FCandidate
		{
			FVector2 Center;
			int32    FrontDir = 1;
		};

		// 앞 방향 Dir로 놓인 W×D 소품의 축 정렬 사각형
		static FRect2 MakePropRect(const FVector2& Center, int32 FrontDir, float Width, float Depth)
		{
			const bool  bFrontX = FrontDir == 0 || FrontDir == 1;
			const float Hx      = (bFrontX ? Depth : Width) * 0.5f;
			const float Hy      = (bFrontX ? Width : Depth) * 0.5f;
			return { Center.X - Hx, Center.Y - Hy, Center.X + Hx, Center.Y + Hy };
		}

		static FRect2 MakeClearanceRect(const FVector2& Center, int32 FrontDir, float Width, float Depth, float Clearance)
		{
			const FVector2 Front(static_cast<float>(GDirX[FrontDir]), static_cast<float>(GDirY[FrontDir]));
			const FVector2 ZoneCenter(Center.X + Front.X * (Depth + Clearance) * 0.5f, Center.Y + Front.Y * (Depth + Clearance) * 0.5f);
			return MakePropRect(ZoneCenter, FrontDir, Width, Clearance);
		}

		std::vector<FCandidate> GatherCandidates(const FBuildingPropRule& Rule, int32 Room)
		{
			std::vector<FCandidate> Candidates;
			const float             Gap   = 1.0f;
			const float             Half  = CellSize() * 0.5f;
			const std::vector<int32>& Cells = Plan.Rooms[static_cast<size_t>(Room)].Cells;
			if (Rule.Placement == EBuildingPropPlacement::Wall)
			{
				// 벽 줄: 방향마다 (선 좌표, 따라가는 좌표) 정렬 → 이어진 벽 칸
				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					std::vector<std::pair<int32, int32>> Edges; // (선, 따라)
					for (int32 Cell : Cells)
					{
						bool bInternal = false;
						if (BoundaryOf(Cell % W, Cell / W, Dir, bInternal) == EBuildingEdge::Wall && !bInternal)
						{
							Edges.emplace_back(Dir < 2 ? Cell % W : Cell / W, Dir < 2 ? Cell / W : Cell % W);
						}
					}
					std::sort(Edges.begin(), Edges.end());
					for (size_t Begin = 0; Begin < Edges.size();)
					{
						size_t End = Begin + 1;
						while (End < Edges.size() && Edges[End].first == Edges[Begin].first && Edges[End].second == Edges[End - 1].second + 1)
						{
							++End;
						}
						const float RunLength = static_cast<float>(End - Begin) * CellSize();
						if (Rule.Width <= RunLength + 0.01f)
						{
							const int32 Line   = Edges[Begin].first;
							const int32 Along0 = Edges[Begin].second;
							const int32 Front  = Dir ^ 1; // 안쪽
							for (float T = Rule.Width * 0.5f; T <= RunLength - Rule.Width * 0.5f + 0.01f; T += Half)
							{
								const FVector2 CellOrigin = Dir < 2 ? CellMin(Line, Along0) : CellMin(Along0, Line);
								const float    WallCoord  = (Dir == 1 || Dir == 3) ? CellSize() : 0.0f;
								const float    Inward     = Rule.Depth * 0.5f + Gap;
								FVector2       Center;
								if (Dir < 2)
								{
									Center = FVector2(CellOrigin.X + WallCoord + static_cast<float>(GDirX[Front]) * Inward, CellOrigin.Y + T);
								}
								else
								{
									Center = FVector2(CellOrigin.X + T, CellOrigin.Y + WallCoord + static_cast<float>(GDirY[Front]) * Inward);
								}
								Candidates.push_back({ Center, Front });
							}
						}
						Begin = End;
					}
				}
			}
			else if (Rule.Placement == EBuildingPropPlacement::Corner)
			{
				for (int32 Cell : Cells)
				{
					const int32 X = Cell % W;
					const int32 Y = Cell / W;
					for (int32 DirX : { 0, 1 })
					{
						for (int32 DirY : { 2, 3 })
						{
							bool       bInternalX = false, bInternalY = false;
							const auto EdgeX      = BoundaryOf(X, Y, DirX, bInternalX);
							const auto EdgeY      = BoundaryOf(X, Y, DirY, bInternalY);
							const bool bWallX     = !bInternalX && (EdgeX == EBuildingEdge::Wall || EdgeX == EBuildingEdge::Window);
							const bool bWallY     = !bInternalY && (EdgeY == EBuildingEdge::Wall || EdgeY == EBuildingEdge::Window);
							if (!bWallX || !bWallY)
							{
								continue;
							}
							const FVector2 Min = CellMin(X, Y);
							const FVector2 Corner(Min.X + (DirX == 1 ? CellSize() : 0.0f), Min.Y + (DirY == 3 ? CellSize() : 0.0f));
							// 등을 X 벽에 (앞 = 안쪽 X) 또는 Y 벽에
							for (int32 Back : { DirX, DirY })
							{
								const int32    Front   = Back ^ 1;
								const int32    Side    = Back == DirX ? DirY : DirX; // 옆 벽
								const FVector2 FrontV(static_cast<float>(GDirX[Front]), static_cast<float>(GDirY[Front]));
								const FVector2 AwayV(-static_cast<float>(GDirX[Side]), -static_cast<float>(GDirY[Side]));
								const FVector2 Center(Corner.X + FrontV.X * (Rule.Depth * 0.5f + Gap) + AwayV.X * (Rule.Width * 0.5f + Gap),
								                      Corner.Y + FrontV.Y * (Rule.Depth * 0.5f + Gap) + AwayV.Y * (Rule.Width * 0.5f + Gap));
								Candidates.push_back({ Center, Front });
							}
						}
					}
				}
			}
			else
			{
				std::vector<bool> Mask(static_cast<size_t>(W * D), false);
				for (int32 Cell : Cells)
				{
					Mask[static_cast<size_t>(Cell)] = true;
				}
				const std::vector<FCellRect> Rects = DecomposeRects(W, D, Mask);
				const FCellRect*             Largest = nullptr;
				for (const FCellRect& Rect : Rects)
				{
					if (Largest == nullptr || Rect.W * Rect.D > Largest->W * Largest->D)
					{
						Largest = &Rect;
					}
				}
				if (Largest != nullptr)
				{
					const FVector2 Min = CellMin(Largest->X, Largest->Y);
					const FVector2 Center(Min.X + static_cast<float>(Largest->W) * Half, Min.Y + static_cast<float>(Largest->D) * Half);
					const bool     bLongX = Largest->W >= Largest->D;
					// 너비(로컬 Y)를 긴 축으로: 긴 축이 X면 앞 = ±Y
					const int32 Front = bLongX ? 2 + static_cast<int32>(Rng.Next() & 1) : static_cast<int32>(Rng.Next() & 1);
					// 가운데에서 가까운 순서로 반 칸 격자 (가운데가 막히면 조금 비켜 놓는다)
					const int32 StepsX = Largest->W;
					const int32 StepsY = Largest->D;
					for (int32 Sy = -StepsY; Sy <= StepsY; ++Sy)
					{
						for (int32 Sx = -StepsX; Sx <= StepsX; ++Sx)
						{
							Candidates.push_back({ FVector2(Center.X + static_cast<float>(Sx) * Half * 0.5f, Center.Y + static_cast<float>(Sy) * Half * 0.5f), Front });
						}
					}
					std::stable_sort(Candidates.begin(), Candidates.end(), [&Center](const FCandidate& A, const FCandidate& B) {
						const float DistA = std::abs(A.Center.X - Center.X) + std::abs(A.Center.Y - Center.Y);
						const float DistB = std::abs(B.Center.X - Center.X) + std::abs(B.Center.Y - Center.Y);
						return DistA < DistB;
					});
					return Candidates; // 가운데 우선 순서 유지 (섞지 않음)
				}
			}
			Rng.Shuffle(Candidates);
			return Candidates;
		}

		void PlaceAllProps()
		{
			if (Props.empty())
			{
				return;
			}
			for (int32 Room = 0; Room < static_cast<int32>(Plan.Rooms.size()); ++Room)
			{
				if (Room == Plan.CoreRoom)
				{
					continue;
				}
				FRoomProps RoomProps = PrepareRoom(Room);
				for (const FBuildingPropRule& Rule : Props)
				{
					if (Rule.RoomType != Plan.Rooms[static_cast<size_t>(Room)].Type)
					{
						continue;
					}
					const float Roll  = Rng.Float();
					const int32 Count = Rng.Range(Rule.MinCount, Rule.MaxCount);
					if (Roll >= Rule.Chance || Count <= 0)
					{
						continue;
					}
					int32 Placed = 0;
					for (const FCandidate& Candidate : GatherCandidates(Rule, Room))
					{
						if (Placed >= Count)
						{
							break;
						}
						if (TryPlaceProp(Rule, Candidate, RoomProps))
						{
							++Placed;
						}
					}
				}
			}
		}

		bool TryPlaceProp(const FBuildingPropRule& Rule, const FCandidate& Candidate, FRoomProps& RoomProps)
		{
			const FRect2 Rect = MakePropRect(Candidate.Center, Candidate.FrontDir, Rule.Width, Rule.Depth);
			if (!IsRectInRoom(Rect, RoomProps.Room))
			{
				return false;
			}
			std::vector<FRect2> NewKeepFree;
			if (Rule.bBlocking)
			{
				for (const FRect2& Other : RoomProps.Blocking)
				{
					if (Rect.Overlaps(Other))
					{
						return false;
					}
				}
				for (const FRect2& Zone : RoomProps.KeepFree)
				{
					if (Rect.Overlaps(Zone))
					{
						return false;
					}
				}
				if (Rule.Clearance > 0.0f)
				{
					const FRect2 Zone = MakeClearanceRect(Candidate.Center, Candidate.FrontDir, Rule.Width, Rule.Depth, Rule.Clearance);
					if (!IsRectInRoom(Zone, RoomProps.Room))
					{
						return false;
					}
					for (const FRect2& Other : RoomProps.Blocking)
					{
						if (Zone.Overlaps(Other))
						{
							return false;
						}
					}
					NewKeepFree.push_back(Zone);
				}
				std::vector<FRect2> Blocking = RoomProps.Blocking;
				Blocking.push_back(Rect);
				if (!AreDoorsConnected(RoomProps, Blocking))
				{
					return false;
				}
				RoomProps.Blocking.push_back(Rect);
				RoomProps.KeepFree.insert(RoomProps.KeepFree.end(), NewKeepFree.begin(), NewKeepFree.end());
			}
			Emit(EBuildingPiece::Prop, Rule.Prefab, FVector3(Candidate.Center.X, Candidate.Center.Y, BaseZ()), YawFromDirection(Candidate.FrontDir),
			     FVector3(1.0f, 1.0f, 1.0f), RoomProps.Room, Rule.Name);
			FBuildingPlacement& Placement = Result.Placements.back();
			Placement.FootprintMin        = FVector2(Rect.MinX, Rect.MinY);
			Placement.FootprintMax        = FVector2(Rect.MaxX, Rect.MaxY);
			Placement.bBlocking           = Rule.bBlocking;
			return true;
		}
	};
} // namespace

// ---------------------------------------------------------------- 공개 함수

int32 FBuildingFloorPlan::GetRoom(int32 X, int32 Y) const
{
	if (X < 0 || Y < 0 || X >= Width || Y >= Depth)
	{
		return -1;
	}
	return CellRoom[static_cast<size_t>(Y * Width + X)];
}

size_t FBuildingResult::CountPieces(EBuildingPiece Piece) const
{
	return static_cast<size_t>(std::count_if(Placements.begin(), Placements.end(), [Piece](const FBuildingPlacement& Placement) { return Placement.Piece == Piece; }));
}

uint32 MakeBuildingFloorSeed(const FBuildingConfig& Config, uint32 Seed, int32 Floor)
{
	for (const FBuildingFloorSeed& Fixed : Config.FixedFloorSeeds)
	{
		if (Fixed.Floor == Floor)
		{
			return Fixed.Seed;
		}
	}
	if (!Config.bVaryFloors)
	{
		return Seed;
	}
	return static_cast<uint32>(Mix64((static_cast<uint64>(Seed) << 32) ^ static_cast<uint64>(static_cast<uint32>(Floor) + 0x51ED27u)));
}

FBuildingResult GenerateBuilding(const FBuildingConfig& Config, const std::vector<FBuildingPropRule>& Props, const FBuildingGenerateOptions& Options)
{
	FBuildingResult Result;
	int32           FloorCount = Options.Floors > 0 ? Options.Floors : Config.Floors;
	if (Options.SectionFloor >= 0)
	{
		FloorCount = std::min(FloorCount, Options.SectionFloor + 1);
	}
	FloorCount = std::max(1, FloorCount);
	Result.Floors.reserve(static_cast<size_t>(FloorCount));
	for (int32 Floor = 0; Floor < FloorCount; ++Floor)
	{
		FFloorBuilder Builder(Config, Props, Floor, FloorCount, Options.SectionFloor, MakeBuildingFloorSeed(Config, Options.Seed, Floor), Result);
		Result.Floors.push_back(Builder.Build());
	}
	return Result;
}

bool AreAllRoomsReachable(const FBuildingFloorPlan& Plan, std::string* OutProblem)
{
	const int32 W     = Plan.Width;
	const int32 D     = Plan.Depth;
	int32       Start = -1;
	for (int32 Preferred : { Plan.CoreRoom, Plan.CorridorRoom, 0 })
	{
		if (Start >= 0 || Preferred < 0 || Preferred >= static_cast<int32>(Plan.Rooms.size()) || Plan.Rooms[static_cast<size_t>(Preferred)].Cells.empty())
		{
			continue;
		}
		Start = Plan.Rooms[static_cast<size_t>(Preferred)].Cells.front();
	}
	for (int32 Cell = 0; Start < 0 && Cell < W * D; ++Cell)
	{
		Start = Plan.CellRoom[static_cast<size_t>(Cell)] >= 0 ? Cell : -1;
	}
	if (Start < 0)
	{
		return true; // 방 없음
	}
	std::vector<bool>  Visited(static_cast<size_t>(W * D), false);
	std::vector<int32> Stack{ Start };
	Visited[static_cast<size_t>(Start)] = true;
	while (!Stack.empty())
	{
		const int32 Cell = Stack.back();
		Stack.pop_back();
		const int32 X = Cell % W;
		const int32 Y = Cell / W;
		for (int32 Dir = 0; Dir < 4; ++Dir)
		{
			const int32 Nx = X + GDirX[Dir];
			const int32 Ny = Y + GDirY[Dir];
			if (Plan.GetRoom(Nx, Ny) < 0 || Visited[static_cast<size_t>(Ny * W + Nx)])
			{
				continue;
			}
			EBuildingEdge Edge;
			switch (Dir)
			{
			case 0:  Edge = Plan.GetEdgeX(X, Y); break;
			case 1:  Edge = Plan.GetEdgeX(X + 1, Y); break;
			case 2:  Edge = Plan.GetEdgeY(X, Y); break;
			default: Edge = Plan.GetEdgeY(X, Y + 1); break;
			}
			if (IsPassable(Edge))
			{
				Visited[static_cast<size_t>(Ny * W + Nx)] = true;
				Stack.push_back(Ny * W + Nx);
			}
		}
	}
	for (size_t Room = 0; Room < Plan.Rooms.size(); ++Room)
	{
		for (int32 Cell : Plan.Rooms[Room].Cells)
		{
			if (!Visited[static_cast<size_t>(Cell)])
			{
				if (OutProblem != nullptr)
				{
					*OutProblem = std::format("방 {} '{}' (호실 {})의 칸 ({}, {})에 갈 수 없습니다", Room, Plan.Rooms[Room].Type, Plan.Rooms[Room].Unit + 1, Cell % W,
					                          Cell / W);
				}
				return false;
			}
		}
	}
	return true;
}

FVector3 GetBuildingCellCenter(const FBuildingConfig& Config, int32 X, int32 Y)
{
	return FVector3((static_cast<float>(X) + 0.5f - static_cast<float>(Config.Width) * 0.5f) * Config.CellSize,
	                (static_cast<float>(Y) + 0.5f - static_cast<float>(Config.Depth) * 0.5f) * Config.CellSize, 0.0f);
}
