#include "Editor/AssetEditors/DataTableView.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace
{
	char LowerAscii(char Char) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Char))); }

	int32 CompareTextInsensitive(std::string_view A, std::string_view B)
	{
		const size_t Count = std::min(A.size(), B.size());
		for (size_t Index = 0; Index < Count; ++Index)
		{
			const char LeftChar  = LowerAscii(A[Index]);
			const char RightChar = LowerAscii(B[Index]);
			if (LeftChar != RightChar)
			{
				return static_cast<unsigned char>(LeftChar) < static_cast<unsigned char>(RightChar) ? -1 : 1;
			}
		}
		if (A.size() != B.size())
		{
			return A.size() < B.size() ? -1 : 1;
		}
		return A == B ? 0 : (A < B ? -1 : 1); // 대소문자만 다르면 그대로 비교 (결정적)
	}

	template <typename T>
	int32 CompareNumber(T A, T B)
	{
		return A < B ? -1 : (B < A ? 1 : 0);
	}

	int32 CompareComponents(const float* A, const float* B, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (const int32 Result = CompareNumber(A[Index], B[Index]); Result != 0)
			{
				return Result;
			}
		}
		return 0;
	}

	int32 EnumIndex(const FDataField& Field, const std::string& Value)
	{
		const auto It = std::find(Field.EnumValues.begin(), Field.EnumValues.end(), Value);
		return It == Field.EnumValues.end() ? static_cast<int32>(Field.EnumValues.size()) : static_cast<int32>(It - Field.EnumValues.begin());
	}

	// 정렬 순서 비교 (SortColumn 기준, 같으면 0)
	int32 CompareRows(const FDataTable& Table, const FDataRow& A, const FDataRow& B, int32 SortColumn)
	{
		if (SortColumn == DataTableView::NameColumn || Table.Struct == nullptr)
		{
			return CompareTextInsensitive(A.Name, B.Name);
		}
		const size_t FieldIndex = static_cast<size_t>(SortColumn - 1);
		if (FieldIndex >= Table.Struct->Fields.size() || FieldIndex >= A.Record.Values.size() || FieldIndex >= B.Record.Values.size())
		{
			return 0;
		}
		return DataTableView::CompareValues(Table.Struct->Fields[FieldIndex], A.Record.Values[FieldIndex], B.Record.Values[FieldIndex]);
	}

	std::vector<int32> SortIndices(const FDataTable& Table, std::vector<int32> Indices, int32 SortColumn, bool bDescending)
	{
		if (SortColumn < 0)
		{
			return Indices;
		}
		const std::vector<FDataRow>& Rows = Table.GetRows();
		std::stable_sort(Indices.begin(), Indices.end(), [&](int32 Left, int32 Right) {
			const int32 Result = CompareRows(Table, Rows[static_cast<size_t>(Left)], Rows[static_cast<size_t>(Right)], SortColumn);
			return bDescending ? Result > 0 : Result < 0;
		});
		return Indices;
	}
} // namespace

bool DataTableView::ContainsInsensitive(std::string_view Text, std::string_view Needle)
{
	if (Needle.empty())
	{
		return true;
	}
	const auto It = std::search(Text.begin(), Text.end(), Needle.begin(), Needle.end(), [](char A, char B) { return LowerAscii(A) == LowerAscii(B); });
	return It != Text.end();
}

bool DataTableView::RowMatches(const FDataTable& Table, const FDataRow& Row, std::string_view Filter)
{
	if (Filter.empty() || ContainsInsensitive(Row.Name, Filter))
	{
		return true;
	}
	if (Table.Struct == nullptr)
	{
		return false;
	}
	const std::vector<FDataField>& Fields = Table.Struct->Fields;
	for (size_t Index = 0; Index < Fields.size() && Index < Row.Record.Values.size(); ++Index)
	{
		if (ContainsInsensitive(DataValueText::ToText(Fields[Index], Row.Record.Values[Index]), Filter))
		{
			return true;
		}
	}
	return false;
}

int32 DataTableView::CompareValues(const FDataField& Field, const FDataValue& A, const FDataValue& B)
{
	switch (Field.Type)
	{
	case EDataFieldType::Bool: return CompareNumber(A.AsBool() ? 1 : 0, B.AsBool() ? 1 : 0);
	case EDataFieldType::Int: return CompareNumber(A.AsInt(), B.AsInt());
	case EDataFieldType::Float: return CompareNumber(A.AsFloat(), B.AsFloat());
	case EDataFieldType::Vector2:
	{
		const FVector2 Left = A.AsVector2(), Right = B.AsVector2();
		const float    LeftValues[]  = { Left.X, Left.Y };
		const float    RightValues[] = { Right.X, Right.Y };
		return CompareComponents(LeftValues, RightValues, 2);
	}
	case EDataFieldType::Vector3:
	{
		const FVector3 Left = A.AsVector3(), Right = B.AsVector3();
		const float    LeftValues[]  = { Left.X, Left.Y, Left.Z };
		const float    RightValues[] = { Right.X, Right.Y, Right.Z };
		return CompareComponents(LeftValues, RightValues, 3);
	}
	case EDataFieldType::Vector4:
	case EDataFieldType::Color:
	{
		const FVector4 Left = A.AsVector4(), Right = B.AsVector4();
		const float    LeftValues[]  = { Left.X, Left.Y, Left.Z, Left.W };
		const float    RightValues[] = { Right.X, Right.Y, Right.Z, Right.W };
		return CompareComponents(LeftValues, RightValues, 4);
	}
	case EDataFieldType::Enum: return CompareNumber(EnumIndex(Field, A.AsString()), EnumIndex(Field, B.AsString()));
	case EDataFieldType::Array:
	{
		const size_t LeftCount = A.AsArray().size(), RightCount = B.AsArray().size();
		if (LeftCount != RightCount)
		{
			return LeftCount < RightCount ? -1 : 1;
		}
		return CompareTextInsensitive(DataValueText::ToText(Field, A), DataValueText::ToText(Field, B));
	}
	default: return CompareTextInsensitive(A.AsString(), B.AsString());
	}
}

