#include "Scene/DataTable.h"

#include "Scene/DataJson.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>

namespace
{
	constexpr const char* FieldTypeNames[] = { "Bool", "Int", "Float", "String", "Text", "Vector2", "Vector3", "Vector4", "Color", "Enum", "Asset", "RowRef", "Array" };
	static_assert(std::size(FieldTypeNames) == static_cast<size_t>(EDataFieldType::Array) + 1);

	const std::string        EmptyString;
	const FDataValue::FArray EmptyArray;

	int32 VectorComponentCount(EDataFieldType Type)
	{
		switch (Type)
		{
		case EDataFieldType::Vector2: return 2;
		case EDataFieldType::Vector3: return 3;
		case EDataFieldType::Vector4:
		case EDataFieldType::Color: return 4;
		default: return 0;
		}
	}

	bool IsNumericScalar(EDataFieldType Type) { return Type == EDataFieldType::Bool || Type == EDataFieldType::Int || Type == EDataFieldType::Float; }

	// 벡터 계열 값 → 성분 4개 (없는 성분 0, 색 알파는 1)
	void GetComponents(EDataFieldType Type, const FDataValue& Value, float (&Out)[4])
	{
		Out[0] = Out[1] = Out[2] = 0.0f;
		Out[3]               = Type == EDataFieldType::Color ? 1.0f : 0.0f;
		switch (Type)
		{
		case EDataFieldType::Vector2:
		{
			const FVector2 V = Value.AsVector2();
			Out[0]           = V.X;
			Out[1]           = V.Y;
			break;
		}
		case EDataFieldType::Vector3:
		{
			const FVector3 V = Value.AsVector3();
			Out[0]           = V.X;
			Out[1]           = V.Y;
			Out[2]           = V.Z;
			break;
		}
		case EDataFieldType::Vector4:
		case EDataFieldType::Color:
		{
			const FVector4 V = Value.AsVector4();
			Out[0]           = V.X;
			Out[1]           = V.Y;
			Out[2]           = V.Z;
			Out[3]           = V.W;
			break;
		}
		default: break;
		}
	}

	FDataValue MakeVectorValue(EDataFieldType Type, const float (&C)[4])
	{
		switch (Type)
		{
		case EDataFieldType::Vector2: return FDataValue::MakeVector2(FVector2(C[0], C[1]));
		case EDataFieldType::Vector3: return FDataValue::MakeVector3(FVector3(C[0], C[1], C[2]));
		default: return FDataValue::MakeVector4(FVector4(C[0], C[1], C[2], C[3]));
		}
	}

	std::string_view Trim(std::string_view Text)
	{
		while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
		{
			Text.remove_prefix(1);
		}
		while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
		{
			Text.remove_suffix(1);
		}
		return Text;
	}

	std::string ToLowerAscii(std::string_view Text)
	{
		std::string Result(Text);
		std::transform(Result.begin(), Result.end(), Result.begin(), [](char C) { return static_cast<char>(std::tolower(static_cast<unsigned char>(C))); });
		return Result;
	}

	bool ParseFloat(std::string_view Text, float& Out)
	{
		Text = Trim(Text);
		if (!Text.empty() && Text.front() == '+')
		{
			Text.remove_prefix(1);
		}
		float      Value  = 0.0f;
		const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
		if (Text.empty() || Result.ec != std::errc() || Result.ptr != Text.data() + Text.size() || !std::isfinite(Value))
		{
			return false;
		}
		Out = Value;
		return true;
	}

	bool ParseInt(std::string_view Text, int32& Out)
	{
		Text = Trim(Text);
		if (!Text.empty() && Text.front() == '+')
		{
			Text.remove_prefix(1);
		}
		int32      Value  = 0;
		const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
		if (Text.empty() || Result.ec != std::errc() || Result.ptr != Text.data() + Text.size())
		{
			return false;
		}
		Out = Value;
		return true;
	}

	bool ParseHexColor(std::string_view Text, float (&Out)[4])
	{
		Text = Trim(Text);
		if (Text.empty() || Text.front() != '#' || (Text.size() != 7 && Text.size() != 9))
		{
			return false;
		}
		Out[3] = 1.0f;
		for (size_t Index = 0; Index * 2 + 1 < Text.size(); ++Index)
		{
			uint32     Byte   = 0;
			const auto Result = std::from_chars(Text.data() + 1 + Index * 2, Text.data() + 3 + Index * 2, Byte, 16);
			if (Result.ec != std::errc() || Result.ptr != Text.data() + 3 + Index * 2)
			{
				return false;
			}
			Out[Index] = static_cast<float>(Byte) / 255.0f;
		}
		return true;
	}

	// 성분 목록 "1;2;3", "1, 2, 3", "(1 2 3)" 모두 허용
	bool ParseComponents(std::string_view Text, int32 Count, bool bColor, float (&Out)[4])
	{
		if (bColor && ParseHexColor(Text, Out))
		{
			return true;
		}
		Text = Trim(Text);
		if (Text.size() >= 2 && ((Text.front() == '(' && Text.back() == ')') || (Text.front() == '[' && Text.back() == ']')))
		{
			Text = Text.substr(1, Text.size() - 2);
		}
		std::vector<std::string_view> Parts;
		size_t                        Start = 0;
		for (size_t Index = 0; Index <= Text.size(); ++Index)
		{
			if (Index == Text.size() || Text[Index] == ';' || Text[Index] == ',' || std::isspace(static_cast<unsigned char>(Text[Index])))
			{
				if (Index > Start)
				{
					Parts.push_back(Text.substr(Start, Index - Start));
				}
				Start = Index + 1;
			}
		}
		const bool bCountOk = static_cast<int32>(Parts.size()) == Count || (bColor && Parts.size() == 3);
		if (!bCountOk)
		{
			return false;
		}
		Out[0] = Out[1] = Out[2] = 0.0f;
		Out[3]               = bColor ? 1.0f : 0.0f;
		for (size_t Index = 0; Index < Parts.size(); ++Index)
		{
			if (!ParseFloat(Parts[Index], Out[Index]))
			{
				return false;
			}
		}
		return true;
	}

