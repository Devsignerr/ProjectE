#include "Scene/Building/BuildingConfig.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scene/DataJson.h"
#include "Scene/DataTable.h"
#include "Scene/Prefab.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using FJson = DataJson::FJson;

	constexpr const char* GPieceNames[] = { "Floor", "CorridorFloor", "Ceiling", "Roof",   "Wall",    "ExteriorWall", "Door",
	                                        "EntranceDoor", "Window", "Corner", "Stairs", "Railing", "Prop" };
	static_assert(std::size(GPieceNames) == static_cast<size_t>(EBuildingPiece::Count));

	constexpr const char* GCorridorNames[] = { "Center", "Single", "None" };

	// 형식 검사 읽기 (없거나 타입이 다르면 기본값 유지 — 타입이 다르면 경고)
	struct FReader
	{
		std::vector<std::string>* Warnings = nullptr;
		std::string               Where;

		void Warn(const std::string& Message) const
		{
			if (Warnings != nullptr)
			{
				Warnings->push_back(Where.empty() ? Message : Where + ": " + Message);
			}
		}

		void Read(const FJson& Object, const char* Key, float& Out) const
		{
			const auto It = Object.find(Key);
			if (It == Object.end())
			{
				return;
			}
			if (!It->is_number() || !std::isfinite(It->get<double>()))
			{
				Warn(std::format("'{}'는 수여야 합니다", Key));
				return;
			}
			Out = It->get<float>();
		}
		void Read(const FJson& Object, const char* Key, int32& Out) const
		{
			const auto It = Object.find(Key);
			if (It == Object.end())
			{
				return;
			}
			if (!It->is_number())
			{
				Warn(std::format("'{}'는 정수여야 합니다", Key));
				return;
			}
			Out = static_cast<int32>(std::lround(It->get<double>()));
		}
		void Read(const FJson& Object, const char* Key, uint32& Out) const
		{
			const auto It = Object.find(Key);
			if (It == Object.end())
			{
				return;
			}
			if (!It->is_number() || It->get<double>() < 0.0)
			{
				Warn(std::format("'{}'는 0 이상 정수여야 합니다", Key));
				return;
			}
			Out = static_cast<uint32>(It->get<double>());
		}
		void Read(const FJson& Object, const char* Key, bool& Out) const
		{
			const auto It = Object.find(Key);
			if (It == Object.end())
			{
				return;
			}
			if (!It->is_boolean())
			{
				Warn(std::format("'{}'는 true/false여야 합니다", Key));
				return;
			}
			Out = It->get<bool>();
		}
		void Read(const FJson& Object, const char* Key, std::string& Out) const
		{
			const auto It = Object.find(Key);
			if (It == Object.end())
			{
				return;
			}
			if (!It->is_string())
			{
				Warn(std::format("'{}'는 문자열이어야 합니다", Key));
				return;
			}
			Out = It->get<std::string>();
		}
		void Read(const FJson& Object, const char* Key, std::vector<std::string>& Out) const
		{
			const auto It = Object.find(Key);
			if (It == Object.end())
			{
				return;
			}
			if (!It->is_array())
			{
				Warn(std::format("'{}'는 문자열 배열이어야 합니다", Key));
				return;
			}
			Out.clear();
			for (const FJson& Item : *It)
			{
				if (Item.is_string())
				{
					Out.push_back(Item.get<std::string>());
				}
			}
		}
		void Read(const FJson& Object, FBuildingCellRect& Out) const
		{
			Read(Object, "X", Out.X);
			Read(Object, "Y", Out.Y);
			Read(Object, "Width", Out.Width);
			Read(Object, "Depth", Out.Depth);
		}
	};

	FJson RectToJson(const FBuildingCellRect& Rect)
	{
		FJson Json = FJson::object();
		Json["X"]     = Rect.X;
		Json["Y"]     = Rect.Y;
		Json["Width"] = Rect.Width;
		Json["Depth"] = Rect.Depth;
		return Json;
	}

	FJson StringsToJson(const std::vector<std::string>& Values)
	{
		FJson Json = FJson::array();
		for (const std::string& Value : Values)
		{
			Json.push_back(Value);
		}
		return Json;
	}

	std::wstring MakeKey(const std::filesystem::path& Path)
	{
		std::wstring Key = Path.lexically_normal().wstring();
		std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Key;
	}
} // namespace

