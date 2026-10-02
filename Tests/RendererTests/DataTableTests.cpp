#include "Core/StringConv.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/DataCsv.h"
#include "Scene/DataLibrary.h"
#include "Scene/Prefab.h"

#include <filesystem>
#include <fstream>
#include <sstream>

// 데이터 테이블/에셋 (Scene/DataTable.h 머리 주석 규칙)
namespace
{
	namespace fs = std::filesystem;

	// 모든 필드 타입을 가진 구조체
	const char* WeaponStructJson = R"({
  "Version": 1, "Name": "Weapon", "Description": "무기",
  "Fields": [
    { "Name": "Equippable", "Type": "Bool", "Default": true },
    { "Name": "Level", "Type": "Int", "Default": 1 },
    { "Name": "Damage", "Type": "Float", "Default": 10.5, "Description": "기본 피해" },
    { "Name": "DisplayName", "Type": "String" },
    { "Name": "Lore", "Type": "Text", "Default": "줄1\n줄2" },
    { "Name": "Offset2", "Type": "Vector2", "Default": [1, 2] },
    { "Name": "Offset", "Type": "Vector3" },
    { "Name": "Extra", "Type": "Vector4", "Default": [1, 2, 3, 4] },
    { "Name": "Tint", "Type": "Color", "Default": "#FF000080" },
    { "Name": "Rarity", "Type": "Enum", "Values": ["Common", "Rare", "Epic"], "Default": "Rare" },
    { "Name": "Icon", "Type": "Asset", "Filter": ".png;.jpg" },
    { "Name": "Upgrade", "Type": "RowRef", "Table": "Data/Weapons.etable" },
    { "Name": "Tags", "Type": "Array", "Element": "String", "Default": ["a", "b"] },
    { "Name": "Combo", "Type": "Array", "Element": "RowRef", "Table": "Data/Weapons.etable" }
  ]
})";

	std::shared_ptr<const FDataStruct> ParseStruct(const char* Json, FDataLoadReport* Report = nullptr)
	{
		auto Struct = std::make_shared<FDataStruct>();
		E_EXPECT_TRUE(FDataStruct::FromJsonString(Json, *Struct, Report));
		return Struct;
	}

	FDataStructResolver ResolverFor(std::shared_ptr<const FDataStruct> Struct)
	{
		return [Struct](const std::string& Path) { return Path == "Data/Weapon.estruct" ? Struct : nullptr; };
	}

	bool HasWarning(const std::vector<std::string>& Warnings, std::string_view Needle)
	{
		for (const std::string& Warning : Warnings)
		{
			if (Warning.find(Needle) != std::string::npos)
			{
				return true;
			}
		}
		return false;
	}

	void WriteText(const fs::path& Path, const std::string& Text)
	{
		fs::create_directories(Path.parent_path());
		std::ofstream(Path, std::ios::binary | std::ios::trunc) << Text;
	}

	std::string ReadText(const fs::path& Path)
	{
		std::ifstream     File(Path, std::ios::binary);
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		return Buffer.str();
	}

	// FPrefabLibrary(=데이터 라이브러리 경로 기준) Content 폴더를 임시 폴더로 바꾸고 되돌린다
	struct FTempDataContent
	{
		fs::path Content;
		fs::path Previous;

		explicit FTempDataContent(const char* Name)
		{
			Content = FTestRegistry::GetTempDirectory() / "ProjectE_DataTests" / Name;
			std::error_code ErrorCode;
			fs::remove_all(Content, ErrorCode);
			fs::create_directories(Content);
			Previous = FPrefabLibrary::Get().GetContentDirectory();
			FPrefabLibrary::Get().SetContentDirectory(Content);
			FDataLibrary::Get().Invalidate();
		}
		~FTempDataContent()
		{
			FPrefabLibrary::Get().SetContentDirectory(Previous);
			FDataLibrary::Get().Invalidate();
			std::error_code ErrorCode;
			fs::remove_all(Content, ErrorCode);
		}
	};
} // namespace