	void SetError(std::string* OutError, std::string Message)
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
	}

	std::string ApplyRenames(std::string Name, const std::vector<FDataFieldRename>& Renames)
	{
		for (const FDataFieldRename& Rename : Renames)
		{
			if (Name == Rename.From)
			{
				Name = Rename.To;
			}
		}
		return Name;
	}

	std::vector<FDataValue> MakeDefaults(const FDataStruct* Struct)
	{
		std::vector<FDataValue> Values;
		if (Struct != nullptr)
		{
			Values.reserve(Struct->Fields.size());
			for (const FDataField& Field : Struct->Fields)
			{
				Values.push_back(Field.Default);
			}
		}
		return Values;
	}

	// 구조체 교체 마이그레이션 (테이블 행/데이터 에셋 공용)
	FDataRecord RebindRecord(const FDataStruct* OldStruct, const FDataStruct* NewStruct, const FDataRecord& Record, const std::vector<FDataFieldRename>& Renames)
	{
		FDataRecord Result;
		Result.Values = MakeDefaults(NewStruct);
		std::vector<bool> bAssigned(Result.Values.size(), false);

		// 같은 새 필드로 여러 값이 모이면 우선순위: ① 이름 변경 목록이 적용된 구조체 값 ② 구조체에 없던 원문(Unknown — 파일이 옛 이름으로
		// 갖고 있던 실제 데이터) ③ 이름이 그대로인 구조체 값. (구조체 파일이 먼저 바뀐 뒤 읽힌 테이블은 값이 Unknown에 있고 구조체 값은 기본값이다)
		const auto MoveKnown = [&](bool bRenamedPass) {
			if (OldStruct == nullptr)
			{
				return;
			}
			for (size_t Index = 0; Index < OldStruct->Fields.size() && Index < Record.Values.size(); ++Index)
			{
				const FDataField& OldField = OldStruct->Fields[Index];
				const std::string NewName  = ApplyRenames(OldField.Name, Renames);
				if ((NewName != OldField.Name) != bRenamedPass)
				{
					continue;
				}
				const int32 Target = NewStruct != nullptr ? NewStruct->FindField(NewName) : -1;
				if (Target < 0)
				{
					if (NewStruct == nullptr)
					{
						// 구조체가 없어지면 값을 원문으로 보존 (다시 연결되면 되살아난다)
						Result.Unknown.emplace_back(NewName, DataJson::DumpCompact(DataJson::ValueToJson(OldField, Record.Values[Index])));
					}
					continue; // 사라진 필드
				}
				if (bAssigned[static_cast<size_t>(Target)])
				{
					continue;
				}
				const FDataField& NewField = NewStruct->Fields[static_cast<size_t>(Target)];
				const bool bSameDefinition = OldField.Type == NewField.Type && (OldField.Type != EDataFieldType::Array || OldField.ElementType == NewField.ElementType);
				const FDataValue& Value    = Record.Values[Index];
				Result.Values[static_cast<size_t>(Target)] =
					bSameDefinition && NewField.Accepts(Value) ? Value : DataValueText::Convert(OldField, NewField, Value);
				bAssigned[static_cast<size_t>(Target)] = true;
			}
		};

		MoveKnown(true);
		for (const auto& [Name, Raw] : Record.Unknown)
		{
			const std::string NewName = ApplyRenames(Name, Renames);
			const int32       Target  = NewStruct != nullptr ? NewStruct->FindField(NewName) : -1;
			if (Target < 0)
			{
				Result.Unknown.emplace_back(Name, Raw);
				continue;
			}
			if (bAssigned[static_cast<size_t>(Target)])
			{
				continue;
			}
			const DataJson::FJson Parsed = DataJson::Parse(Raw);
			FDataValue            Value;
			std::string           Problem;
			if (!Parsed.is_discarded() && DataJson::ValueFromJson(NewStruct->Fields[static_cast<size_t>(Target)], Parsed, Value, Problem))
			{
				Result.Values[static_cast<size_t>(Target)] = std::move(Value);
				bAssigned[static_cast<size_t>(Target)]     = true;
			}
		}
		MoveKnown(false);
		return Result;
	}

	const FDataValue* FindRecordValue(const FDataStruct* Struct, const FDataRecord& Record, std::string_view FieldName)
	{
		if (Struct == nullptr)
		{
			return nullptr;
		}
		const int32 Index = Struct->FindField(FieldName);
		return Index >= 0 && static_cast<size_t>(Index) < Record.Values.size() ? &Record.Values[static_cast<size_t>(Index)] : nullptr;
	}

	bool SetRecordValue(const FDataStruct* Struct, FDataRecord& Record, std::string_view FieldName, const FDataValue& Value, std::string* OutError)
	{
		if (Struct == nullptr)
		{
			SetError(OutError, "구조체가 없습니다");
			return false;
		}
		const int32 Index = Struct->FindField(FieldName);
		if (Index < 0)
		{
			SetError(OutError, std::format("필드가 없습니다: {}", FieldName));
			return false;
		}
		const FDataField& Field = Struct->Fields[static_cast<size_t>(Index)];
		FDataValue        Final = Value;
		// 정수 → 실수는 편의상 허용
		if (Field.Type == EDataFieldType::Float && std::holds_alternative<int32>(Value.Storage))
		{
			Final = FDataValue::MakeFloat(static_cast<float>(Value.AsInt()));
		}
		if (!Field.Accepts(Final))
		{
			SetError(OutError, std::format("필드 '{}'({})에 맞지 않는 값입니다", Field.Name, ToString(Field.Type)));
			return false;
		}
		Record.Values.resize(Struct->Fields.size());
		Record.Values[static_cast<size_t>(Index)] = std::move(Final);
		return true;
	}

	// Values 객체 → 레코드 (구조체 순서, 없는 값 = 기본값, 모르는 필드 = 원문 보존)
	void ReadRecord(const FDataStruct* Struct, const DataJson::FJson* Values, FDataRecord& Out, const std::string& Context, FDataLoadReport* Report)
	{
		Out.Values = MakeDefaults(Struct);
		Out.Unknown.clear();
		if (Values == nullptr)
		{
			return;
		}
		if (!Values->is_object())
		{
			if (Report != nullptr)
			{
				Report->Warn(std::format("{}: Values가 객체가 아닙니다", Context));
			}
			return;
		}
		for (auto It = Values->begin(); It != Values->end(); ++It)
		{
			const int32 Index = Struct != nullptr ? Struct->FindField(It.key()) : -1;
			if (Index < 0)
			{
				if (Report != nullptr && Struct != nullptr)
				{
					Report->Warn(std::format("{}: 구조체에 없는 필드 '{}' (값은 보존)", Context, It.key()));
				}
				Out.Unknown.emplace_back(It.key(), DataJson::DumpCompact(It.value()));
				continue;
			}
			const FDataField& Field = Struct->Fields[static_cast<size_t>(Index)];
			FDataValue        Value;
			std::string       Problem;
			if (DataJson::ValueFromJson(Field, It.value(), Value, Problem))
			{
				Out.Values[static_cast<size_t>(Index)] = std::move(Value);
			}
			if (!Problem.empty() && Report != nullptr)
			{
				Report->Warn(std::format("{} 필드 '{}': {}", Context, Field.Name, Problem));
			}
		}
	}

	DataJson::FJson WriteRecord(const FDataStruct* Struct, const FDataRecord& Record)
	{
		DataJson::FJson Values = DataJson::FJson::object();
		if (Struct != nullptr)
		{
			for (size_t Index = 0; Index < Struct->Fields.size(); ++Index)
			{
				const FDataField& Field = Struct->Fields[Index];
				Values[Field.Name]      = DataJson::ValueToJson(Field, Index < Record.Values.size() ? Record.Values[Index] : Field.Default);
			}
		}
		for (const auto& [Name, Raw] : Record.Unknown)
		{
			if (Values.contains(Name))
			{
				continue;
			}
			const DataJson::FJson Parsed = DataJson::Parse(Raw);
			Values[Name]                 = Parsed.is_discarded() ? DataJson::FJson(Raw) : Parsed;
		}
		return Values;
	}

	bool WriteTextFile(const std::filesystem::path& Path, const std::string& Text, std::string* OutError)
	{
		std::error_code ErrorCode;
		if (Path.has_parent_path())
		{
			std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		}
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		if (!File)
		{
			SetError(OutError, "파일을 쓸 수 없습니다: " + Path.generic_string());
			return false;
		}
		File << Text;
		return static_cast<bool>(File);
	}

	std::shared_ptr<const FDataStruct> ResolveStruct(const DataJson::FJson& Root, const FDataStructResolver& Resolver, std::string& OutPath,
	                                                 FDataLoadReport* Report)
	{
		OutPath = Root.contains("Struct") && Root["Struct"].is_string() ? Root["Struct"].get<std::string>() : std::string();
		if (OutPath.empty())
		{
			if (Report != nullptr)
			{
				Report->Warn("Struct 경로가 없습니다");
			}
			return nullptr;
		}
		std::shared_ptr<const FDataStruct> Struct = Resolver ? Resolver(OutPath) : nullptr;
		if (Struct == nullptr && Report != nullptr)
		{
			Report->Warn("구조체를 찾을 수 없습니다: " + OutPath + " (값은 보존)");
		}
		return Struct;
	}

	void CheckVersion(const DataJson::FJson& Root, int32 Supported, FDataLoadReport* Report)
	{
		const int32 FileVersion = Root.contains("Version") && Root["Version"].is_number_integer() ? Root["Version"].get<int32>() : Supported;
		if (FileVersion > Supported && Report != nullptr)
		{
			Report->Warn(std::format("파일 버전 {}이(가) 지원 버전 {}보다 높습니다", FileVersion, Supported));
		}
	}
} // namespace

