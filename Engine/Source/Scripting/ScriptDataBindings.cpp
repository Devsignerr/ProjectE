#include "Scripting/LuaRuntime.h"

#include "Core/StringConv.h"
#include "Scene/DataLibrary.h"

#include <algorithm>
#include <cwctype>
#include <format>
#include <stdexcept>

// Data 테이블 — 데이터 테이블(.etable)/데이터 에셋(.edata) 읽기 (Scene/DataTable.h, 경로는 Content 기준)
//   Data.GetRow(tablePath, rowName)        → 행 테이블 { Name = 행 이름, <필드> = 값, ... } 또는 nil (행 없음)
//   Data.GetRows(tablePath)                → 배열 { {Name=..., ...}, ... } (파일의 행 순서)
//   Data.GetRowNames(tablePath)            → 배열 { "Sword", ... }
//   Data.HasRow(tablePath, rowName)        → bool
//   Data.Load(dataAssetPath)               → 값 테이블 { <필드> = 값 } 또는 nil
//   Data.ResolveRef(tablePath, rowName, fieldName) → RowRef 필드가 가리키는 행 테이블(배열 RowRef면 행 테이블 배열, 빈 참조/없는 행은 nil 또는 빠짐)
//   Data.GetGeneration()                   → 정수. 데이터 파일이 다시 로드될 때(에디터 핫 리로드·Invalidate)마다 바뀐다
// 값 변환: Bool → boolean, Int → 정수, Float → 수, String/Text/Enum/Asset/RowRef → 문자열(RowRef는 행 이름), Vector2/3/4 → Vector2/3/4,
//   Color → Vector4(sRGB), Array → Lua 배열(1부터)
// 캐시 의미: 파싱된 데이터는 C++ FDataLibrary가 세대마다 한 번 읽어 공유하고, Lua 결과는 부를 때마다 새 테이블로 만든다
//   (스크립트가 결과를 고쳐도 다른 호출·다른 스크립트에 영향 없음 — Vector userdata도 값 복사). 그래서 큰 테이블을 매 프레임 GetRows 하지 말고
//   OnStart 등에서 한 번 받아 두고, 핫 리로드를 반영하려면 Data.GetGeneration()이 바뀌었을 때 다시 받는다
// 오류: 경로가 없거나 읽지 못한 파일/구조체 없는 테이블 → nil (경고 로그는 파일마다 한 번 — 라이브러리 캐시가 실패도 기억).
//   인자 타입이 틀리거나 확장자가 다른 파일(.etable 자리에 .edata 등), 없는 필드 이름(ResolveRef) → Lua 오류

namespace
{
	std::wstring LowerExtension(const std::string& Path)
	{
		std::wstring Extension = std::filesystem::path(FStringConv::ToWide(Path)).extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension;
	}

	std::string RequireString(const sol::object& Value, const char* Api, const char* Argument)
	{
		if (Value.get_type() != sol::type::string)
		{
			throw std::runtime_error(std::format("Data.{}: {}는 문자열이어야 합니다", Api, Argument));
		}
		return Value.as<std::string>();
	}

	sol::object ToLua(sol::state& Lua, const FDataField& Field, const FDataValue& Value)
	{
		switch (Field.Type)
		{
		case EDataFieldType::Bool: return sol::make_object(Lua, Value.AsBool());
		case EDataFieldType::Int: return sol::make_object(Lua, static_cast<int64>(Value.AsInt()));
		case EDataFieldType::Float: return sol::make_object(Lua, static_cast<double>(Value.AsFloat()));
		case EDataFieldType::Vector2: return sol::make_object(Lua, Value.AsVector2());
		case EDataFieldType::Vector3: return sol::make_object(Lua, Value.AsVector3());
		case EDataFieldType::Vector4:
		case EDataFieldType::Color: return sol::make_object(Lua, Value.AsVector4());
		case EDataFieldType::Array:
		{
			const FDataField          Element  = Field.MakeElementField();
			const FDataValue::FArray& Elements = Value.AsArray();
			sol::table                List     = Lua.create_table(static_cast<int>(Elements.size()), 0);
			for (size_t Index = 0; Index < Elements.size(); ++Index)
			{
				List[Index + 1] = ToLua(Lua, Element, Elements[Index]);
			}
			return List;
		}
		default: return sol::make_object(Lua, Value.AsString());
		}
	}

	void FillRecord(sol::state& Lua, sol::table& Table, const FDataStruct& Struct, const FDataRecord& Record)
	{
		for (size_t Index = 0; Index < Struct.Fields.size(); ++Index)
		{
			const FDataField& Field = Struct.Fields[Index];
			Table[Field.Name]       = ToLua(Lua, Field, Index < Record.Values.size() ? Record.Values[Index] : Field.Default);
		}
	}

	sol::table MakeRowTable(sol::state& Lua, const FDataStruct& Struct, const FDataRow& Row)
	{
		sol::table Table = Lua.create_table(0, static_cast<int>(Struct.Fields.size()) + 1);
		Table["Name"]    = Row.Name;
		FillRecord(Lua, Table, Struct, Row.Record);
		return Table;
	}
} // namespace