E_TEST(DataTable_StructParsesAllTypesAndDefaults)
{
	FDataLoadReport Report;
	const auto      Struct = ParseStruct(WeaponStructJson, &Report);
	E_EXPECT_EQ(Report.Warnings.size(), static_cast<size_t>(0));
	E_EXPECT_EQ(Struct->Name, std::string("Weapon"));
	E_EXPECT_EQ(Struct->Fields.size(), static_cast<size_t>(14));
	E_EXPECT_TRUE(Struct->GetField("Equippable")->Default.AsBool());
	E_EXPECT_EQ(Struct->GetField("Level")->Default.AsInt(), 1);
	E_EXPECT_NEAR(Struct->GetField("Damage")->Default.AsFloat(), 10.5f, 1.0e-6f);
	E_EXPECT_EQ(Struct->GetField("DisplayName")->Default.AsString(), std::string());
	E_EXPECT_EQ(Struct->GetField("Lore")->Default.AsString(), std::string("줄1\n줄2"));
	E_EXPECT_TRUE(Struct->GetField("Offset2")->Default.AsVector2() == FVector2(1.0f, 2.0f));
	E_EXPECT_TRUE(Struct->GetField("Offset")->Default.AsVector3() == FVector3(0.0f, 0.0f, 0.0f));
	E_EXPECT_TRUE(Struct->GetField("Extra")->Default.AsVector4() == FVector4(1.0f, 2.0f, 3.0f, 4.0f));
	const FVector4 Tint = Struct->GetField("Tint")->Default.AsVector4();
	E_EXPECT_NEAR(Tint.X, 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(Tint.W, 128.0f / 255.0f, 1.0e-6f);
	E_EXPECT_EQ(Struct->GetField("Rarity")->Default.AsString(), std::string("Rare"));
	E_EXPECT_EQ(Struct->GetField("Icon")->GetFilterExtensions().size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Struct->GetField("Upgrade")->Table, std::string("Data/Weapons.etable"));
	E_EXPECT_EQ(Struct->GetField("Tags")->Default.AsArray().size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Struct->GetField("Combo")->ElementType == EDataFieldType::RowRef);
	// 타입 기본값: 색 흰색, Enum 첫 값
	E_EXPECT_TRUE(FDataField::MakeTypeDefault(EDataFieldType::Color, {}).AsVector4() == FVector4(1.0f, 1.0f, 1.0f, 1.0f));
	E_EXPECT_EQ(FDataField::MakeTypeDefault(EDataFieldType::Enum, { "A", "B" }).AsString(), std::string("A"));

	// 잘못된 정의: 알 수 없는 타입/예약어/중복/배열의 배열은 버리고, 맞지 않는 기본값은 타입 기본값 + 경고 (크래시 없음)
	FDataLoadReport BadReport;
	const auto      Bad = ParseStruct(R"({ "Fields": [
		{ "Name": "A", "Type": "Quaternion" }, { "Name": "Name", "Type": "Int" }, { "Name": "B", "Type": "Int", "Default": "x" },
		{ "Name": "B", "Type": "Float" }, { "Name": "C", "Type": "Array", "Element": "Array" }, { "Name": "9X", "Type": "Int" }, 42 ] })",
	                                  &BadReport);
	E_EXPECT_EQ(Bad->Fields.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Bad->Fields[0].Default.AsInt(), 0);
	E_EXPECT_TRUE(HasWarning(BadReport.Warnings, "Quaternion"));
	E_EXPECT_TRUE(HasWarning(BadReport.Warnings, "타입이 맞지 않습니다"));
	E_EXPECT_TRUE(BadReport.Warnings.size() >= 6);

	// 깨진 JSON은 false (크래시 없음)
	FDataStruct Broken;
	std::string Error;
	E_EXPECT_FALSE(FDataStruct::FromJsonString("{ not json", Broken, nullptr, &Error));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_TRUE(FDataStruct::FromJsonString(R"({ "Name": 5, "Fields": "x" })", Broken)); // 타입이 틀린 키도 예외 없이
}

E_TEST(DataTable_TableValuesDefaultsAndWarnings)
{
	const auto      Struct = ParseStruct(WeaponStructJson);
	FDataTable      Table;
	FDataLoadReport Report;
	E_EXPECT_TRUE(FDataTable::FromJsonString(R"({ "Version": 1, "Struct": "Data/Weapon.estruct", "Rows": [
		{ "Name": "Sword", "Values": { "Damage": 12, "Level": 3, "Rarity": "Epic", "Offset": [1, 2, 3], "Tint": [0, 1, 0],
		                               "Tags": ["fast", 5], "Upgrade": "Axe", "Combo": ["Axe", "Sword"], "Icon": "Icons/Sword.png", "Secret": { "X": 1 } } },
		{ "Name": "Axe", "Values": { "Damage": "big", "Rarity": "Mythic", "Level": 2.0 } },
		{ "Name": "Sword", "Values": {} },
		{ "Values": {} }
	] })",
	                                         ResolverFor(Struct), Table, &Report));
	E_EXPECT_TRUE(Table.Struct == Struct);
	E_EXPECT_EQ(Table.GetRowCount(), 4);
	// 행 순서 유지 + 중복 이름은 바꿔서 보존, 이름 없는 행도 생성
	const std::vector<std::string> Names = Table.GetRowNames();
	E_EXPECT_EQ(Names[0], std::string("Sword"));
	E_EXPECT_EQ(Names[1], std::string("Axe"));
	E_EXPECT_EQ(Names[2], std::string("Sword_2"));
	E_EXPECT_EQ(Names[3], std::string("Row"));
	E_EXPECT_TRUE(HasWarning(Report.Warnings, "중복"));

	// 값 + 형식 지정 읽기
	E_EXPECT_NEAR(Table.GetFloat("Sword", "Damage"), 12.0f, 1.0e-6f);
	E_EXPECT_EQ(Table.GetInt("Sword", "Level"), 3);
	E_EXPECT_EQ(Table.GetString("Sword", "Rarity"), std::string("Epic"));
	E_EXPECT_TRUE(Table.GetVector3("Sword", "Offset") == FVector3(1.0f, 2.0f, 3.0f));
	E_EXPECT_TRUE(Table.GetVector4("Sword", "Tint") == FVector4(0.0f, 1.0f, 0.0f, 1.0f)); // 색 3성분 → 알파 1
	E_EXPECT_TRUE(Table.GetBool("Sword", "Equippable"));                                  // 없는 값 = 구조체 기본값
	E_EXPECT_EQ(Table.GetString("Sword", "Upgrade"), std::string("Axe"));
	// 배열 요소 타입 오류는 그 요소만 기본값
	const FDataValue* Tags = Table.FindValue("Sword", "Tags");
	E_EXPECT_TRUE(Tags != nullptr && Tags->AsArray().size() == 2 && Tags->AsArray()[1].AsString().empty());
	// 타입 오류/목록에 없는 Enum → 기본값 + 경고, 정수로 쓴 실수(2.0)는 Int로 허용
	E_EXPECT_NEAR(Table.GetFloat("Axe", "Damage"), 10.5f, 1.0e-6f);
	E_EXPECT_EQ(Table.GetString("Axe", "Rarity"), std::string("Rare"));
	E_EXPECT_EQ(Table.GetInt("Axe", "Level"), 2);
	E_EXPECT_TRUE(HasWarning(Report.Warnings, "Mythic"));
	E_EXPECT_TRUE(HasWarning(Report.Warnings, "Damage"));
	// 구조체에 없는 필드는 경고 + 원문 보존 → 저장 시 다시 쓴다
	E_EXPECT_TRUE(HasWarning(Report.Warnings, "Secret"));
	E_EXPECT_EQ(Table.FindRow("Sword")->Record.Unknown.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Table.ToJsonString().find("\"Secret\":{\"X\":1}") != std::string::npos);
	// 없는 행/필드
	E_EXPECT_TRUE(Table.FindValue("Nope", "Damage") == nullptr);
	E_EXPECT_TRUE(Table.FindValue("Sword", "Nope") == nullptr);
	E_EXPECT_EQ(Table.GetInt("Nope", "Level", -7), -7);

	// 구조체를 못 찾으면 경고 + 값은 모두 원문으로 보존 (조회는 실패)
	FDataTable      Orphan;
	FDataLoadReport OrphanReport;
	E_EXPECT_TRUE(FDataTable::FromJsonString(R"({ "Struct": "Data/Missing.estruct", "Rows": [ { "Name": "A", "Values": { "Damage": 3 } } ] })",
	                                         ResolverFor(Struct), Orphan, &OrphanReport));
	E_EXPECT_TRUE(Orphan.Struct == nullptr);
	E_EXPECT_TRUE(HasWarning(OrphanReport.Warnings, "구조체를 찾을 수 없습니다"));
	E_EXPECT_TRUE(Orphan.FindValue("A", "Damage") == nullptr);
	E_EXPECT_TRUE(Orphan.ToJsonString().find("\"Damage\":3") != std::string::npos);
	// 나중에 구조체를 연결하면 원문이 되살아난다
	Orphan.Rebind(Struct);
	E_EXPECT_NEAR(Orphan.GetFloat("A", "Damage"), 3.0f, 1.0e-6f);
}