// ---------------------------------------------------------------- 타입 이름

const char* ToString(EDataFieldType Type)
{
	const size_t Index = static_cast<size_t>(Type);
	return Index < std::size(FieldTypeNames) ? FieldTypeNames[Index] : "Unknown";
}

bool TryParseDataFieldType(std::string_view Text, EDataFieldType& OutType)
{
	for (size_t Index = 0; Index < std::size(FieldTypeNames); ++Index)
	{
		if (Text == FieldTypeNames[Index])
		{
			OutType = static_cast<EDataFieldType>(Index);
			return true;
		}
	}
	return false;
}

bool IsStringDataType(EDataFieldType Type)
{
	return Type == EDataFieldType::String || Type == EDataFieldType::Text || Type == EDataFieldType::Enum || Type == EDataFieldType::Asset ||
	       Type == EDataFieldType::RowRef;
}

// ---------------------------------------------------------------- FDataValue

FDataValue FDataValue::MakeBool(bool Value) { return FDataValue{ Value }; }
FDataValue FDataValue::MakeInt(int32 Value) { return FDataValue{ Value }; }
FDataValue FDataValue::MakeFloat(float Value) { return FDataValue{ Value }; }
FDataValue FDataValue::MakeString(std::string Value) { return FDataValue{ std::move(Value) }; }
FDataValue FDataValue::MakeVector2(const FVector2& Value) { return FDataValue{ Value }; }
FDataValue FDataValue::MakeVector3(const FVector3& Value) { return FDataValue{ Value }; }
FDataValue FDataValue::MakeVector4(const FVector4& Value) { return FDataValue{ Value }; }
FDataValue FDataValue::MakeArray(FArray Elements) { return FDataValue{ std::move(Elements) }; }

bool FDataValue::AsBool() const
{
	const bool* Value = std::get_if<bool>(&Storage);
	return Value != nullptr && *Value;
}

int32 FDataValue::AsInt() const
{
	if (const int32* Value = std::get_if<int32>(&Storage))
	{
		return *Value;
	}
	return 0;
}

float FDataValue::AsFloat() const
{
	if (const float* Value = std::get_if<float>(&Storage))
	{
		return *Value;
	}
	if (const int32* Value = std::get_if<int32>(&Storage))
	{
		return static_cast<float>(*Value);
	}
	return 0.0f;
}

const std::string& FDataValue::AsString() const
{
	const std::string* Value = std::get_if<std::string>(&Storage);
	return Value != nullptr ? *Value : EmptyString;
}

FVector2 FDataValue::AsVector2() const
{
	const FVector2* Value = std::get_if<FVector2>(&Storage);
	return Value != nullptr ? *Value : FVector2(0.0f, 0.0f);
}

FVector3 FDataValue::AsVector3() const
{
	const FVector3* Value = std::get_if<FVector3>(&Storage);
	return Value != nullptr ? *Value : FVector3(0.0f, 0.0f, 0.0f);
}

FVector4 FDataValue::AsVector4() const
{
	const FVector4* Value = std::get_if<FVector4>(&Storage);
	return Value != nullptr ? *Value : FVector4(0.0f, 0.0f, 0.0f, 0.0f);
}

const FDataValue::FArray& FDataValue::AsArray() const
{
	const FArray* Value = std::get_if<FArray>(&Storage);
	return Value != nullptr ? *Value : EmptyArray;
}

bool FDataValue::operator==(const FDataValue& Other) const
{
	return Storage == Other.Storage;
}

// ---------------------------------------------------------------- FDataField

FDataValue FDataField::MakeTypeDefault(EDataFieldType Type, const std::vector<std::string>& EnumValues)
{
	switch (Type)
	{
	case EDataFieldType::Bool: return FDataValue::MakeBool(false);
	case EDataFieldType::Int: return FDataValue::MakeInt(0);
	case EDataFieldType::Float: return FDataValue::MakeFloat(0.0f);
	case EDataFieldType::Vector2: return FDataValue::MakeVector2(FVector2(0.0f, 0.0f));
	case EDataFieldType::Vector3: return FDataValue::MakeVector3(FVector3(0.0f, 0.0f, 0.0f));
	case EDataFieldType::Vector4: return FDataValue::MakeVector4(FVector4(0.0f, 0.0f, 0.0f, 0.0f));
	case EDataFieldType::Color: return FDataValue::MakeVector4(FVector4(1.0f, 1.0f, 1.0f, 1.0f));
	case EDataFieldType::Enum: return FDataValue::MakeString(EnumValues.empty() ? std::string() : EnumValues.front());
	case EDataFieldType::Array: return FDataValue::MakeArray({});
	default: return FDataValue::MakeString(std::string());
	}
}

FDataField FDataField::MakeElementField() const
{
	FDataField Element = *this;
	if (Type == EDataFieldType::Array)
	{
		Element.Type    = ElementType;
		Element.Default = MakeTypeDefault(ElementType, EnumValues);
	}
	return Element;
}