void FLuaRuntime::RegisterDataBindings()
{
	// Content 기준 경로 → 절대 (스크립트와 같은 Content 폴더). 확장자가 다르면 Lua 오류
	const auto Resolve = [this](const sol::object& PathValue, const char* Api, const wchar_t* Extension) -> std::string {
		const std::string Path = RequireString(PathValue, Api, "경로");
		if (LowerExtension(Path) != Extension)
		{
			throw std::runtime_error(std::format("Data.{}: {} 파일 경로가 아닙니다: {}", Api, FStringConv::ToUtf8(Extension), Path));
		}
		const std::filesystem::path Relative = FStringConv::ToWide(Path);
		if (Relative.is_absolute() || ContentDirectory.empty())
		{
			return Path;
		}
		return FStringConv::ToUtf8((ContentDirectory / Relative).lexically_normal().wstring());
	};
	// 구조체까지 해석된 테이블 (없으면 nullptr — 경고는 라이브러리가 한 번)
	const auto LoadTable = [Resolve](const sol::object& PathValue, const char* Api) -> std::shared_ptr<const FDataTable> {
		std::shared_ptr<const FDataTable> Table = FDataLibrary::Get().LoadTable(Resolve(PathValue, Api, FDataTable::Extension));
		return Table != nullptr && Table->Struct != nullptr ? Table : nullptr;
	};

	sol::table Data = Lua.create_named_table("Data");
	Data["GetRow"]  = [this, LoadTable](const sol::object& PathValue, const sol::object& RowValue) -> sol::object {
        const std::shared_ptr<const FDataTable> Table   = LoadTable(PathValue, "GetRow");
        const std::string                       RowName = RequireString(RowValue, "GetRow", "행 이름");
        const FDataRow*                         Row     = Table != nullptr ? Table->FindRow(RowName) : nullptr;
        return Row != nullptr ? sol::object(MakeRowTable(Lua, *Table->Struct, *Row)) : sol::object(sol::lua_nil);
	};
	Data["GetRows"] = [this, LoadTable](const sol::object& PathValue) -> sol::object {
		const std::shared_ptr<const FDataTable> Table = LoadTable(PathValue, "GetRows");
		if (Table == nullptr)
		{
			return sol::lua_nil;
		}
		sol::table List = Lua.create_table(Table->GetRowCount(), 0);
		for (int32 Index = 0; Index < Table->GetRowCount(); ++Index)
		{
			List[Index + 1] = MakeRowTable(Lua, *Table->Struct, Table->GetRows()[static_cast<size_t>(Index)]);
		}
		return List;
	};
	Data["GetRowNames"] = [this, LoadTable](const sol::object& PathValue) -> sol::object {
		const std::shared_ptr<const FDataTable> Table = LoadTable(PathValue, "GetRowNames");
		if (Table == nullptr)
		{
			return sol::lua_nil;
		}
		sol::table List = Lua.create_table(Table->GetRowCount(), 0);
		for (int32 Index = 0; Index < Table->GetRowCount(); ++Index)
		{
			List[Index + 1] = Table->GetRows()[static_cast<size_t>(Index)].Name;
		}
		return List;
	};
	Data["HasRow"] = [LoadTable](const sol::object& PathValue, const sol::object& RowValue) {
		const std::shared_ptr<const FDataTable> Table   = LoadTable(PathValue, "HasRow");
		const std::string                       RowName = RequireString(RowValue, "HasRow", "행 이름");
		return Table != nullptr && Table->HasRow(RowName);
	};
	Data["Load"] = [this, Resolve](const sol::object& PathValue) -> sol::object {
		const std::shared_ptr<const FDataAsset> Asset = FDataLibrary::Get().LoadDataAsset(Resolve(PathValue, "Load", FDataAsset::Extension));
		if (Asset == nullptr || Asset->Struct == nullptr)
		{
			return sol::lua_nil;
		}
		sol::table Table = Lua.create_table(0, static_cast<int>(Asset->Struct->Fields.size()));
		FillRecord(Lua, Table, *Asset->Struct, Asset->Record);
		return Table;
	};
	Data["ResolveRef"] = [this, LoadTable](const sol::object& PathValue, const sol::object& RowValue, const sol::object& FieldValue) -> sol::object {
		const std::shared_ptr<const FDataTable> Table     = LoadTable(PathValue, "ResolveRef");
		const std::string                       RowName   = RequireString(RowValue, "ResolveRef", "행 이름");
		const std::string                       FieldName = RequireString(FieldValue, "ResolveRef", "필드 이름");
		if (Table == nullptr)
		{
			return sol::lua_nil;
		}
		const FDataField* Field = Table->Struct->GetField(FieldName);
		const EDataFieldType ValueType = Field == nullptr ? EDataFieldType::Bool : Field->Type == EDataFieldType::Array ? Field->ElementType : Field->Type;
		if (Field == nullptr || ValueType != EDataFieldType::RowRef)
		{
			throw std::runtime_error(std::format("Data.ResolveRef: RowRef 필드가 아닙니다: {}", FieldName));
		}
		const FDataValue* Value = Table->FindValue(RowName, FieldName);
		if (Value == nullptr || Field->Table.empty())
		{
			return sol::lua_nil;
		}
		// 대상 테이블 경로는 Content 기준 (Lua 경로와 같은 규칙으로 해석)
		const std::shared_ptr<const FDataTable> Target = LoadTable(sol::make_object(Lua, Field->Table), "ResolveRef");
		if (Target == nullptr)
		{
			return sol::lua_nil;
		}
		const auto Lookup = [&](const std::string& Name) -> sol::object {
			const FDataRow* Row = Name.empty() ? nullptr : Target->FindRow(Name);
			return Row != nullptr ? sol::object(MakeRowTable(Lua, *Target->Struct, *Row)) : sol::object(sol::lua_nil);
		};
		if (Field->Type != EDataFieldType::Array)
		{
			return Lookup(Value->AsString());
		}
		sol::table List  = Lua.create_table();
		int32      Count = 0;
		for (const FDataValue& Item : Value->AsArray())
		{
			sol::object Row = Lookup(Item.AsString());
			if (Row.valid() && Row.get_type() != sol::type::lua_nil)
			{
				List[++Count] = Row;
			}
		}
		return List;
	};
	Data["GetGeneration"] = [] { return static_cast<int64>(FDataLibrary::Get().GetGeneration()); };
}