E_TEST(DataTable_JsonRoundTrip)
{
	const auto Struct = ParseStruct(WeaponStructJson);
	FDataTable Table;
	E_EXPECT_TRUE(FDataTable::FromJsonString(R"({ "Struct": "Data/Weapon.estruct", "Rows": [
		{ "Name": "Sword", "Values": { "Damage": 0.1, "Lore": "가\n\"나\"", "Offset2": [0.25, -3], "Tags": ["x|y", "z"], "Extra": [1e-3, 2, 3, 4] } },
		{ "Name": "Axe", "Values": { "Upgrade": "Sword" } } ] })",
	                                         ResolverFor(Struct), Table));
	const std::string Json = Table.ToJsonString();
	// float는 가장 짧은 표현, 행 하나 = 한 줄
	E_EXPECT_TRUE(Json.find("\"Damage\":0.1,") != std::string::npos);
	E_EXPECT_TRUE(Json.find("\n    {\"Name\":\"Axe\"") != std::string::npos);

	FDataTable Again;
	E_EXPECT_TRUE(FDataTable::FromJsonString(Json, ResolverFor(Struct), Again));
	E_EXPECT_EQ(Again.GetRowCount(), 2);
	for (const FDataRow& Row : Table.GetRows())
	{
		const FDataRow* Other = Again.FindRow(Row.Name);
		E_EXPECT_TRUE(Other != nullptr && Other->Record.Values == Row.Record.Values);
	}
	E_EXPECT_EQ(Again.ToJsonString(), Json); // 두 번째 저장은 같은 글자

	// 구조체도 왕복
	const std::string StructJson = Struct->ToJsonString();
	FDataStruct       StructAgain;
	E_EXPECT_TRUE(FDataStruct::FromJsonString(StructJson, StructAgain));
	E_EXPECT_EQ(StructAgain.ToJsonString(), StructJson);
	E_EXPECT_EQ(StructAgain.Fields.size(), Struct->Fields.size());
	for (size_t Index = 0; Index < Struct->Fields.size(); ++Index)
	{
		E_EXPECT_TRUE(StructAgain.Fields[Index].Default == Struct->Fields[Index].Default);
		E_EXPECT_TRUE(StructAgain.Fields[Index].Type == Struct->Fields[Index].Type);
	}

	// 데이터 에셋 왕복
	FDataAsset Asset;
	E_EXPECT_TRUE(FDataAsset::FromJsonString(R"({ "Version": 1, "Struct": "Data/Weapon.estruct", "Values": { "Level": 9, "Tint": "#00FF00" } })",
	                                         ResolverFor(Struct), Asset));
	E_EXPECT_EQ(Asset.GetInt("Level"), 9);
	E_EXPECT_TRUE(Asset.GetVector4("Tint") == FVector4(0.0f, 1.0f, 0.0f, 1.0f));
	E_EXPECT_NEAR(Asset.GetFloat("Damage"), 10.5f, 1.0e-6f);
	FDataAsset AssetAgain;
	E_EXPECT_TRUE(FDataAsset::FromJsonString(Asset.ToJsonString(), ResolverFor(Struct), AssetAgain));
	E_EXPECT_TRUE(AssetAgain.Record.Values == Asset.Record.Values);
}

