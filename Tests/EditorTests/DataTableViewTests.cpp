#include "Core/Testing/TestFramework.h"
#include "Editor/AssetEditors/DataTableView.h"

#include <memory>
#include <string>
#include <vector>

namespace
{
	FDataField MakeField(const char* Name, EDataFieldType Type)
	{
		FDataField Field;
		Field.Name = Name;
		Field.Type = Type;
		if (Type == EDataFieldType::Enum)
		{
			Field.EnumValues = { "Common", "Rare", "Epic" };
		}
		if (Type == EDataFieldType::Array)
		{
			Field.ElementType = EDataFieldType::RowRef;
			Field.Table       = "Data/Self.etable";
		}
		if (Type == EDataFieldType::RowRef)
		{
			Field.Table = "Data/Self.etable";
		}
		Field.Default = FDataField::MakeTypeDefault(Type, Field.EnumValues);
		return Field;
	}

	// 무기 표: Damage(Float) / Rarity(Enum) / Label(String) / Next(RowRef) / Links(RowRef 배열)
	FDataTable MakeTable()
	{
		auto Struct = std::make_shared<FDataStruct>();
		Struct->AddField(MakeField("Damage", EDataFieldType::Float));
		Struct->AddField(MakeField("Rarity", EDataFieldType::Enum));
		Struct->AddField(MakeField("Label", EDataFieldType::String));
		Struct->AddField(MakeField("Next", EDataFieldType::RowRef));
		Struct->AddField(MakeField("Links", EDataFieldType::Array));

		FDataTable Table;
		Table.StructPath = "Data/Weapon.estruct";
		Table.Rebind(Struct);
		const struct
		{
			const char* Name;
			float       Damage;
			const char* Rarity;
			const char* Label;
			const char* Next;
		} Rows[] = {
			{ "Sword", 12.0f, "Common", "blade", "Axe" },
			{ "Axe", 18.0f, "Epic", "heavy", "" },
			{ "dagger", 6.0f, "Rare", "fast BLADE", "Sword" },
			{ "Bow", 12.0f, "Rare", "ranged", "Sword" },
		};
		for (const auto& Row : Rows)
		{
			Table.AddRow(Row.Name);
			Table.SetValue(Row.Name, "Damage", FDataValue::MakeFloat(Row.Damage));
			Table.SetValue(Row.Name, "Rarity", FDataValue::MakeString(Row.Rarity));
			Table.SetValue(Row.Name, "Label", FDataValue::MakeString(Row.Label));
			Table.SetValue(Row.Name, "Next", FDataValue::MakeString(Row.Next));
		}
		Table.SetValue("Axe", "Links", FDataValue::MakeArray({ FDataValue::MakeString("Sword"), FDataValue::MakeString("Bow") }));
		return Table;
	}

	std::vector<std::string> Names(const FDataTable& Table, const std::vector<int32>& View)
	{
		std::vector<std::string> Result;
		for (const int32 Index : View)
		{
			Result.push_back(Table.GetRows()[static_cast<size_t>(Index)].Name);
		}
		return Result;
	}
} // namespace

E_TEST(DataTableView_FilterMatchesNameAndCells)
{
	const FDataTable Table = MakeTable();
	// 빈 검색 = 파일 순서 전부
	E_EXPECT_EQ(DataTableView::BuildView(Table, "", DataTableView::NoSort, false).size(), static_cast<size_t>(4));
	// 이름 (대소문자 무시)
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "DAG", DataTableView::NoSort, false)) == std::vector<std::string>{ "dagger" });
	// 칸 글자: Label "blade"/"fast BLADE"
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "blade", DataTableView::NoSort, false)) == (std::vector<std::string>{ "Sword", "dagger" }));
	// 배열 요소 글자 (Axe.Links에 Bow)
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "Bow", DataTableView::NoSort, false)) == (std::vector<std::string>{ "Axe", "Bow" }));
	E_EXPECT_TRUE(DataTableView::BuildView(Table, "zzz", DataTableView::NoSort, false).empty());
}