const char* ToString(EBuildingPiece Piece)
{
	const size_t Index = static_cast<size_t>(Piece);
	return Index < std::size(GPieceNames) ? GPieceNames[Index] : "?";
}

bool TryParseBuildingPiece(std::string_view Text, EBuildingPiece& Out)
{
	for (size_t Index = 0; Index < std::size(GPieceNames); ++Index)
	{
		if (Text == GPieceNames[Index])
		{
			Out = static_cast<EBuildingPiece>(Index);
			return true;
		}
	}
	return false;
}

const std::string& FBuildingConfig::GetKitPiece(EBuildingPiece Piece) const
{
	const std::string& Direct = Kit[static_cast<size_t>(Piece)];
	if (!Direct.empty())
	{
		return Direct;
	}
	switch (Piece)
	{
	case EBuildingPiece::CorridorFloor: return Kit[static_cast<size_t>(EBuildingPiece::Floor)];
	case EBuildingPiece::ExteriorWall:  return Kit[static_cast<size_t>(EBuildingPiece::Wall)];
	case EBuildingPiece::EntranceDoor:  return Kit[static_cast<size_t>(EBuildingPiece::Door)];
	case EBuildingPiece::Roof:          return Kit[static_cast<size_t>(EBuildingPiece::Ceiling)];
	default:                            return Direct;
	}
}

const FBuildingRoomType* FBuildingConfig::FindRoomType(std::string_view Name) const
{
	for (const FBuildingRoomType& Type : Rooms)
	{
		if (Type.Name == Name)
		{
			return &Type;
		}
	}
	return nullptr;
}

const FBuildingUnitTemplate* FBuildingConfig::FindTemplate(std::string_view Name) const
{
	for (const FBuildingUnitTemplate& Template : Templates)
	{
		if (Template.Name == Name)
		{
			return &Template;
		}
	}
	return nullptr;
}

bool FBuildingConfig::IsInside(int32 X, int32 Y) const
{
	if (X < 0 || Y < 0 || X >= Width || Y >= Depth)
	{
		return false;
	}
	if (Cells.empty())
	{
		return true;
	}
	return std::find(Cells.begin(), Cells.end(), std::pair<int32, int32>(X, Y)) != Cells.end();
}