bool FDataField::Accepts(const FDataValue& Value) const
{
	switch (Type)
	{
	case EDataFieldType::Bool: return std::holds_alternative<bool>(Value.Storage);
	case EDataFieldType::Int: return std::holds_alternative<int32>(Value.Storage);
	case EDataFieldType::Float: return std::holds_alternative<float>(Value.Storage);
	case EDataFieldType::Vector2: return std::holds_alternative<FVector2>(Value.Storage);
	case EDataFieldType::Vector3: return std::holds_alternative<FVector3>(Value.Storage);
	case EDataFieldType::Vector4:
	case EDataFieldType::Color: return std::holds_alternative<FVector4>(Value.Storage);
	case EDataFieldType::Enum:
	{
		const std::string* Text = std::get_if<std::string>(&Value.Storage);
		return Text != nullptr && (std::find(EnumValues.begin(), EnumValues.end(), *Text) != EnumValues.end() || (EnumValues.empty() && Text->empty()));
	}
	case EDataFieldType::Array:
	{
		const FDataValue::FArray* Elements = std::get_if<FDataValue::FArray>(&Value.Storage);
		if (Elements == nullptr)
		{
			return false;
		}
		const FDataField Element = MakeElementField();
		return std::all_of(Elements->begin(), Elements->end(), [&](const FDataValue& Item) { return Element.Accepts(Item); });
	}
	default: return std::holds_alternative<std::string>(Value.Storage);
	}
}

std::vector<std::string> FDataField::GetFilterExtensions() const
{
	std::vector<std::string> Result;
	size_t                   Start = 0;
	for (size_t Index = 0; Index <= Filter.size(); ++Index)
	{
		if (Index == Filter.size() || Filter[Index] == ';' || Filter[Index] == ',')
		{
			std::string Item = ToLowerAscii(Trim(std::string_view(Filter).substr(Start, Index - Start)));
			if (!Item.empty())
			{
				if (Item.front() != '.')
				{
					Item.insert(Item.begin(), '.');
				}
				Result.push_back(std::move(Item));
			}
			Start = Index + 1;
		}
	}
	return Result;
}

// ---------------------------------------------------------------- FDataStruct