E_TEST(DataTable_MutationApi)
{
	const auto Struct = ParseStruct(WeaponStructJson);
	FDataTable Table;
	Table.StructPath = "Data/Weapon.estruct";
	Table.Struct     = Struct;
	std::string Error;
	E_EXPECT_TRUE(Table.AddRow("Sword") != nullptr);
	E_EXPECT_TRUE(Table.AddRow("Axe") != nullptr);
	E_EXPECT_TRUE(Table.AddRow("Dagger", 0) != nullptr); // 맨 앞
	E_EXPECT_TRUE(Table.AddRow("Sword", -1, &Error) == nullptr); // 중복
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_TRUE(Table.AddRow(" Bad") == nullptr); // 앞 공백
	E_EXPECT_TRUE(Table.AddRow("") == nullptr);
	E_EXPECT_EQ(Table.GetRowNames()[0], std::string("Dagger"));
	E_EXPECT_NEAR(Table.GetFloat("Sword", "Damage"), 10.5f, 1.0e-6f); // 새 행 = 기본값

	E_EXPECT_TRUE(Table.SetValue("Sword", "Damage", FDataValue::MakeFloat(30.0f)));
	E_EXPECT_TRUE(Table.SetValue("Sword", "Damage", FDataValue::MakeInt(31))); // 정수 → 실수 허용
	E_EXPECT_NEAR(Table.GetFloat("Sword", "Damage"), 31.0f, 1.0e-6f);
	E_EXPECT_FALSE(Table.SetValue("Sword", "Damage", FDataValue::MakeString("x"), &Error));
	E_EXPECT_FALSE(Table.SetValue("Sword", "Rarity", FDataValue::MakeString("Mythic"))); // 목록 밖 Enum
	E_EXPECT_TRUE(Table.SetValue("Sword", "Rarity", FDataValue::MakeString("Epic")));
	E_EXPECT_FALSE(Table.SetValue("Sword", "Tags", FDataValue::MakeArray({ FDataValue::MakeInt(1) }))); // 요소 타입
	E_EXPECT_TRUE(Table.SetValue("Sword", "Tags", FDataValue::MakeArray({ FDataValue::MakeString("t") })));
	E_EXPECT_FALSE(Table.SetValue("Nope", "Damage", FDataValue::MakeFloat(1.0f)));
	E_EXPECT_FALSE(Table.SetValue("Sword", "Nope", FDataValue::MakeFloat(1.0f)));

	E_EXPECT_TRUE(Table.DuplicateRow("Sword", "Sword2") != nullptr);
	E_EXPECT_EQ(Table.FindRowIndex("Sword2"), Table.FindRowIndex("Sword") + 1);
	E_EXPECT_NEAR(Table.GetFloat("Sword2", "Damage"), 31.0f, 1.0e-6f);
	E_EXPECT_EQ(Table.MakeUniqueRowName("Sword"), std::string("Sword_2"));
	E_EXPECT_TRUE(Table.RenameRow("Sword2", "Greatsword"));
	E_EXPECT_FALSE(Table.RenameRow("Greatsword", "Axe")); // 이미 있음
	E_EXPECT_TRUE(Table.HasRow("Greatsword") && !Table.HasRow("Sword2"));
	E_EXPECT_TRUE(Table.MoveRow(Table.FindRowIndex("Greatsword"), 0));
	E_EXPECT_EQ(Table.GetRowNames()[0], std::string("Greatsword"));
	E_EXPECT_EQ(Table.FindRowIndex("Greatsword"), 0); // 색인도 갱신
	E_EXPECT_FALSE(Table.MoveRow(0, 99));
	E_EXPECT_TRUE(Table.RemoveRow("Axe"));
	E_EXPECT_FALSE(Table.RemoveRow("Axe"));
	E_EXPECT_EQ(Table.GetRowCount(), 3);

	// 구조체 없는 테이블은 편집 실패 (크래시 없음)
	FDataTable Empty;
	E_EXPECT_TRUE(Empty.AddRow("A") == nullptr);

	// 데이터 에셋 값 설정
	FDataAsset Asset;
	Asset.Struct = Struct;
	Asset.ResetToDefaults();
	E_EXPECT_TRUE(Asset.SetValue("Offset", FDataValue::MakeVector3(FVector3(1.0f, 2.0f, 3.0f))));
	E_EXPECT_FALSE(Asset.SetValue("Offset", FDataValue::MakeVector2(FVector2(1.0f, 2.0f))));
	E_EXPECT_TRUE(Asset.GetVector3("Offset") == FVector3(1.0f, 2.0f, 3.0f));
}