std::vector<int32> DataTableView::BuildView(const FDataTable& Table, std::string_view Filter, int32 SortColumn, bool bDescending)
{
	std::vector<int32>           Indices;
	const std::vector<FDataRow>& Rows = Table.GetRows();
	Indices.reserve(Rows.size());
	for (size_t Index = 0; Index < Rows.size(); ++Index)
	{
		if (RowMatches(Table, Rows[Index], Filter))
		{
			Indices.push_back(static_cast<int32>(Index));
		}
	}
	return SortIndices(Table, std::move(Indices), SortColumn, bDescending);
}

std::vector<FDataRow> DataTableView::SortedRows(const FDataTable& Table, int32 SortColumn, bool bDescending)
{
	const std::vector<FDataRow>& Rows = Table.GetRows();
	std::vector<int32>           Indices(Rows.size());
	for (size_t Index = 0; Index < Rows.size(); ++Index)
	{
		Indices[Index] = static_cast<int32>(Index);
	}
	Indices = SortIndices(Table, std::move(Indices), SortColumn, bDescending);
	std::vector<FDataRow> Result;
	Result.reserve(Rows.size());
	for (const int32 Index : Indices)
	{
		Result.push_back(Rows[static_cast<size_t>(Index)]);
	}
	return Result;
}

std::vector<FDataRow> DataTableView::MergeRows(const std::vector<FDataRow>& Existing, const std::vector<FDataRow>& Imported)
{
	std::vector<FDataRow>                  Result = Existing;
	std::unordered_map<std::string, size_t> ByName;
	ByName.reserve(Result.size() + Imported.size());
	for (size_t Index = 0; Index < Result.size(); ++Index)
	{
		ByName.emplace(Result[Index].Name, Index);
	}
	for (const FDataRow& Row : Imported)
	{
		if (const auto Found = ByName.find(Row.Name); Found != ByName.end())
		{
			Result[Found->second].Record.Values = Row.Record.Values; // Unknown 원문은 기존 행 것을 유지
			continue;
		}
		ByName.emplace(Row.Name, Result.size());
		Result.push_back(Row);
	}
	return Result;
}

std::string DataTableView::SummarizeArray(const FDataField& Field, const FDataValue& Value, size_t MaxChars)
{
	const FDataValue::FArray& Elements = Value.AsArray();
	const FDataField          Element  = Field.MakeElementField();
	std::string               Text     = "[" + std::to_string(Elements.size()) + "]";
	for (size_t Index = 0; Index < Elements.size(); ++Index)
	{
		Text += Index == 0 ? " " : ", ";
		Text += DataValueText::ToText(Element, Elements[Index]);
		if (Text.size() > MaxChars)
		{
			break;
		}
	}
	if (Text.size() > MaxChars)
	{
		size_t Cut = MaxChars;
		while (Cut > 0 && (static_cast<unsigned char>(Text[Cut]) & 0xC0) == 0x80) // UTF-8 이어지는 바이트에서 자르지 않는다
		{
			--Cut;
		}
		Text.resize(Cut);
		Text += "\xE2\x80\xA6"; // …
	}
	return Text;
}

int32 DataTableView::RenameRowReferences(FDataTable& Table, const std::vector<int32>& Fields, const std::string& From, const std::string& To)
{
	if (Table.Struct == nullptr || Fields.empty() || From == To)
	{
		return 0;
	}
	int32                 Changed = 0;
	std::vector<FDataRow> Rows    = Table.GetRows();
	for (FDataRow& Row : Rows)
	{
		for (const int32 FieldIndex : Fields)
		{
			if (FieldIndex < 0 || static_cast<size_t>(FieldIndex) >= Row.Record.Values.size())
			{
				continue;
			}
			FDataValue& Value = Row.Record.Values[static_cast<size_t>(FieldIndex)];
			if (auto* Elements = std::get_if<FDataValue::FArray>(&Value.Storage))
			{
				for (FDataValue& Element : *Elements)
				{
					if (Element.AsString() == From)
					{
						Element = FDataValue::MakeString(To);
						++Changed;
					}
				}
			}
			else if (Value.AsString() == From && std::holds_alternative<std::string>(Value.Storage))
			{
				Value = FDataValue::MakeString(To);
				++Changed;
			}
		}
	}
	if (Changed > 0)
	{
		Table.SetRows(std::move(Rows));
	}
	return Changed;
}

float DataTableView::GetDefaultColumnWidth(const FDataField& Field)
{
	switch (Field.Type)
	{
	case EDataFieldType::Bool: return 56.0f;
	case EDataFieldType::Int:
	case EDataFieldType::Float: return 84.0f;
	case EDataFieldType::Vector2: return 140.0f;
	case EDataFieldType::Vector3: return 200.0f;
	case EDataFieldType::Vector4: return 260.0f;
	case EDataFieldType::Color: return 170.0f;
	case EDataFieldType::Enum:
	case EDataFieldType::RowRef: return 130.0f;
	case EDataFieldType::Asset: return 240.0f;
	case EDataFieldType::Array: return 180.0f;
	case EDataFieldType::Text: return 180.0f;
	default: return 140.0f;
	}
}