int32 FDataStruct::FindField(std::string_view FieldName) const
{
	for (size_t Index = 0; Index < Fields.size(); ++Index)
	{
		if (Fields[Index].Name == FieldName)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

const FDataField* FDataStruct::GetField(std::string_view FieldName) const
{
	const int32 Index = FindField(FieldName);
	return Index >= 0 ? &Fields[static_cast<size_t>(Index)] : nullptr;
}

bool FDataStruct::IsValidFieldName(std::string_view FieldName)
{
	if (FieldName.empty() || FieldName == "Name" || std::isdigit(static_cast<unsigned char>(FieldName.front())))
	{
		return false;
	}
	return std::all_of(FieldName.begin(), FieldName.end(), [](char C) { return C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9'); });
}

bool FDataStruct::ValidateField(const FDataField& Field, int32 IgnoreIndex, std::string* OutError) const
{
	if (!IsValidFieldName(Field.Name))
	{
		SetError(OutError, std::format("필드 이름이 올바르지 않습니다: '{}' (영문/숫자/_, 숫자로 시작 불가, \"Name\"은 예약어)", Field.Name));
		return false;
	}
	for (size_t Index = 0; Index < Fields.size(); ++Index)
	{
		if (static_cast<int32>(Index) != IgnoreIndex && Fields[Index].Name == Field.Name)
		{
			SetError(OutError, "이미 있는 필드 이름입니다: " + Field.Name);
			return false;
		}
	}
	const EDataFieldType ValueType = Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type;
	if (ValueType == EDataFieldType::Array)
	{
		SetError(OutError, "배열의 요소는 배열일 수 없습니다");
		return false;
	}
	if (ValueType == EDataFieldType::Enum)
	{
		if (Field.EnumValues.empty())
		{
			SetError(OutError, "Enum 값 목록이 비었습니다: " + Field.Name);
			return false;
		}
		std::vector<std::string> Sorted = Field.EnumValues;
		std::sort(Sorted.begin(), Sorted.end());
		if (std::adjacent_find(Sorted.begin(), Sorted.end()) != Sorted.end() || std::find(Sorted.begin(), Sorted.end(), std::string()) != Sorted.end())
		{
			SetError(OutError, "Enum 값은 비어 있지 않고 고유해야 합니다: " + Field.Name);
			return false;
		}
	}
	if (!Field.Accepts(Field.Default))
	{
		SetError(OutError, "기본값이 필드 타입과 맞지 않습니다: " + Field.Name);
		return false;
	}
	return true;
}

bool FDataStruct::AddField(FDataField Field, int32 Index, std::string* OutError)
{
	if (!ValidateField(Field, -1, OutError))
	{
		return false;
	}
	const int32 Count = static_cast<int32>(Fields.size());
	Index             = Index < 0 || Index > Count ? Count : Index;
	Fields.insert(Fields.begin() + Index, std::move(Field));
	return true;
}

bool FDataStruct::RemoveField(std::string_view FieldName)
{
	const int32 Index = FindField(FieldName);
	if (Index < 0)
	{
		return false;
	}
	Fields.erase(Fields.begin() + Index);
	return true;
}

bool FDataStruct::RenameField(std::string_view From, const std::string& To, std::string* OutError)
{
	const int32 Index = FindField(From);
	if (Index < 0)
	{
		SetError(OutError, std::format("필드가 없습니다: {}", From));
		return false;
	}
	FDataField Renamed = Fields[static_cast<size_t>(Index)];
	Renamed.Name       = To;
	if (!ValidateField(Renamed, Index, OutError))
	{
		return false;
	}
	Fields[static_cast<size_t>(Index)].Name = To;
	return true;
}

bool FDataStruct::MoveField(int32 From, int32 To)
{
	const int32 Count = static_cast<int32>(Fields.size());
	if (From < 0 || From >= Count || To < 0 || To >= Count)
	{
		return false;
	}
	FDataField Field = std::move(Fields[static_cast<size_t>(From)]);
	Fields.erase(Fields.begin() + From);
	Fields.insert(Fields.begin() + To, std::move(Field));
	return true;
}

bool FDataStruct::SetField(int32 Index, FDataField Field, std::string* OutError)
{
	if (Index < 0 || Index >= static_cast<int32>(Fields.size()))
	{
		SetError(OutError, "필드 번호가 범위를 벗어났습니다");
		return false;
	}
	if (!Field.Accepts(Field.Default))
	{
		Field.Default = FDataField::MakeTypeDefault(Field.Type, Field.EnumValues);
	}
	if (!ValidateField(Field, Index, OutError))
	{
		return false;
	}
	Fields[static_cast<size_t>(Index)] = std::move(Field);
	return true;
}

std::string FDataStruct::MakeUniqueFieldName(std::string_view Base) const
{
	std::string Root = IsValidFieldName(Base) ? std::string(Base) : std::string("Field");
	if (FindField(Root) < 0)
	{
		return Root;
	}
	for (int32 Suffix = 2;; ++Suffix)
	{
		std::string Candidate = std::format("{}_{}", Root, Suffix);
		if (FindField(Candidate) < 0)
		{
			return Candidate;
		}
	}
}

bool FDataStruct::FromJsonString(const std::string& Text, FDataStruct& Out, FDataLoadReport* Report, std::string* OutError)
{
	const DataJson::FJson Root = DataJson::Parse(Text);
	if (Root.is_discarded() || !Root.is_object())
	{
		SetError(OutError, "JSON 파싱 실패 (객체가 아님)");
		return false;
	}
	CheckVersion(Root, Version, Report);
	FDataStruct Result;
	Result.Name        = DataJson::GetString(Root, "Name");
	Result.Description = DataJson::GetString(Root, "Description");
	const auto FieldsIt = Root.find("Fields");
	if (FieldsIt != Root.end() && !FieldsIt->is_array() && Report != nullptr)
	{
		Report->Warn("Fields가 배열이 아닙니다");
	}
	if (FieldsIt != Root.end() && FieldsIt->is_array())
	{
		int32 Ordinal = 0;
		for (const DataJson::FJson& Item : *FieldsIt)
		{
			++Ordinal;
			const std::string Context = std::format("필드 #{}", Ordinal);
			if (!Item.is_object())
			{
				if (Report != nullptr)
				{
					Report->Warn(Context + ": 객체가 아닙니다 (무시)");
				}
				continue;
			}
			FDataField Field;
			Field.Name               = DataJson::GetString(Item, "Name");
			Field.Description        = DataJson::GetString(Item, "Description");
			Field.Filter             = DataJson::GetString(Item, "Filter");
			Field.Table              = DataJson::GetString(Item, "Table");
			const std::string TypeName = DataJson::GetString(Item, "Type");
			if (!TryParseDataFieldType(TypeName, Field.Type))
			{
				if (Report != nullptr)
				{
					Report->Warn(std::format("{} '{}': 알 수 없는 타입 '{}' (무시)", Context, Field.Name, TypeName));
				}
				continue;
			}
			if (Field.Type == EDataFieldType::Array)
			{
				const std::string ElementName = DataJson::GetString(Item, "Element");
				if (!TryParseDataFieldType(ElementName, Field.ElementType) || Field.ElementType == EDataFieldType::Array)
				{
					if (Report != nullptr)
					{
						Report->Warn(std::format("{} '{}': 배열 요소 타입이 올바르지 않습니다 '{}' (무시)", Context, Field.Name, ElementName));
					}
					continue;
				}
			}
			if (const auto ValuesIt = Item.find("Values"); ValuesIt != Item.end() && ValuesIt->is_array())
			{
				for (const DataJson::FJson& Value : *ValuesIt)
				{
					if (Value.is_string())
					{
						Field.EnumValues.push_back(Value.get<std::string>());
					}
				}
			}
			Field.Default = FDataField::MakeTypeDefault(Field.Type, Field.EnumValues);
			if (const auto DefaultIt = Item.find("Default"); DefaultIt != Item.end())
			{
				FDataValue  Value;
				std::string Problem;
				if (DataJson::ValueFromJson(Field, *DefaultIt, Value, Problem))
				{
					Field.Default = std::move(Value);
				}
				if (!Problem.empty() && Report != nullptr)
				{
					Report->Warn(std::format("필드 '{}' 기본값: {}", Field.Name, Problem));
				}
			}
			std::string Error;
			if (!Result.ValidateField(Field, -1, &Error))
			{
				// 이름 문제는 키로 쓸 수 없어 버리고, 나머지(Enum 목록 등)는 경고만 하고 유지한다
				const bool bNameProblem = !IsValidFieldName(Field.Name) || Result.FindField(Field.Name) >= 0;
				if (Report != nullptr)
				{
					Report->Warn(std::format("{}: {}{}", Context, Error, bNameProblem ? " (무시)" : ""));
				}
				if (bNameProblem)
				{
					continue;
				}
			}
			Result.Fields.push_back(std::move(Field));
		}
	}
	Out = std::move(Result);
	return true;
}

std::string FDataStruct::ToJsonString() const
{
	DataJson::FJson Root = DataJson::FJson::object();
	Root["Version"]      = Version;
	Root["Name"]         = Name;
	Root["Description"]  = Description;
	DataJson::FJson List = DataJson::FJson::array();
	for (const FDataField& Field : Fields)
	{
		DataJson::FJson Item = DataJson::FJson::object();
		Item["Name"]         = Field.Name;
		Item["Type"]         = ToString(Field.Type);
		if (Field.Type == EDataFieldType::Array)
		{
			Item["Element"] = ToString(Field.ElementType);
		}
		const EDataFieldType ValueType = Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type;
		if (ValueType == EDataFieldType::Enum || !Field.EnumValues.empty())
		{
			Item["Values"] = Field.EnumValues;
		}
		if (ValueType == EDataFieldType::Asset || !Field.Filter.empty())
		{
			Item["Filter"] = Field.Filter;
		}
		if (ValueType == EDataFieldType::RowRef || !Field.Table.empty())
		{
			Item["Table"] = Field.Table;
		}
		Item["Default"] = DataJson::ValueToJson(Field, Field.Default);
		if (!Field.Description.empty())
		{
			Item["Description"] = Field.Description;
		}
		List.push_back(std::move(Item));
	}
	Root["Fields"] = std::move(List);
	return DataJson::FormatDocument(Root);
}

// ---------------------------------------------------------------- FDataTable

void FDataTable::RebuildIndex()
{
	RowIndex.clear();
	RowIndex.reserve(Rows.size());
	for (size_t Index = 0; Index < Rows.size(); ++Index)
	{
		RowIndex.emplace(Rows[Index].Name, static_cast<int32>(Index)); // 중복이면 앞의 것
	}
}

int32 FDataTable::FindRowIndex(std::string_view RowName) const
{
	const auto Found = RowIndex.find(std::string(RowName));
	return Found != RowIndex.end() ? Found->second : -1;
}

const FDataRow* FDataTable::FindRow(std::string_view RowName) const
{
	const int32 Index = FindRowIndex(RowName);
	return Index >= 0 ? &Rows[static_cast<size_t>(Index)] : nullptr;
}

std::vector<std::string> FDataTable::GetRowNames() const
{
	std::vector<std::string> Names;
	Names.reserve(Rows.size());
	for (const FDataRow& Row : Rows)
	{
		Names.push_back(Row.Name);
	}
	return Names;
}

const FDataValue* FDataTable::FindValue(std::string_view RowName, std::string_view FieldName) const
{
	const FDataRow* Row = FindRow(RowName);
	return Row != nullptr ? FindRecordValue(Struct.get(), Row->Record, FieldName) : nullptr;
}

bool FDataTable::GetBool(std::string_view RowName, std::string_view FieldName, bool Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	return Value != nullptr && std::holds_alternative<bool>(Value->Storage) ? Value->AsBool() : Fallback;
}

int32 FDataTable::GetInt(std::string_view RowName, std::string_view FieldName, int32 Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	if (Value == nullptr)
	{
		return Fallback;
	}
	if (const float* Number = std::get_if<float>(&Value->Storage))
	{
		return static_cast<int32>(std::lround(*Number));
	}
	return std::holds_alternative<int32>(Value->Storage) ? Value->AsInt() : Fallback;
}

float FDataTable::GetFloat(std::string_view RowName, std::string_view FieldName, float Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	return Value != nullptr && (std::holds_alternative<float>(Value->Storage) || std::holds_alternative<int32>(Value->Storage)) ? Value->AsFloat() : Fallback;
}

std::string FDataTable::GetString(std::string_view RowName, std::string_view FieldName, std::string_view Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	return Value != nullptr && std::holds_alternative<std::string>(Value->Storage) ? Value->AsString() : std::string(Fallback);
}

FVector2 FDataTable::GetVector2(std::string_view RowName, std::string_view FieldName, const FVector2& Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	return Value != nullptr && std::holds_alternative<FVector2>(Value->Storage) ? Value->AsVector2() : Fallback;
}

FVector3 FDataTable::GetVector3(std::string_view RowName, std::string_view FieldName, const FVector3& Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	return Value != nullptr && std::holds_alternative<FVector3>(Value->Storage) ? Value->AsVector3() : Fallback;
}

FVector4 FDataTable::GetVector4(std::string_view RowName, std::string_view FieldName, const FVector4& Fallback) const
{
	const FDataValue* Value = FindValue(RowName, FieldName);
	return Value != nullptr && std::holds_alternative<FVector4>(Value->Storage) ? Value->AsVector4() : Fallback;
}

bool FDataTable::IsValidRowName(std::string_view RowName)
{
	if (RowName.empty() || RowName != Trim(RowName))
	{
		return false;
	}
	return std::none_of(RowName.begin(), RowName.end(), [](char C) { return static_cast<unsigned char>(C) < 0x20; });
}

std::string FDataTable::MakeUniqueRowName(std::string_view Base) const
{
	std::string Root = IsValidRowName(Base) ? std::string(Base) : std::string("Row");
	if (!HasRow(Root))
	{
		return Root;
	}
	for (int32 Suffix = 2;; ++Suffix)
	{
		std::string Candidate = std::format("{}_{}", Root, Suffix);
		if (!HasRow(Candidate))
		{
			return Candidate;
		}
	}
}

FDataRow* FDataTable::AddRow(const std::string& RowName, int32 Index, std::string* OutError)
{
	if (Struct == nullptr)
	{
		SetError(OutError, "구조체가 없어 행을 추가할 수 없습니다");
		return nullptr;
	}
	if (!IsValidRowName(RowName))
	{
		SetError(OutError, "행 이름이 올바르지 않습니다 (비었거나 앞뒤 공백/제어 문자)");
		return nullptr;
	}
	if (HasRow(RowName))
	{
		SetError(OutError, "이미 있는 행 이름입니다: " + RowName);
		return nullptr;
	}
	const int32 Count = GetRowCount();
	Index             = Index < 0 || Index > Count ? Count : Index;
	FDataRow Row;
	Row.Name          = RowName;
	Row.Record.Values = MakeDefaults(Struct.get());
	Rows.insert(Rows.begin() + Index, std::move(Row));
	RebuildIndex();
	return &Rows[static_cast<size_t>(Index)];
}

FDataRow* FDataTable::DuplicateRow(std::string_view Source, const std::string& NewName, std::string* OutError)
{
	const int32 SourceIndex = FindRowIndex(Source);
	if (SourceIndex < 0)
	{
		SetError(OutError, std::format("행이 없습니다: {}", Source));
		return nullptr;
	}
	if (!IsValidRowName(NewName) || HasRow(NewName))
	{
		SetError(OutError, "새 행 이름이 올바르지 않거나 이미 있습니다: " + NewName);
		return nullptr;
	}
	FDataRow Copy = Rows[static_cast<size_t>(SourceIndex)];
	Copy.Name     = NewName;
	Rows.insert(Rows.begin() + SourceIndex + 1, std::move(Copy));
	RebuildIndex();
	return &Rows[static_cast<size_t>(SourceIndex + 1)];
}

bool FDataTable::RemoveRow(std::string_view RowName)
{
	const int32 Index = FindRowIndex(RowName);
	if (Index < 0)
	{
		return false;
	}
	Rows.erase(Rows.begin() + Index);
	RebuildIndex();
	return true;
}

bool FDataTable::RenameRow(std::string_view From, const std::string& To, std::string* OutError)
{
	const int32 Index = FindRowIndex(From);
	if (Index < 0)
	{
		SetError(OutError, std::format("행이 없습니다: {}", From));
		return false;
	}
	if (From == To)
	{
		return true;
	}
	if (!IsValidRowName(To) || HasRow(To))
	{
		SetError(OutError, "새 행 이름이 올바르지 않거나 이미 있습니다: " + To);
		return false;
	}
	Rows[static_cast<size_t>(Index)].Name = To;
	RebuildIndex();
	return true;
}

bool FDataTable::MoveRow(int32 From, int32 To)
{
	const int32 Count = GetRowCount();
	if (From < 0 || From >= Count || To < 0 || To >= Count)
	{
		return false;
	}
	FDataRow Row = std::move(Rows[static_cast<size_t>(From)]);
	Rows.erase(Rows.begin() + From);
	Rows.insert(Rows.begin() + To, std::move(Row));
	RebuildIndex();
	return true;
}

bool FDataTable::SetValue(std::string_view RowName, std::string_view FieldName, const FDataValue& Value, std::string* OutError)
{
	const int32 Index = FindRowIndex(RowName);
	if (Index < 0)
	{
		SetError(OutError, std::format("행이 없습니다: {}", RowName));
		return false;
	}
	return SetRecordValue(Struct.get(), Rows[static_cast<size_t>(Index)].Record, FieldName, Value, OutError);
}

void FDataTable::SetRows(std::vector<FDataRow> NewRows)
{
	Rows = std::move(NewRows);
	RebuildIndex();
}

void FDataTable::Rebind(std::shared_ptr<const FDataStruct> NewStruct, const std::vector<FDataFieldRename>& Renames)
{
	for (FDataRow& Row : Rows)
	{
		Row.Record = RebindRecord(Struct.get(), NewStruct.get(), Row.Record, Renames);
	}
	Struct = std::move(NewStruct);
}

bool FDataTable::FromJsonString(const std::string& Text, const FDataStructResolver& Resolver, FDataTable& Out, FDataLoadReport* Report, std::string* OutError)
{
	const DataJson::FJson Root = DataJson::Parse(Text);
	if (Root.is_discarded() || !Root.is_object())
	{
		SetError(OutError, "JSON 파싱 실패 (객체가 아님)");
		return false;
	}
	CheckVersion(Root, Version, Report);
	FDataTable Result;
	Result.Struct = ResolveStruct(Root, Resolver, Result.StructPath, Report);

	const auto RowsIt = Root.find("Rows");
	if (RowsIt != Root.end() && !RowsIt->is_array() && Report != nullptr)
	{
		Report->Warn("Rows가 배열이 아닙니다");
	}
	if (RowsIt != Root.end() && RowsIt->is_array())
	{
		Result.Rows.reserve(RowsIt->size());
		int32 Ordinal = 0;
		for (const DataJson::FJson& Item : *RowsIt)
		{
			++Ordinal;
			if (!Item.is_object())
			{
				if (Report != nullptr)
				{
					Report->Warn(std::format("행 #{}: 객체가 아닙니다 (무시)", Ordinal));
				}
				continue;
			}
			FDataRow Row;
			Row.Name = Item.contains("Name") && Item["Name"].is_string() ? Item["Name"].get<std::string>() : std::string();
			if (Row.Name.empty())
			{
				Row.Name = Result.MakeUniqueRowName("Row");
				if (Report != nullptr)
				{
					Report->Warn(std::format("행 #{}: 이름이 없어 '{}'(으)로 읽음", Ordinal, Row.Name));
				}
			}
			else if (Result.HasRow(Row.Name))
			{
				const std::string Unique = Result.MakeUniqueRowName(Row.Name);
				if (Report != nullptr)
				{
					Report->Warn(std::format("행 이름 중복 '{}' → '{}'(으)로 읽음", Row.Name, Unique));
				}
				Row.Name = Unique;
			}
			const auto ValuesIt = Item.find("Values");
			ReadRecord(Result.Struct.get(), ValuesIt != Item.end() ? &*ValuesIt : nullptr, Row.Record, "행 '" + Row.Name + "'", Report);
			Result.Rows.push_back(std::move(Row));
			Result.RowIndex.emplace(Result.Rows.back().Name, static_cast<int32>(Result.Rows.size() - 1));
		}
	}
	Out = std::move(Result);
	return true;
}

std::string FDataTable::ToJsonString() const
{
	DataJson::FJson Root = DataJson::FJson::object();
	Root["Version"]      = Version;
	Root["Struct"]       = StructPath;
	DataJson::FJson List = DataJson::FJson::array();
	for (const FDataRow& Row : Rows)
	{
		DataJson::FJson Item = DataJson::FJson::object();
		Item["Name"]         = Row.Name;
		Item["Values"]       = WriteRecord(Struct.get(), Row.Record);
		List.push_back(std::move(Item));
	}
	Root["Rows"] = std::move(List);
	return DataJson::FormatDocument(Root);
}

bool FDataTable::SaveToFile(const std::filesystem::path& Path, std::string* OutError) const
{
	return WriteTextFile(Path, ToJsonString(), OutError);
}

// ---------------------------------------------------------------- FDataAsset

const FDataValue* FDataAsset::FindValue(std::string_view FieldName) const
{
	return FindRecordValue(Struct.get(), Record, FieldName);
}

bool FDataAsset::GetBool(std::string_view FieldName, bool Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	return Value != nullptr && std::holds_alternative<bool>(Value->Storage) ? Value->AsBool() : Fallback;
}

int32 FDataAsset::GetInt(std::string_view FieldName, int32 Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	if (Value == nullptr)
	{
		return Fallback;
	}
	if (const float* Number = std::get_if<float>(&Value->Storage))
	{
		return static_cast<int32>(std::lround(*Number));
	}
	return std::holds_alternative<int32>(Value->Storage) ? Value->AsInt() : Fallback;
}

float FDataAsset::GetFloat(std::string_view FieldName, float Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	return Value != nullptr && (std::holds_alternative<float>(Value->Storage) || std::holds_alternative<int32>(Value->Storage)) ? Value->AsFloat() : Fallback;
}

std::string FDataAsset::GetString(std::string_view FieldName, std::string_view Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	return Value != nullptr && std::holds_alternative<std::string>(Value->Storage) ? Value->AsString() : std::string(Fallback);
}

FVector2 FDataAsset::GetVector2(std::string_view FieldName, const FVector2& Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	return Value != nullptr && std::holds_alternative<FVector2>(Value->Storage) ? Value->AsVector2() : Fallback;
}

FVector3 FDataAsset::GetVector3(std::string_view FieldName, const FVector3& Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	return Value != nullptr && std::holds_alternative<FVector3>(Value->Storage) ? Value->AsVector3() : Fallback;
}

FVector4 FDataAsset::GetVector4(std::string_view FieldName, const FVector4& Fallback) const
{
	const FDataValue* Value = FindValue(FieldName);
	return Value != nullptr && std::holds_alternative<FVector4>(Value->Storage) ? Value->AsVector4() : Fallback;
}

bool FDataAsset::SetValue(std::string_view FieldName, const FDataValue& Value, std::string* OutError)
{
	return SetRecordValue(Struct.get(), Record, FieldName, Value, OutError);
}

void FDataAsset::ResetToDefaults()
{
	Record.Values = MakeDefaults(Struct.get());
}

void FDataAsset::Rebind(std::shared_ptr<const FDataStruct> NewStruct, const std::vector<FDataFieldRename>& Renames)
{
	Record = RebindRecord(Struct.get(), NewStruct.get(), Record, Renames);
	Struct = std::move(NewStruct);
}

bool FDataAsset::FromJsonString(const std::string& Text, const FDataStructResolver& Resolver, FDataAsset& Out, FDataLoadReport* Report, std::string* OutError)
{
	const DataJson::FJson Root = DataJson::Parse(Text);
	if (Root.is_discarded() || !Root.is_object())
	{
		SetError(OutError, "JSON 파싱 실패 (객체가 아님)");
		return false;
	}
	CheckVersion(Root, Version, Report);
	FDataAsset Result;
	Result.Struct       = ResolveStruct(Root, Resolver, Result.StructPath, Report);
	const auto ValuesIt = Root.find("Values");
	ReadRecord(Result.Struct.get(), ValuesIt != Root.end() ? &*ValuesIt : nullptr, Result.Record, "값", Report);
	Out = std::move(Result);
	return true;
}

std::string FDataAsset::ToJsonString() const
{
	DataJson::FJson Root = DataJson::FJson::object();
	Root["Version"]      = Version;
	Root["Struct"]       = StructPath;
	Root["Values"]       = WriteRecord(Struct.get(), Record);
	return DataJson::FormatDocument(Root);
}

bool FDataAsset::SaveToFile(const std::filesystem::path& Path, std::string* OutError) const
{
	return WriteTextFile(Path, ToJsonString(), OutError);
}

// ---------------------------------------------------------------- 값 ↔ 글자

namespace DataValueText
{
	namespace
	{
		std::string EscapeElement(std::string_view Text)
		{
			std::string Result;
			Result.reserve(Text.size());
			for (char C : Text)
			{
				if (C == '|' || C == '\\')
				{
					Result.push_back('\\');
				}
				Result.push_back(C);
			}
			return Result;
		}

		std::vector<std::string> SplitElements(std::string_view Text)
		{
			std::vector<std::string> Result(1);
			for (size_t Index = 0; Index < Text.size(); ++Index)
			{
				const char C = Text[Index];
				if (C == '\\' && Index + 1 < Text.size())
				{
					Result.back().push_back(Text[++Index]);
				}
				else if (C == '|')
				{
					Result.emplace_back();
				}
				else
				{
					Result.back().push_back(C);
				}
			}
			return Result;
		}
	} // namespace

	std::string ToText(const FDataField& Field, const FDataValue& Value)
	{
		switch (Field.Type)
		{
		case EDataFieldType::Bool: return Value.AsBool() ? "true" : "false";
		case EDataFieldType::Int: return std::to_string(Value.AsInt());
		case EDataFieldType::Float: return DataJson::FormatFloat(Value.AsFloat());
		case EDataFieldType::Vector2:
		case EDataFieldType::Vector3:
		case EDataFieldType::Vector4:
		case EDataFieldType::Color:
		{
			float Components[4];
			GetComponents(Field.Type, Value, Components);
			std::string Result;
			for (int32 Index = 0; Index < VectorComponentCount(Field.Type); ++Index)
			{
				Result += (Index > 0 ? ";" : "") + DataJson::FormatFloat(Components[Index]);
			}
			return Result;
		}
		case EDataFieldType::Array:
		{
			const FDataField Element = Field.MakeElementField();
			std::string      Result;
			bool             bFirst = true;
			for (const FDataValue& Item : Value.AsArray())
			{
				Result += (bFirst ? "" : "|") + EscapeElement(ToText(Element, Item));
				bFirst = false;
			}
			return Result;
		}
		default: return Value.AsString();
		}
	}

	bool FromText(const FDataField& Field, std::string_view Text, FDataValue& Out, std::string* OutError)
	{
		if (IsStringDataType(Field.Type))
		{
			if (Field.Type == EDataFieldType::Enum)
			{
				const std::string Name(Trim(Text));
				if (Name.empty())
				{
					Out = Field.Default;
					return true;
				}
				if (std::find(Field.EnumValues.begin(), Field.EnumValues.end(), Name) == Field.EnumValues.end())
				{
					SetError(OutError, std::format("Enum 목록에 없는 값 '{}'", Name));
					return false;
				}
				Out = FDataValue::MakeString(Name);
				return true;
			}
			Out = FDataValue::MakeString(std::string(Text));
			return true;
		}
		if (Field.Type == EDataFieldType::Array)
		{
			FDataValue::FArray Elements;
			if (!Trim(Text).empty())
			{
				const FDataField Element = Field.MakeElementField();
				for (const std::string& Part : SplitElements(Text))
				{
					FDataValue Item;
					if (!FromText(Element, Part, Item, OutError))
					{
						return false;
					}
					Elements.push_back(std::move(Item));
				}
			}
			Out = FDataValue::MakeArray(std::move(Elements));
			return true;
		}
		const std::string_view Trimmed = Trim(Text);
		if (Trimmed.empty())
		{
			Out = Field.Default;
			return true;
		}
		switch (Field.Type)
		{
		case EDataFieldType::Bool:
		{
			const std::string Lower = ToLowerAscii(Trimmed);
			if (Lower == "true" || Lower == "1" || Lower == "yes")
			{
				Out = FDataValue::MakeBool(true);
				return true;
			}
			if (Lower == "false" || Lower == "0" || Lower == "no")
			{
				Out = FDataValue::MakeBool(false);
				return true;
			}
			break;
		}
		case EDataFieldType::Int:
		{
			int32 Value = 0;
			if (ParseInt(Trimmed, Value))
			{
				Out = FDataValue::MakeInt(Value);
				return true;
			}
			break;
		}
		case EDataFieldType::Float:
		{
			float Value = 0.0f;
			if (ParseFloat(Trimmed, Value))
			{
				Out = FDataValue::MakeFloat(Value);
				return true;
			}
			break;
		}
		default:
		{
			float Components[4] = {};
			if (ParseComponents(Trimmed, VectorComponentCount(Field.Type), Field.Type == EDataFieldType::Color, Components))
			{
				Out = MakeVectorValue(Field.Type, Components);
				return true;
			}
			break;
		}
		}
		SetError(OutError, std::format("{} 값으로 읽을 수 없습니다: '{}'", ToString(Field.Type), Trimmed));
		return false;
	}

	FDataValue Convert(const FDataField& From, const FDataField& To, const FDataValue& Value)
	{
		FDataValue Result = To.Default;
		if (From.Type == EDataFieldType::Array && To.Type == EDataFieldType::Array)
		{
			const FDataField   FromElement = From.MakeElementField();
			const FDataField   ToElement   = To.MakeElementField();
			FDataValue::FArray Elements;
			for (const FDataValue& Item : Value.AsArray())
			{
				Elements.push_back(Convert(FromElement, ToElement, Item));
			}
			Result = FDataValue::MakeArray(std::move(Elements));
		}
		else if (To.Type == EDataFieldType::Array)
		{
			Result = FDataValue::MakeArray({ Convert(From, To.MakeElementField(), Value) });
		}
		else if (From.Type == EDataFieldType::Array)
		{
			const FDataValue::FArray& Elements = Value.AsArray();
			Result                             = Elements.empty() ? To.Default : Convert(From.MakeElementField(), To, Elements.front());
		}
		else if (IsNumericScalar(From.Type) && IsNumericScalar(To.Type))
		{
			const float Number = From.Type == EDataFieldType::Bool ? (Value.AsBool() ? 1.0f : 0.0f) : Value.AsFloat();
			if (To.Type == EDataFieldType::Bool)
			{
				Result = FDataValue::MakeBool(Number != 0.0f);
			}
			else if (To.Type == EDataFieldType::Int)
			{
				const double Clamped = std::clamp(static_cast<double>(std::round(Number)), static_cast<double>(std::numeric_limits<int32>::min()),
				                                  static_cast<double>(std::numeric_limits<int32>::max()));
				Result               = FDataValue::MakeInt(static_cast<int32>(Clamped));
			}
			else
			{
				Result = FDataValue::MakeFloat(Number);
			}
		}
		else if (VectorComponentCount(From.Type) > 0 && VectorComponentCount(To.Type) > 0)
		{
			float Components[4];
			GetComponents(From.Type, Value, Components);
			if (To.Type == EDataFieldType::Color && From.Type != EDataFieldType::Vector4 && From.Type != EDataFieldType::Color)
			{
				Components[3] = 1.0f;
			}
			Result = MakeVectorValue(To.Type, Components);
		}
		else
		{
			FDataValue Parsed;
			if (FromText(To, ToText(From, Value), Parsed, nullptr))
			{
				Result = std::move(Parsed);
			}
		}
		return To.Accepts(Result) ? Result : To.Default;
	}
} // namespace DataValueText