E_TEST(DataTable_StructEditAndRebindMigration)
{
	auto Struct = std::make_shared<FDataStruct>();
	std::string Error;
	FDataField  Damage;
	Damage.Name    = "Damage";
	Damage.Type    = EDataFieldType::Int;
	Damage.Default = FDataValue::MakeInt(5);
	E_EXPECT_TRUE(Struct->AddField(Damage));
	FDataField Speed;
	Speed.Name    = "Speed";
	Speed.Type    = EDataFieldType::Float;
	Speed.Default = FDataValue::MakeFloat(1.5f);
	E_EXPECT_TRUE(Struct->AddField(Speed));
	FDataField Note;
	Note.Name    = "Note";
	Note.Type    = EDataFieldType::String;
	Note.Default = FDataValue::MakeString("");
	E_EXPECT_TRUE(Struct->AddField(Note));
	// 필드 규칙
	FDataField Invalid = Note;
	E_EXPECT_FALSE(Struct->AddField(Invalid, -1, &Error)); // 중복
	Invalid.Name = "Name";
	E_EXPECT_FALSE(Struct->AddField(Invalid)); // 예약어
	Invalid.Name = "Bad Name";
	E_EXPECT_FALSE(Struct->AddField(Invalid));
	FDataField EmptyEnum;
	EmptyEnum.Name    = "Kind";
	EmptyEnum.Type    = EDataFieldType::Enum;
	EmptyEnum.Default = FDataValue::MakeString("");
	E_EXPECT_FALSE(Struct->AddField(EmptyEnum)); // 값 목록 없음
	E_EXPECT_EQ(Struct->MakeUniqueFieldName("Speed"), std::string("Speed_2"));

	FDataTable Table;
	Table.Struct = Struct;
	Table.AddRow("A");
	Table.AddRow("B");
	Table.SetValue("A", "Damage", FDataValue::MakeInt(7));
	Table.SetValue("A", "Speed", FDataValue::MakeFloat(2.0f));
	Table.SetValue("A", "Note", FDataValue::MakeString("12"));
	Table.SetValue("B", "Note", FDataValue::MakeString("not a number"));

	// 새 구조체: Damage → Power(Float로 타입 변경), Speed 삭제, Note → Int로 변경, 새 필드 Weight 추가, 순서 변경
	auto NewStruct = std::make_shared<FDataStruct>(*Struct);
	E_EXPECT_TRUE(NewStruct->RenameField("Damage", "Power"));
	E_EXPECT_FALSE(NewStruct->RenameField("Power", "Note")); // 중복 이름
	FDataField PowerFloat = *NewStruct->GetField("Power");
	PowerFloat.Type       = EDataFieldType::Float; // 기본값(Int)이 안 맞으면 타입 기본값
	E_EXPECT_TRUE(NewStruct->SetField(NewStruct->FindField("Power"), PowerFloat));
	E_EXPECT_NEAR(NewStruct->GetField("Power")->Default.AsFloat(), 0.0f, 1.0e-6f);
	E_EXPECT_TRUE(NewStruct->RemoveField("Speed"));
	FDataField NoteInt = *NewStruct->GetField("Note");
	NoteInt.Type       = EDataFieldType::Int;
	NoteInt.Default    = FDataValue::MakeInt(-1);
	E_EXPECT_TRUE(NewStruct->SetField(NewStruct->FindField("Note"), NoteInt));
	FDataField Weight;
	Weight.Name    = "Weight";
	Weight.Type    = EDataFieldType::Float;
	Weight.Default = FDataValue::MakeFloat(3.0f);
	E_EXPECT_TRUE(NewStruct->AddField(Weight, 0));
	E_EXPECT_TRUE(NewStruct->MoveField(0, 2));
	E_EXPECT_EQ(NewStruct->Fields[2].Name, std::string("Weight"));

	Table.Rebind(NewStruct, { { "Damage", "Power" } });
	E_EXPECT_TRUE(Table.Struct == NewStruct);
	E_EXPECT_NEAR(Table.GetFloat("A", "Power"), 7.0f, 1.0e-6f);  // 이름 변경 + Int → Float
	E_EXPECT_NEAR(Table.GetFloat("B", "Power"), 5.0f, 1.0e-6f);  // 옛 기본값이던 값도 옮겨진다
	E_EXPECT_EQ(Table.GetInt("A", "Note"), 12);                  // 문자열 "12" → Int (글자 경유)
	E_EXPECT_EQ(Table.GetInt("B", "Note"), -1);                  // 변환 실패 → 새 기본값
	E_EXPECT_NEAR(Table.GetFloat("A", "Weight"), 3.0f, 1.0e-6f); // 새 필드 = 기본값
	E_EXPECT_TRUE(Table.FindValue("A", "Speed") == nullptr);     // 삭제된 필드
	E_EXPECT_EQ(Table.FindRow("A")->Record.Values.size(), NewStruct->Fields.size());

	// 연쇄 이름 변경 (A→B, B→C) + 파일에 옛 이름으로 남은 값(Unknown)도 되살린다
	FDataTable Old;
	E_EXPECT_TRUE(FDataTable::FromJsonString(R"({ "Struct": "S", "Rows": [ { "Name": "R", "Values": { "Damage": 9, "Speed": 4 } } ] })",
	                                         [&](const std::string&) { return NewStruct; }, Old));
	E_EXPECT_EQ(Old.FindRow("R")->Record.Unknown.size(), static_cast<size_t>(2));
	Old.Rebind(NewStruct, { { "Damage", "Tmp" }, { "Tmp", "Power" } });
	E_EXPECT_NEAR(Old.GetFloat("R", "Power"), 9.0f, 1.0e-6f);
	E_EXPECT_EQ(Old.FindRow("R")->Record.Unknown.size(), static_cast<size_t>(1)); // Speed는 여전히 모르는 필드 (보존)

	// 변환 규칙 (숫자/벡터/배열)
	FDataField Vec3;
	Vec3.Type = EDataFieldType::Vector3;
	FDataField Color;
	Color.Type = EDataFieldType::Color;
	E_EXPECT_TRUE(DataValueText::Convert(Vec3, Color, FDataValue::MakeVector3(FVector3(0.5f, 0.25f, 1.0f))).AsVector4() == FVector4(0.5f, 0.25f, 1.0f, 1.0f));
	FDataField IntArray;
	IntArray.Type        = EDataFieldType::Array;
	IntArray.ElementType = EDataFieldType::Int;
	FDataField FloatField;
	FloatField.Type = EDataFieldType::Float;
	const FDataValue Wrapped = DataValueText::Convert(FloatField, IntArray, FDataValue::MakeFloat(2.6f));
	E_EXPECT_TRUE(Wrapped.AsArray().size() == 1 && Wrapped.AsArray()[0].AsInt() == 3);
	E_EXPECT_NEAR(DataValueText::Convert(IntArray, FloatField, Wrapped).AsFloat(), 3.0f, 1.0e-6f);
}

