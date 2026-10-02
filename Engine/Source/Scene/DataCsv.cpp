#include "Scene/DataCsv.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <unordered_set>

namespace
{
	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		return A.size() == B.size() &&
		       std::equal(A.begin(), A.end(), B.begin(), [](char X, char Y) { return std::tolower(static_cast<unsigned char>(X)) == std::tolower(static_cast<unsigned char>(Y)); });
	}

	std::string_view StripBom(std::string_view Text)
	{
		if (Text.size() >= 3 && static_cast<unsigned char>(Text[0]) == 0xEF && static_cast<unsigned char>(Text[1]) == 0xBB && static_cast<unsigned char>(Text[2]) == 0xBF)
		{
			Text.remove_prefix(3);
		}
		return Text;
	}

	bool IsBlankLine(const std::vector<std::string>& Line)
	{
		return std::all_of(Line.begin(), Line.end(), [](const std::string& Cell) { return Cell.empty(); });
	}
} // namespace

namespace DataCsv
{
	std::vector<std::vector<std::string>> Parse(std::string_view Text, char Delimiter)
	{
		Text = StripBom(Text);
		if (Delimiter == 0)
		{
			const std::string_view Header = Text.substr(0, Text.find('\n'));
			Delimiter                     = Header.find(',') == std::string_view::npos && Header.find('\t') != std::string_view::npos ? '\t' : ',';
		}
		std::vector<std::vector<std::string>> Lines;
		std::vector<std::string>              Line;
		std::string                           Cell;
		bool                                  bQuoted   = false;
		bool                                  bLineOpen = false; // 이 줄에 무엇이든 읽었는가
		for (size_t Index = 0; Index < Text.size(); ++Index)
		{
			const char C = Text[Index];
			if (bQuoted)
			{
				if (C == '"')
				{
					if (Index + 1 < Text.size() && Text[Index + 1] == '"')
					{
						Cell.push_back('"');
						++Index;
					}
					else
					{
						bQuoted = false;
					}
				}
				else
				{
					Cell.push_back(C);
				}
				continue;
			}
			if (C == '"' && Cell.empty())
			{
				bQuoted   = true;
				bLineOpen = true;
			}
			else if (C == Delimiter)
			{
				Line.push_back(std::move(Cell));
				Cell.clear();
				bLineOpen = true;
			}
			else if (C == '\r' || C == '\n')
			{
				if (C == '\r' && Index + 1 < Text.size() && Text[Index + 1] == '\n')
				{
					++Index;
				}
				Line.push_back(std::move(Cell));
				Cell.clear();
				Lines.push_back(std::move(Line));
				Line.clear();
				bLineOpen = false;
			}
			else
			{
				Cell.push_back(C);
				bLineOpen = true;
			}
		}
		if (bLineOpen || !Cell.empty())
		{
			Line.push_back(std::move(Cell));
			Lines.push_back(std::move(Line));
		}
		return Lines;
	}

	std::string Write(const std::vector<std::vector<std::string>>& Cells, char Delimiter, bool bWriteBom)
	{
		std::string Text = bWriteBom ? "\xEF\xBB\xBF" : "";
		for (const std::vector<std::string>& Line : Cells)
		{
			for (size_t Index = 0; Index < Line.size(); ++Index)
			{
				const std::string& Cell     = Line[Index];
				const bool         bNeedsQuote = Cell.find_first_of(std::string{ Delimiter, '"', '\r', '\n' }) != std::string::npos ||
				                         (!Cell.empty() && (std::isspace(static_cast<unsigned char>(Cell.front())) || std::isspace(static_cast<unsigned char>(Cell.back()))));
				if (Index > 0)
				{
					Text.push_back(Delimiter);
				}
				if (!bNeedsQuote)
				{
					Text += Cell;
					continue;
				}
				Text.push_back('"');
				for (char C : Cell)
				{
					if (C == '"')
					{
						Text.push_back('"');
					}
					Text.push_back(C);
				}
				Text.push_back('"');
			}
			Text += "\r\n";
		}
		return Text;
	}

	std::string Export(const FDataTable& Table, bool bWriteBom)
	{
		std::vector<std::vector<std::string>> Cells;
		Cells.reserve(static_cast<size_t>(Table.GetRowCount()) + 1);
		std::vector<std::string> Header{ "Name" };
		if (Table.Struct != nullptr)
		{
			for (const FDataField& Field : Table.Struct->Fields)
			{
				Header.push_back(Field.Name);
			}
		}
		Cells.push_back(std::move(Header));
		for (const FDataRow& Row : Table.GetRows())
		{
			std::vector<std::string> Line{ Row.Name };
			if (Table.Struct != nullptr)
			{
				for (size_t Index = 0; Index < Table.Struct->Fields.size(); ++Index)
				{
					const FDataField& Field = Table.Struct->Fields[Index];
					Line.push_back(DataValueText::ToText(Field, Index < Row.Record.Values.size() ? Row.Record.Values[Index] : Field.Default));
				}
			}
			Cells.push_back(std::move(Line));
		}
		return Write(Cells, ',', bWriteBom);
	}