E_TEST(DataTableView_SortIsStableAndTyped)
{
	const FDataTable Table = MakeTable();
	const int32      DamageColumn = 1, RarityColumn = 2;
	// 수 정렬, 같은 값(Sword/Bow 12)은 파일 순서
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "", DamageColumn, false)) == (std::vector<std::string>{ "dagger", "Sword", "Bow", "Axe" }));
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "", DamageColumn, true)) == (std::vector<std::string>{ "Axe", "Sword", "Bow", "dagger" }));
	// Enum = 목록 순서 (Common < Rare < Epic), 글자 순서가 아님
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "", RarityColumn, false)) == (std::vector<std::string>{ "Sword", "dagger", "Bow", "Axe" }));
	// 이름 = 대소문자 무시
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "", DataTableView::NameColumn, false)) == (std::vector<std::string>{ "Axe", "Bow", "dagger", "Sword" }));
	// 검색 + 정렬
	E_EXPECT_TRUE(Names(Table, DataTableView::BuildView(Table, "r", DamageColumn, false)) == (std::vector<std::string>{ "dagger", "Sword", "Bow", "Axe" }));
}

E_TEST(DataTableView_SortedRowsReordersFile)
{
	FDataTable Table = MakeTable();
	Table.SetRows(DataTableView::SortedRows(Table, 1, false));
	E_EXPECT_TRUE(Table.GetRowNames() == (std::vector<std::string>{ "dagger", "Sword", "Bow", "Axe" }));
	E_EXPECT_EQ(Table.GetFloat("Axe", "Damage"), 18.0f); // 값은 행과 함께 이동, 이름 색인도 다시 만든다
	E_EXPECT_EQ(Table.FindRowIndex("Axe"), 3);
}

E_TEST(DataTableView_MergeRows)
{
	FDataTable            Table    = MakeTable();
	std::vector<FDataRow> Imported = { Table.GetRows()[1], Table.GetRows()[0] }; // Axe, Sword
	Imported[0].Record.Values[0]   = FDataValue::MakeFloat(99.0f);
	Imported[1].Name               = "Spear";
	Table.SetRows(DataTableView::MergeRows(Table.GetRows(), Imported));
	E_EXPECT_TRUE(Table.GetRowNames() == (std::vector<std::string>{ "Sword", "Axe", "dagger", "Bow", "Spear" }));
	E_EXPECT_EQ(Table.GetFloat("Axe", "Damage"), 99.0f);   // 같은 이름 = 값 교체, 위치 유지
	E_EXPECT_EQ(Table.GetFloat("Spear", "Damage"), 12.0f); // 새 이름 = 끝에 추가
}

E_TEST(DataTableView_RenameRowReferences)
{
	FDataTable Table = MakeTable();
	E_EXPECT_TRUE(Table.RenameRow("Sword", "Longsword"));
	const int32 Changed = DataTableView::RenameRowReferences(Table, { 3, 4 }, "Sword", "Longsword");
	E_EXPECT_EQ(Changed, 3); // dagger.Next, Bow.Next, Axe.Links[0]
	E_EXPECT_EQ(Table.GetString("dagger", "Next"), std::string("Longsword"));
	E_EXPECT_EQ(Table.GetString("Axe", "Next"), std::string());
	E_EXPECT_EQ(Table.FindValue("Axe", "Links")->AsArray()[0].AsString(), std::string("Longsword"));
	E_EXPECT_EQ(Table.FindValue("Axe", "Links")->AsArray()[1].AsString(), std::string("Bow"));
	// 다른 필드(Label)는 같은 글자여도 바꾸지 않는다
	E_EXPECT_EQ(DataTableView::RenameRowReferences(Table, { 3 }, "blade", "x"), 0);
}

E_TEST(DataTableView_SummarizeArray)
{
	FDataField Field  = MakeField("Tags", EDataFieldType::Array);
	Field.ElementType = EDataFieldType::String;
	Field.Table.clear();
	const FDataValue Value = FDataValue::MakeArray({ FDataValue::MakeString("melee"), FDataValue::MakeString("blade") });
	E_EXPECT_EQ(DataTableView::SummarizeArray(Field, Value), std::string("[2] melee, blade"));
	E_EXPECT_EQ(DataTableView::SummarizeArray(Field, FDataValue::MakeArray({})), std::string("[0]"));
	// 길면 줄이고 한글(3바이트)을 자르지 않는다
	const FDataValue Long = FDataValue::MakeArray({ FDataValue::MakeString("가나다라마바사아자차카타파하") });
	const std::string Summary = DataTableView::SummarizeArray(Field, Long, 10);
	E_EXPECT_TRUE(Summary.size() <= 13 && Summary.rfind("\xE2\x80\xA6") == Summary.size() - 3);
	E_EXPECT_EQ(Summary.substr(0, Summary.size() - 3), std::string("[1] \xEA\xB0\x80\xEB\x82\x98")); // "[1] 가나"
}