E_TEST(DataTable_CsvRoundTripAndWarnings)
{
	const auto Struct = ParseStruct(WeaponStructJson);
	FDataTable Table;
	E_EXPECT_TRUE(FDataTable::FromJsonString(R"({ "Struct": "Data/Weapon.estruct", "Rows": [
		{ "Name": "Sword, Long", "Values": { "Damage": 0.1, "Lore": "가,\n\"나\"", "Offset": [1, -2.5, 3], "Tags": ["x|y", "a\\b", ""],
		                                      "Rarity": "Epic", "Tint": [0.5, 0.25, 1, 0.75], "Combo": ["Axe", "Sword, Long"], "Equippable": false } },
		{ "Name": "Axe", "Values": { "Tags": [], "Icon": "Icons/Axe.png", "DisplayName": " 공백 " } } ] })",
	                                         ResolverFor(Struct), Table));
	const std::string Csv = DataCsv::Export(Table);
	E_EXPECT_TRUE(Csv.rfind("\xEF\xBB\xBF" "Name,Equippable,Level,Damage", 0) == 0); // BOM + 머리글
	E_EXPECT_TRUE(Csv.find("\"Sword, Long\"") != std::string::npos);
	E_EXPECT_TRUE(Csv.find("x\\|y|a\\\\b|") != std::string::npos); // 배열 이스케이프

	FDataTable      Imported = Table;
	FDataLoadReport Report;
	E_EXPECT_TRUE(DataCsv::ImportInto(Csv, Imported, &Report));
	E_EXPECT_EQ(Report.Warnings.size(), static_cast<size_t>(0));
	E_EXPECT_EQ(Imported.GetRowCount(), Table.GetRowCount());
	for (const FDataRow& Row : Table.GetRows())
	{
		const FDataRow* Other = Imported.FindRow(Row.Name);
		E_EXPECT_TRUE(Other != nullptr && Other->Record.Values == Row.Record.Values);
	}

	// 손으로 쓴 CSV: 열 순서 다름, 대소문자 다른 머리글, 모르는 열, 빠진 열, 잘못된 칸, 중복 행, 다른 성분 표기, 빈 줄
	const char* Hand = "Name,damage,Rarity,Unknown,Tint,Offset,Tags,Level\r\n"
	                   "A,5,Common,?,#FF0000,(1 2 3),p|q,abc\r\n"
	                   "\r\n"
	                   "A,6,Nope,,,,,\r\n";
	std::vector<FDataRow> Rows;
	FDataLoadReport       HandReport;
	E_EXPECT_TRUE(DataCsv::Import(Hand, Table, Rows, &HandReport));
	E_EXPECT_EQ(Rows.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Rows[1].Name, std::string("A_2"));
	const int32 Damage = Struct->FindField("Damage");
	const int32 Tint   = Struct->FindField("Tint");
	const int32 Offset = Struct->FindField("Offset");
	const int32 Tags   = Struct->FindField("Tags");
	const int32 Level  = Struct->FindField("Level");
	const int32 Rarity = Struct->FindField("Rarity");
	E_EXPECT_NEAR(Rows[0].Record.Values[static_cast<size_t>(Damage)].AsFloat(), 5.0f, 1.0e-6f);
	E_EXPECT_TRUE(Rows[0].Record.Values[static_cast<size_t>(Tint)].AsVector4() == FVector4(1.0f, 0.0f, 0.0f, 1.0f));
	E_EXPECT_TRUE(Rows[0].Record.Values[static_cast<size_t>(Offset)].AsVector3() == FVector3(1.0f, 2.0f, 3.0f));
	E_EXPECT_EQ(Rows[0].Record.Values[static_cast<size_t>(Tags)].AsArray().size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Rows[0].Record.Values[static_cast<size_t>(Level)].AsInt(), 1); // "abc" → 기본값
	E_EXPECT_EQ(Rows[1].Record.Values[static_cast<size_t>(Rarity)].AsString(), std::string("Rare")); // 목록 밖 → 기본값
	E_EXPECT_TRUE(Rows[1].Record.Values[static_cast<size_t>(Tint)] == Struct->GetField("Tint")->Default); // 빈 칸 → 기본값
	E_EXPECT_TRUE(Rows[1].Record.Values[static_cast<size_t>(Tags)].AsArray().empty());                  // 빈 칸 배열 → 빈 배열
	E_EXPECT_TRUE(HasWarning(HandReport.Warnings, "Unknown"));
	E_EXPECT_TRUE(HasWarning(HandReport.Warnings, "Equippable")); // 빠진 열
	E_EXPECT_TRUE(HasWarning(HandReport.Warnings, "abc"));
	E_EXPECT_TRUE(HasWarning(HandReport.Warnings, "Nope"));
	E_EXPECT_TRUE(HasWarning(HandReport.Warnings, "A_2"));

	// TSV 자동 감지 + 실패 경우
	const auto Tsv = DataCsv::Parse("Name\tLevel\nX\t4\n");
	E_EXPECT_TRUE(Tsv.size() == 2 && Tsv[1].size() == 2 && Tsv[1][1] == "4");
	std::string Error;
	E_EXPECT_FALSE(DataCsv::Import("", Table, Rows, nullptr, &Error));
	FDataTable NoStruct;
	E_EXPECT_FALSE(DataCsv::Import("Name\nA\n", NoStruct, Rows, nullptr, &Error));
}