	bool Import(std::string_view Csv, const FDataTable& Table, std::vector<FDataRow>& OutRows, FDataLoadReport* Report, std::string* OutError)
	{
		const auto Fail = [&](std::string Message) {
			if (OutError != nullptr)
			{
				*OutError = std::move(Message);
			}
			return false;
		};
		const auto Warn = [&](std::string Message) {
			if (Report != nullptr)
			{
				Report->Warn(std::move(Message));
			}
		};
		if (Table.Struct == nullptr)
		{
			return Fail("구조체가 없는 테이블에는 가져올 수 없습니다");
		}
		const FDataStruct&                          Struct = *Table.Struct;
		const std::vector<std::vector<std::string>> Lines  = Parse(Csv);
		if (Lines.empty() || Lines.front().empty())
		{
			return Fail("CSV 머리글이 없습니다");
		}

		// 열 → 필드 번호 (-1 = 무시)
		const std::vector<std::string>& Header = Lines.front();
		std::vector<int32>              Columns(Header.size(), -1);
		std::vector<bool>               bFieldSeen(Struct.Fields.size(), false);
		for (size_t Column = 1; Column < Header.size(); ++Column)
		{
			int32 Field = Struct.FindField(Header[Column]);
			if (Field < 0)
			{
				for (size_t Index = 0; Index < Struct.Fields.size(); ++Index)
				{
					if (EqualsIgnoreCase(Struct.Fields[Index].Name, Header[Column]))
					{
						Field = static_cast<int32>(Index);
						break;
					}
				}
			}
			if (Field < 0 || bFieldSeen[static_cast<size_t>(Field)])
			{
				Warn(std::format("{}열 '{}': {} (무시)", Column + 1, Header[Column], Field < 0 ? "구조체에 없는 필드" : "중복 열"));
				continue;
			}
			Columns[Column]                       = Field;
			bFieldSeen[static_cast<size_t>(Field)] = true;
		}
		for (size_t Index = 0; Index < Struct.Fields.size(); ++Index)
		{
			if (!bFieldSeen[Index])
			{
				Warn(std::format("필드 '{}' 열이 없습니다 (기본값)", Struct.Fields[Index].Name));
			}
		}

		std::vector<FDataRow>           Rows;
		std::unordered_set<std::string> Names;
		Rows.reserve(Lines.size() - 1);
		for (size_t LineIndex = 1; LineIndex < Lines.size(); ++LineIndex)
		{
			const std::vector<std::string>& Line = Lines[LineIndex];
			if (IsBlankLine(Line))
			{
				continue;
			}
			FDataRow Row;
			Row.Name               = Line.front();
			const auto MakeUnique = [&](const std::string& Base) {
				std::string Root = FDataTable::IsValidRowName(Base) ? Base : std::string("Row");
				std::string Name = Root;
				for (int32 Suffix = 2; Names.contains(Name); ++Suffix)
				{
					Name = std::format("{}_{}", Root, Suffix);
				}
				return Name;
			};
			if (!FDataTable::IsValidRowName(Row.Name) || Names.contains(Row.Name))
			{
				const std::string Unique = MakeUnique(Row.Name);
				Warn(std::format("{}줄: 행 이름 '{}'이(가) 비었거나 중복/올바르지 않아 '{}'(으)로 읽음", LineIndex + 1, Row.Name, Unique));
				Row.Name = Unique;
			}
			Names.insert(Row.Name);
			Row.Record.Values.reserve(Struct.Fields.size());
			for (const FDataField& Field : Struct.Fields)
			{
				Row.Record.Values.push_back(Field.Default);
			}
			for (size_t Column = 1; Column < Line.size() && Column < Columns.size(); ++Column)
			{
				const int32 FieldIndex = Columns[Column];
				if (FieldIndex < 0)
				{
					continue;
				}
				const FDataField& Field = Struct.Fields[static_cast<size_t>(FieldIndex)];
				FDataValue        Value;
				std::string       Error;
				if (DataValueText::FromText(Field, Line[Column], Value, &Error))
				{
					Row.Record.Values[static_cast<size_t>(FieldIndex)] = std::move(Value);
				}
				else
				{
					Warn(std::format("{}줄 행 '{}' 필드 '{}': {} (기본값)", LineIndex + 1, Row.Name, Field.Name, Error));
				}
			}
			Rows.push_back(std::move(Row));
		}
		OutRows = std::move(Rows);
		return true;
	}

	bool ImportInto(std::string_view Csv, FDataTable& Table, FDataLoadReport* Report, std::string* OutError)
	{
		std::vector<FDataRow> Rows;
		if (!Import(Csv, Table, Rows, Report, OutError))
		{
			return false;
		}
		Table.SetRows(std::move(Rows));
		return true;
	}
} // namespace DataCsv