bool FBuildingConfig::FromJsonString(const std::string& Text, FBuildingConfig& Out, std::string* OutError, std::vector<std::string>* OutWarnings)
{
	const FJson Root = DataJson::Parse(Text);
	if (Root.is_discarded() || !Root.is_object())
	{
		if (OutError != nullptr)
		{
			*OutError = "JSON 형식 오류 (최상위는 객체)";
		}
		return false;
	}

	FBuildingConfig Config;
	FReader         Reader{ OutWarnings, {} };
	int32           FileVersion = Version;
	Reader.Read(Root, "Version", FileVersion);
	if (FileVersion > Version)
	{
		Reader.Warn(std::format("더 새 버전 {} (이 엔진은 {}) — 아는 키만 읽습니다", FileVersion, Version));
	}
	Reader.Read(Root, "CellSize", Config.CellSize);
	Reader.Read(Root, "Width", Config.Width);
	Reader.Read(Root, "Depth", Config.Depth);
	Reader.Read(Root, "Floors", Config.Floors);
	Reader.Read(Root, "FloorHeight", Config.FloorHeight);
	Reader.Read(Root, "Seed", Config.Seed);
	Reader.Read(Root, "VaryFloors", Config.bVaryFloors);
	Reader.Read(Root, "MinRoomSide", Config.MinRoomSide);
	Reader.Read(Root, "PropTable", Config.PropTable);

	if (const auto It = Root.find("Cells"); It != Root.end() && It->is_array())
	{
		for (const FJson& Cell : *It)
		{
			if (Cell.is_array() && Cell.size() == 2 && Cell[0].is_number() && Cell[1].is_number())
			{
				Config.Cells.emplace_back(Cell[0].get<int32>(), Cell[1].get<int32>());
			}
			else
			{
				Reader.Warn("Cells 항목은 [x, y]여야 합니다");
			}
		}
	}
	if (const auto It = Root.find("FixedFloorSeeds"); It != Root.end() && It->is_array())
	{
		for (const FJson& Item : *It)
		{
			if (Item.is_object())
			{
				FBuildingFloorSeed Seed;
				Reader.Read(Item, "Floor", Seed.Floor);
				Reader.Read(Item, "Seed", Seed.Seed);
				Config.FixedFloorSeeds.push_back(Seed);
			}
		}
	}
	if (const auto It = Root.find("Core"); It != Root.end() && It->is_object())
	{
		Reader.Read(*It, Config.Core);
	}
	if (const auto It = Root.find("Corridor"); It != Root.end() && It->is_object())
	{
		std::string TypeName = GCorridorNames[static_cast<size_t>(Config.Corridor)];
		Reader.Read(*It, "Type", TypeName);
		bool bFound = false;
		for (size_t Index = 0; Index < std::size(GCorridorNames); ++Index)
		{
			if (TypeName == GCorridorNames[Index])
			{
				Config.Corridor = static_cast<EBuildingCorridor>(Index);
				bFound          = true;
			}
		}
		if (!bFound)
		{
			Reader.Warn(std::format("알 수 없는 복도 형식 '{}' → Center", TypeName));
		}
		Reader.Read(*It, "Width", Config.CorridorWidth);
		Reader.Read(*It, "Entrance", Config.bEntrance);
	}
	if (const auto It = Root.find("Units"); It != Root.end() && It->is_object())
	{
		Reader.Read(*It, "MinWidth", Config.UnitMinWidth);
		Reader.Read(*It, "MaxWidth", Config.UnitMaxWidth);
	}
	if (const auto It = Root.find("Rooms"); It != Root.end() && It->is_array())
	{
		for (const FJson& Item : *It)
		{
			if (!Item.is_object())
			{
				continue;
			}
			FBuildingRoomType Type;
			Reader.Read(Item, "Type", Type.Name);
			FReader Room{ OutWarnings, "방 종류 '" + Type.Name + "'" };
			Room.Read(Item, "Weight", Type.Weight);
			Room.Read(Item, "MinArea", Type.MinArea);
			Room.Read(Item, "Required", Type.bRequired);
			Room.Read(Item, "Chance", Type.Chance);
			Room.Read(Item, "Entry", Type.bEntry);
			Room.Read(Item, "Hub", Type.bHub);
			Room.Read(Item, "NeedsWindow", Type.bNeedsWindow);
			Room.Read(Item, "WindowChance", Type.WindowChance);
			Room.Read(Item, "Adjacent", Type.Adjacent);
			Room.Read(Item, "OpenTo", Type.OpenTo);
			Room.Read(Item, "Floor", Type.FloorPiece);
			if (Type.Name.empty())
			{
				Reader.Warn("이름 없는 방 종류는 건너뜁니다");
				continue;
			}
			Config.Rooms.push_back(std::move(Type));
		}
	}
	if (const auto It = Root.find("Templates"); It != Root.end() && It->is_array())
	{
		for (const FJson& Item : *It)
		{
			if (!Item.is_object())
			{
				continue;
			}
			FBuildingUnitTemplate Template;
			Reader.Read(Item, "Name", Template.Name);
			Reader.Read(Item, "Width", Template.Width);
			Reader.Read(Item, "Depth", Template.Depth);
			if (const auto Rooms = Item.find("Rooms"); Rooms != Item.end() && Rooms->is_array())
			{
				for (const FJson& RoomJson : *Rooms)
				{
					if (RoomJson.is_object())
					{
						FBuildingTemplateRoom Room;
						Reader.Read(RoomJson, "Type", Room.Type);
						Reader.Read(RoomJson, Room.Rect);
						Template.Rooms.push_back(std::move(Room));
					}
				}
			}
			Config.Templates.push_back(std::move(Template));
		}
	}
	if (const auto It = Root.find("FixedUnits"); It != Root.end() && It->is_array())
	{
		for (const FJson& Item : *It)
		{
			if (Item.is_object())
			{
				FBuildingFixedUnit Fixed;
				Reader.Read(Item, "Floor", Fixed.Floor);
				Reader.Read(Item, "Unit", Fixed.Unit);
				Reader.Read(Item, "Template", Fixed.Template);
				Config.FixedUnits.push_back(std::move(Fixed));
			}
		}
	}
	if (const auto It = Root.find("Kit"); It != Root.end() && It->is_object())
	{
		for (auto Entry = It->begin(); Entry != It->end(); ++Entry)
		{
			EBuildingPiece Piece;
			if (!TryParseBuildingPiece(Entry.key(), Piece) || Piece == EBuildingPiece::Prop)
			{
				Reader.Warn(std::format("알 수 없는 키트 조각 '{}'", Entry.key()));
				continue;
			}
			if (Entry->is_string())
			{
				Config.Kit[static_cast<size_t>(Piece)] = Entry->get<std::string>();
			}
		}
	}

	// 의미 검사 (값을 안전한 범위로 맞춘다)
	if (!(Config.CellSize > 1.0f))
	{
		Reader.Warn("CellSize는 1보다 커야 합니다 → 100");
		Config.CellSize = 100.0f;
	}
	if (!(Config.FloorHeight > 1.0f))
	{
		Reader.Warn("FloorHeight는 1보다 커야 합니다 → 260");
		Config.FloorHeight = 260.0f;
	}
	Config.Width         = std::clamp(Config.Width, 1, 256);
	Config.Depth         = std::clamp(Config.Depth, 1, 256);
	Config.Floors        = std::clamp(Config.Floors, 1, 100);
	Config.CorridorWidth = std::clamp(Config.CorridorWidth, 1, std::max(1, Config.Depth));
	Config.MinRoomSide   = std::clamp(Config.MinRoomSide, 1, 16);
	Config.UnitMinWidth  = std::clamp(Config.UnitMinWidth, 1, 256);
	Config.UnitMaxWidth  = std::clamp(Config.UnitMaxWidth, Config.UnitMinWidth, 256);
	if (!Config.Core.IsEmpty() &&
	    (Config.Core.X < 0 || Config.Core.Y < 0 || Config.Core.X + Config.Core.Width > Config.Width || Config.Core.Y + Config.Core.Depth > Config.Depth))
	{
		Reader.Warn("계단실(Core)이 건물 밖으로 나갑니다 → 계단실 없음");
		Config.Core = {};
	}
	for (const FBuildingFixedUnit& Fixed : Config.FixedUnits)
	{
		if (Config.FindTemplate(Fixed.Template) == nullptr)
		{
			Reader.Warn(std::format("고정 호실 ({}층 {}번)의 템플릿 '{}'가 없습니다", Fixed.Floor, Fixed.Unit, Fixed.Template));
		}
	}
	Out = std::move(Config);
	return true;
}