E_TEST(DataLibrary_LoadCacheInvalidateAndReferenceWarnings)
{
	FTempDataContent Temp("Library");
	WriteText(Temp.Content / "Data/Weapon.estruct", WeaponStructJson);
	WriteText(Temp.Content / "Icons/Sword.png", "png");
	WriteText(Temp.Content / "Data/Weapons.etable", R"({ "Version": 1, "Struct": "Data/Weapon.estruct", "Rows": [
		{ "Name": "Sword", "Values": { "Upgrade": "Axe", "Icon": "Icons/Sword.png", "Combo": ["Sword", "Ghost"] } },
		{ "Name": "Axe", "Values": { "Upgrade": "Missing", "Icon": "Icons/Axe.png" } },
		{ "Name": "Bow", "Values": { "Icon": "Icons/Bow.glb" } } ] })");
	WriteText(Temp.Content / "Data/Balance.edata", R"({ "Version": 1, "Struct": "Data/Weapon.estruct", "Values": { "Level": 4, "Upgrade": "Bow" } })");
	WriteText(Temp.Content / "Data/Garbage.etable", "\x01\x02 not json");

	FDataLibrary&    Library = FDataLibrary::Get();
	const uint32     Start   = Library.GetGeneration();
	const auto       Table   = Library.LoadTable("Data/Weapons.etable");
	E_EXPECT_TRUE(Table != nullptr && Table->Struct != nullptr);
	E_EXPECT_EQ(Table->GetRowCount(), 3);
	// 같은 경로(대소문자/구분자 달라도)는 캐시
	E_EXPECT_TRUE(Library.LoadTable("data\\WEAPONS.etable") == Table);
	E_EXPECT_TRUE(Library.LoadTable(FStringConv::ToUtf8((Temp.Content / "Data/Weapons.etable").wstring())) == Table);
	E_EXPECT_TRUE(Library.LoadStruct("Data/Weapon.estruct") == Table->Struct); // 구조체도 공유

	// 참조 경고: 없는 행(자기 참조 포함), 없는 에셋, 필터 밖 확장자. 정상 참조는 경고 없음
	const std::vector<std::string> Warnings = Library.ValidateReferences(*Table, "Data/Weapons.etable");
	E_EXPECT_TRUE(HasWarning(Warnings, "'Missing'"));
	E_EXPECT_TRUE(HasWarning(Warnings, "'Ghost'"));
	E_EXPECT_TRUE(HasWarning(Warnings, "Icons/Axe.png"));
	E_EXPECT_TRUE(HasWarning(Warnings, "Icons/Bow.glb"));
	E_EXPECT_FALSE(HasWarning(Warnings, "없는 행 'Axe'"));
	E_EXPECT_FALSE(HasWarning(Warnings, "Icons/Sword.png"));
	E_EXPECT_EQ(Warnings.size(), static_cast<size_t>(4));

	const auto Asset = Library.LoadDataAsset("Data/Balance.edata");
	E_EXPECT_TRUE(Asset != nullptr && Asset->GetInt("Level") == 4);
	E_EXPECT_EQ(Library.ValidateReferences(*Asset).size(), static_cast<size_t>(0)); // Bow는 있는 행

	// 없는 파일/깨진 파일 → nullptr (실패도 캐시, 크래시 없음)
	E_EXPECT_TRUE(Library.LoadTable("Data/None.etable") == nullptr);
	E_EXPECT_TRUE(Library.LoadTable("Data/Garbage.etable") == nullptr);
	E_EXPECT_TRUE(Library.LoadDataAsset("Data/None.edata") == nullptr);

	// 파일을 바꾸고 Invalidate(경로) → 세대 증가 + 새 객체 (받아 둔 옛 객체는 그대로)
	WriteText(Temp.Content / "Data/Weapons.etable", R"({ "Struct": "Data/Weapon.estruct", "Rows": [ { "Name": "Spear", "Values": {} } ] })");
	E_EXPECT_TRUE(Library.LoadTable("Data/Weapons.etable") == Table); // 아직 캐시
	Library.Invalidate("Data/Weapons.etable");
	E_EXPECT_TRUE(Library.GetGeneration() > Start);
	const auto Reloaded = Library.LoadTable("Data/Weapons.etable");
	E_EXPECT_TRUE(Reloaded != Table && Reloaded->HasRow("Spear"));
	E_EXPECT_TRUE(Table->HasRow("Sword"));
	E_EXPECT_TRUE(Library.LoadDataAsset("Data/Balance.edata") == Asset); // 다른 파일은 유지

	// 구조체 Invalidate → 그 구조체를 쓰는 테이블/에셋도 다시 읽는다
	Library.Invalidate("Data/Weapon.estruct");
	E_EXPECT_TRUE(Library.LoadTable("Data/Weapons.etable") != Reloaded);
	E_EXPECT_TRUE(Library.LoadDataAsset("Data/Balance.edata") != Asset);
	E_EXPECT_TRUE(FDataLibrary::IsDataExtension(L".etable") && FDataLibrary::IsDataExtension(L".estruct") && !FDataLibrary::IsDataExtension(L".json"));
}