std::string FBuildingConfig::ToJsonString() const
{
	FJson Root = FJson::object();
	Root["Version"]     = Version;
	Root["CellSize"]    = DataJson::FloatToJson(CellSize);
	Root["Width"]       = Width;
	Root["Depth"]       = Depth;
	if (!Cells.empty())
	{
		FJson CellsJson = FJson::array();
		for (const auto& [X, Y] : Cells)
		{
			CellsJson.push_back(FJson::array({ X, Y }));
		}
		Root["Cells"] = std::move(CellsJson);
	}
	Root["Floors"]      = Floors;
	Root["FloorHeight"] = DataJson::FloatToJson(FloorHeight);
	Root["Seed"]        = Seed;
	Root["VaryFloors"]  = bVaryFloors;
	if (!FixedFloorSeeds.empty())
	{
		FJson Seeds = FJson::array();
		for (const FBuildingFloorSeed& FloorSeed : FixedFloorSeeds)
		{
			FJson Item     = FJson::object();
			Item["Floor"]  = FloorSeed.Floor;
			Item["Seed"]   = FloorSeed.Seed;
			Seeds.push_back(std::move(Item));
		}
		Root["FixedFloorSeeds"] = std::move(Seeds);
	}
	Root["Core"] = RectToJson(Core);
	{
		FJson CorridorJson       = FJson::object();
		CorridorJson["Type"]     = GCorridorNames[static_cast<size_t>(Corridor)];
		CorridorJson["Width"]    = CorridorWidth;
		CorridorJson["Entrance"] = bEntrance;
		Root["Corridor"]         = std::move(CorridorJson);
	}
	{
		FJson UnitsJson       = FJson::object();
		UnitsJson["MinWidth"] = UnitMinWidth;
		UnitsJson["MaxWidth"] = UnitMaxWidth;
		Root["Units"]         = std::move(UnitsJson);
	}
	Root["MinRoomSide"] = MinRoomSide;
	FJson RoomsJson     = FJson::array();
	for (const FBuildingRoomType& Type : Rooms)
	{
		FJson Item            = FJson::object();
		Item["Type"]          = Type.Name;
		Item["Weight"]        = DataJson::FloatToJson(Type.Weight);
		Item["MinArea"]       = Type.MinArea;
		Item["Required"]      = Type.bRequired;
		Item["Chance"]        = DataJson::FloatToJson(Type.Chance);
		Item["Entry"]         = Type.bEntry;
		Item["Hub"]           = Type.bHub;
		Item["NeedsWindow"]   = Type.bNeedsWindow;
		Item["WindowChance"]  = DataJson::FloatToJson(Type.WindowChance);
		Item["Adjacent"]      = StringsToJson(Type.Adjacent);
		Item["OpenTo"]        = StringsToJson(Type.OpenTo);
		Item["Floor"]         = Type.FloorPiece;
		RoomsJson.push_back(std::move(Item));
	}
	Root["Rooms"]       = std::move(RoomsJson);
	FJson TemplatesJson = FJson::array();
	for (const FBuildingUnitTemplate& Template : Templates)
	{
		FJson Item      = FJson::object();
		Item["Name"]    = Template.Name;
		Item["Width"]   = Template.Width;
		Item["Depth"]   = Template.Depth;
		FJson TemplateRooms = FJson::array();
		for (const FBuildingTemplateRoom& Room : Template.Rooms)
		{
			FJson RoomJson    = RectToJson(Room.Rect);
			RoomJson["Type"]  = Room.Type;
			TemplateRooms.push_back(std::move(RoomJson));
		}
		Item["Rooms"] = std::move(TemplateRooms);
		TemplatesJson.push_back(std::move(Item));
	}
	Root["Templates"] = std::move(TemplatesJson);
	FJson FixedJson   = FJson::array();
	for (const FBuildingFixedUnit& Fixed : FixedUnits)
	{
		FJson Item        = FJson::object();
		Item["Floor"]     = Fixed.Floor;
		Item["Unit"]      = Fixed.Unit;
		Item["Template"]  = Fixed.Template;
		FixedJson.push_back(std::move(Item));
	}
	Root["FixedUnits"] = std::move(FixedJson);
	FJson KitJson      = FJson::object();
	for (size_t Index = 0; Index < Kit.size(); ++Index)
	{
		if (!Kit[Index].empty())
		{
			KitJson[GPieceNames[Index]] = Kit[Index];
		}
	}
	Root["Kit"]       = std::move(KitJson);
	Root["PropTable"] = PropTable;
	return DataJson::FormatDocument(Root);
}

std::vector<FBuildingPropRule> MakeBuildingPropRules(const FDataTable& Table, std::vector<std::string>* OutWarnings)
{
	std::vector<FBuildingPropRule> Rules;
	if (Table.Struct == nullptr)
	{
		if (OutWarnings != nullptr)
		{
			OutWarnings->push_back("소품 테이블의 구조체를 찾지 못했습니다: " + Table.StructPath);
		}
		return Rules;
	}
	Rules.reserve(Table.GetRows().size());
	for (const FDataRow& Row : Table.GetRows())
	{
		FBuildingPropRule Rule;
		Rule.Name      = Row.Name;
		Rule.RoomType  = Table.GetString(Row.Name, "RoomType");
		Rule.Prefab    = Table.GetString(Row.Name, "Prefab");
		Rule.Width     = std::max(1.0f, Table.GetFloat(Row.Name, "Width", Rule.Width));
		Rule.Depth     = std::max(1.0f, Table.GetFloat(Row.Name, "Depth", Rule.Depth));
		Rule.Clearance = std::max(0.0f, Table.GetFloat(Row.Name, "Clearance", Rule.Clearance));
		Rule.MinCount  = std::max(0, Table.GetInt(Row.Name, "MinCount", Rule.MinCount));
		Rule.MaxCount  = std::max(Rule.MinCount, Table.GetInt(Row.Name, "MaxCount", Rule.MaxCount));
		Rule.Chance    = std::clamp(Table.GetFloat(Row.Name, "Chance", Rule.Chance), 0.0f, 1.0f);
		Rule.bBlocking = Table.GetBool(Row.Name, "Blocking", Rule.bBlocking);
		const std::string Placement = Table.GetString(Row.Name, "Placement", "Wall");
		Rule.Placement = Placement == "Corner" ? EBuildingPropPlacement::Corner
		               : Placement == "Center" ? EBuildingPropPlacement::Center
		                                       : EBuildingPropPlacement::Wall;
		if (Rule.Prefab.empty() || Rule.RoomType.empty())
		{
			if (OutWarnings != nullptr)
			{
				OutWarnings->push_back(std::format("소품 '{}': RoomType/Prefab이 비어 건너뜁니다", Row.Name));
			}
			continue;
		}
		Rules.push_back(std::move(Rule));
	}
	return Rules;
}