E_TEST(DataLibrary_SaveStructAndMigrateUsers)
{
	FTempDataContent Temp("Migrate");
	WriteText(Temp.Content / "Data/Stats.estruct", R"({ "Version": 1, "Name": "Stats", "Fields": [
		{ "Name": "Hp", "Type": "Int", "Default": 10 }, { "Name": "Atk", "Type": "Int", "Default": 1 } ] })");
	WriteText(Temp.Content / "Data/Enemies.etable", R"({ "Struct": "Data/Stats.estruct", "Rows": [ { "Name": "Slime", "Values": { "Hp": 5, "Atk": 2 } } ] })");
	WriteText(Temp.Content / "Data/Sub/Bosses.etable", R"({ "Struct": "Data/Stats.estruct", "Rows": [ { "Name": "King", "Values": { "Hp": 500 } } ] })");
	WriteText(Temp.Content / "Data/Player.edata", R"({ "Struct": "Data/Stats.estruct", "Values": { "Hp": 100 } })");
	WriteText(Temp.Content / "Data/Other.etable", R"({ "Struct": "Data/OtherStruct.estruct", "Rows": [] })");

	FDataLibrary& Library = FDataLibrary::Get();
	E_EXPECT_EQ(FDataLibrary::FindStructUsers(Temp.Content, "Data/Stats.estruct").size(), static_cast<size_t>(3));
	const auto Before = Library.LoadTable("Data/Enemies.etable");

	// Hp → Health(Float), Atk 삭제, Def 추가
	FDataStruct NewStruct = *Library.LoadStruct("Data/Stats.estruct");
	E_EXPECT_TRUE(NewStruct.RenameField("Hp", "Health"));
	FDataField Health = *NewStruct.GetField("Health");
	Health.Type       = EDataFieldType::Float;
	Health.Default    = FDataValue::MakeFloat(10.0f);
	E_EXPECT_TRUE(NewStruct.SetField(0, Health));
	E_EXPECT_TRUE(NewStruct.RemoveField("Atk"));
	FDataField Def;
	Def.Name    = "Def";
	Def.Type    = EDataFieldType::Int;
	Def.Default = FDataValue::MakeInt(3);
	E_EXPECT_TRUE(NewStruct.AddField(Def));

	std::vector<fs::path> Changed;
	std::string           Error;
	E_EXPECT_TRUE(Library.SaveStructAndMigrate("Data/Stats.estruct", NewStruct, { { "Hp", "Health" } }, Temp.Content, &Changed, &Error));
	E_EXPECT_EQ(Changed.size(), static_cast<size_t>(4)); // 구조체 + 사용 파일 3개
	E_EXPECT_TRUE(ReadText(Temp.Content / "Data/Other.etable").find("\"Rows\": []") != std::string::npos); // 다른 구조체 파일은 그대로

	const auto Enemies = Library.LoadTable("Data/Enemies.etable");
	E_EXPECT_TRUE(Enemies != Before); // 전체 Invalidate
	E_EXPECT_NEAR(Enemies->GetFloat("Slime", "Health"), 5.0f, 1.0e-6f);
	E_EXPECT_EQ(Enemies->GetInt("Slime", "Def"), 3);
	E_EXPECT_TRUE(Enemies->FindValue("Slime", "Atk") == nullptr);
	E_EXPECT_TRUE(Enemies->FindRow("Slime")->Record.Unknown.empty()); // 삭제한 필드는 남기지 않음
	E_EXPECT_NEAR(Library.LoadTable("Data/Sub/Bosses.etable")->GetFloat("King", "Health"), 500.0f, 1.0e-6f);
	E_EXPECT_NEAR(Library.LoadDataAsset("Data/Player.edata")->GetFloat("Health"), 100.0f, 1.0e-6f);
	E_EXPECT_TRUE(ReadText(Temp.Content / "Data/Enemies.etable").find("\"Health\":5") != std::string::npos);

	// 저장 도우미: 쓰고 그 경로만 Invalidate
	FDataTable Copy = *Enemies;
	E_EXPECT_TRUE(Copy.AddRow("Bat") != nullptr);
	E_EXPECT_TRUE(Library.SaveTable("Data/Enemies.etable", Copy));
	E_EXPECT_TRUE(Library.LoadTable("Data/Enemies.etable")->HasRow("Bat"));
}