FBuildingLibrary& FBuildingLibrary::Get()
{
	static FBuildingLibrary Instance;
	return Instance;
}

std::shared_ptr<const FBuildingConfig> FBuildingLibrary::Load(const std::string& AssetPath, std::string* OutError)
{
	const std::filesystem::path File = FPrefabLibrary::Get().ResolveAssetPath(AssetPath);
	const std::wstring          Key  = MakeKey(File);
	for (const auto& [CachedKey, Config] : Cache)
	{
		if (CachedKey == Key)
		{
			return Config;
		}
	}
	std::string Text;
	if (!FFileSystem::ReadTextFile(File, Text))
	{
		if (OutError != nullptr)
		{
			*OutError = "건물 설정 파일을 읽지 못했습니다: " + AssetPath;
		}
		return nullptr;
	}
	auto                     Config = std::make_shared<FBuildingConfig>();
	std::vector<std::string> Warnings;
	std::string              Error;
	if (!FBuildingConfig::FromJsonString(Text, *Config, &Error, &Warnings))
	{
		if (OutError != nullptr)
		{
			*OutError = std::format("건물 설정 '{}': {}", AssetPath, Error);
		}
		return nullptr;
	}
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogScene, Warning, "[건물] {}: {}", AssetPath, Warning);
	}
	Cache.emplace_back(Key, Config);
	return Config;
}

void FBuildingLibrary::Invalidate()
{
	Cache.clear();
}
