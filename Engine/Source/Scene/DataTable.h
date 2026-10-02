#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// 데이터 테이블 / 데이터 에셋 (언리얼 DataTable + Row Struct, Primary Data Asset식). JSON, 경로는 모두 Content 기준.
//   .estruct  구조체 정의 (여러 테이블/데이터 에셋이 공유)
//     { "Version": 1, "Name": "Weapon", "Description": "...",
//       "Fields": [ { "Name": "Damage", "Type": "Float", "Default": 10, "Description": "..." },
//                   { "Name": "Rarity", "Type": "Enum", "Values": ["Common", "Rare"], "Default": "Common" },
//                   { "Name": "Icon", "Type": "Asset", "Filter": ".png;.jpg" },
//                   { "Name": "Upgrade", "Type": "RowRef", "Table": "Data/Weapons.etable" },
//                   { "Name": "Tags", "Type": "Array", "Element": "String", "Default": ["a"] } ] }
//   .etable   데이터 테이블 = 구조체 + 순서 있는 행 목록
//     { "Version": 1, "Struct": "Data/Weapon.estruct", "Rows": [ { "Name": "Sword", "Values": { "Damage": 12 } } ] }
//   .edata    데이터 에셋 = 구조체 값 하나
//     { "Version": 1, "Struct": "Data/Balance.estruct", "Values": { "StartGold": 50 } }
// 필드 타입과 JSON 값:
//   Bool(true/false) Int(정수, int32) Float(수) String/Text(문자열 — Text는 여러 줄 편집 힌트) Vector2/3/4([x,y(,z(,w))])
//   Color([r,g,b,a] sRGB 0~1, 읽을 때 "#RRGGBB[AA]"도 허용) Enum(이름 문자열, Values 중 하나) Asset(Content 기준 경로, Filter = ".png;.glb" 확장자 목록)
//   RowRef(행 이름 문자열, Table = 대상 .etable — 빈 문자열 = 참조 없음) Array(Element = Array/Struct를 뺀 위 타입 하나 + 그 타입의 Values/Filter/Table)
//   중첩 구조체(Struct 타입)는 v1에서 지원하지 않는다 — 다른 테이블 행 참조(RowRef)나 필드 펼치기로 표현한다.
// 규칙:
//   - 필드 이름은 식별자([A-Za-z_][A-Za-z0-9_]*)이고 고유하다. "Name"은 행 이름 자리(Lua 결과의 Name)라 예약어
//   - 행 이름은 비어 있지 않고 고유하다(읽을 때 중복은 경고 후 "이름_2"처럼 바꿔 데이터를 잃지 않는다). 행 순서는 파일 순서
//   - 값이 없는 필드 = 구조체 기본값. 타입이 맞지 않는 값/목록에 없는 Enum = 경고 후 기본값
//   - 구조체에 없는 필드 값은 경고 후 원문(JSON)을 보존해 저장 시 다시 쓴다(구조체 필드 이름을 바꾼 뒤 Rebind 마이그레이션이 되살린다)
//   - 저장은 모든 필드 값을 구조체 필드 순서로 쓴다(행 하나 = 한 줄)
//   - 어떤 입력에도 크래시하지 않는다: 파싱 실패는 false + 오류, 의미 오류는 경고 목록(FDataLoadReport)
// 경로 캐시/핫 리로드/참조 검증은 Scene/DataLibrary.h, CSV는 Scene/DataCsv.h, Lua는 Scripting/ScriptDataBindings.cpp.

enum class EDataFieldType : uint8
{
	Bool,
	Int,
	Float,
	String,
	Text,
	Vector2,
	Vector3,
	Vector4,
	Color,
	Enum,
	Asset,
	RowRef,
	Array,
};

const char*    ToString(EDataFieldType Type);
bool           TryParseDataFieldType(std::string_view Text, EDataFieldType& OutType);
bool           IsStringDataType(EDataFieldType Type); // String/Text/Enum/Asset/RowRef (값 = 문자열)

// 값 하나 (타입은 필드가 정한다 — 같은 저장 형식을 여러 필드 타입이 공유: 문자열 5종, FVector4 = Vector4/Color)
struct FDataValue
{
	using FArray = std::vector<FDataValue>;

	std::variant<bool, int32, float, std::string, FVector2, FVector3, FVector4, FArray> Storage;

	static FDataValue MakeBool(bool Value);
	static FDataValue MakeInt(int32 Value);
	static FDataValue MakeFloat(float Value);
	static FDataValue MakeString(std::string Value);
	static FDataValue MakeVector2(const FVector2& Value);
	static FDataValue MakeVector3(const FVector3& Value);
	static FDataValue MakeVector4(const FVector4& Value);
	static FDataValue MakeArray(FArray Elements);

	// 저장 형식이 다르면 0/빈 값 (크래시하지 않음)
	bool               AsBool() const;
	int32              AsInt() const;
	float              AsFloat() const; // Int도 변환
	const std::string& AsString() const;
	FVector2           AsVector2() const;
	FVector3           AsVector3() const;
	FVector4           AsVector4() const;
	const FArray&      AsArray() const;

	bool operator==(const FDataValue& Other) const;
};

// 구조체 필드 정의
struct FDataField
{
	std::string              Name;
	EDataFieldType           Type        = EDataFieldType::Float;
	EDataFieldType           ElementType = EDataFieldType::Float; // Array 전용
	FDataValue               Default;                              // 이 필드 타입의 값 (Array면 배열)
	std::string              Description;
	std::vector<std::string> EnumValues; // Enum (또는 Enum 배열)
	std::string              Filter;     // Asset: ".png;.glb" (비면 모든 파일)
	std::string              Table;      // RowRef: 대상 .etable (Content 기준)

	// 타입 기본값 (false, 0, "", 0 벡터, 색 흰색 (1,1,1,1), Enum 첫 값, 빈 배열)
	static FDataValue MakeTypeDefault(EDataFieldType Type, const std::vector<std::string>& EnumValues);
	// Array의 요소 하나를 나타내는 필드 (Type = ElementType, Values/Filter/Table 복사). 배열이 아니면 자신 복사
	FDataField        MakeElementField() const;
	// 값의 저장 형식이 이 필드와 맞는가 (Enum은 목록 포함까지, Array는 요소마다)
	bool              Accepts(const FDataValue& Value) const;
	// Asset 필터 목록 (소문자, "." 포함)
	std::vector<std::string> GetFilterExtensions() const;
};

// 구조체/테이블 읽기 결과 경고 (의미 오류 — 파일은 읽힘)
struct FDataLoadReport
{
	std::vector<std::string> Warnings;
	void Warn(std::string Message) { Warnings.push_back(std::move(Message)); }
};

// 필드 이름 변경 (마이그레이션). 목록 순서대로 적용하므로 A→B, B→C 연쇄도 된다
struct FDataFieldRename
{
	std::string From;
	std::string To;
};

class FDataStruct
{
public:
	static constexpr const wchar_t* Extension = L".estruct";
	static constexpr int32          Version   = 1;

	std::string             Name;
	std::string             Description;
	std::vector<FDataField> Fields;

	int32             FindField(std::string_view FieldName) const; // 없으면 -1
	const FDataField* GetField(std::string_view FieldName) const;

	// 필드 정의 검사 (이름 규칙·중복·Enum 값·배열 요소·기본값 타입). Fields에 들어 있지 않은 새 필드도 검사할 수 있다(Ignore = 중복 검사에서 뺄 칸)
	bool ValidateField(const FDataField& Field, int32 IgnoreIndex, std::string* OutError) const;
	static bool IsValidFieldName(std::string_view FieldName);

	// ---- 편집 (실패하면 false + 오류, 구조체는 그대로). 이 구조체를 쓰는 테이블/에셋은 FDataTable::Rebind로 맞춘다
	bool AddField(FDataField Field, int32 Index = -1, std::string* OutError = nullptr); // -1 = 끝
	bool RemoveField(std::string_view FieldName);
	bool RenameField(std::string_view From, const std::string& To, std::string* OutError = nullptr);
	bool MoveField(int32 From, int32 To);
	bool SetField(int32 Index, FDataField Field, std::string* OutError = nullptr); // 정의 교체 (타입 변경 포함, 기본값이 안 맞으면 타입 기본값)
	std::string MakeUniqueFieldName(std::string_view Base) const;

	// ---- JSON
	static bool FromJsonString(const std::string& Text, FDataStruct& Out, FDataLoadReport* Report = nullptr, std::string* OutError = nullptr);
	std::string ToJsonString() const;
};

// 행/에셋이 공유하는 값 묶음: 구조체 필드 순서와 같은 칸 + 구조체에 없는 필드 원문
struct FDataRecord
{
	std::vector<FDataValue>                          Values;  // Struct->Fields와 같은 길이
	std::vector<std::pair<std::string, std::string>> Unknown; // (필드 이름, 원문 JSON) — 저장 시 보존
};

struct FDataRow
{
	std::string Name;
	FDataRecord Record;
};

// 구조체를 경로로 찾는 함수 (라이브러리는 캐시 로드, 테스트는 람다). 없으면 nullptr
using FDataStructResolver = std::function<std::shared_ptr<const FDataStruct>(const std::string& StructPath)>;

class FDataTable
{
public:
	static constexpr const wchar_t* Extension = L".etable";
	static constexpr int32          Version   = 1;

	std::string                        StructPath; // Content 기준
	std::shared_ptr<const FDataStruct> Struct;     // 해석 결과 (없으면 nullptr — 모든 값이 Record.Unknown에 남는다)

	// ---- 조회
	const std::vector<FDataRow>& GetRows() const { return Rows; }
	int32                        GetRowCount() const { return static_cast<int32>(Rows.size()); }
	int32                        FindRowIndex(std::string_view RowName) const; // 없으면 -1
	const FDataRow*              FindRow(std::string_view RowName) const;
	bool                         HasRow(std::string_view RowName) const { return FindRowIndex(RowName) >= 0; }
	std::vector<std::string>     GetRowNames() const;
	// 행/필드가 없거나 구조체가 없으면 nullptr
	const FDataValue* FindValue(std::string_view RowName, std::string_view FieldName) const;
	// 형식 지정 읽기 (없으면 Fallback)
	bool        GetBool(std::string_view RowName, std::string_view FieldName, bool Fallback = false) const;
	int32       GetInt(std::string_view RowName, std::string_view FieldName, int32 Fallback = 0) const;
	float       GetFloat(std::string_view RowName, std::string_view FieldName, float Fallback = 0.0f) const;
	std::string GetString(std::string_view RowName, std::string_view FieldName, std::string_view Fallback = {}) const; // 문자열 5종
	FVector2    GetVector2(std::string_view RowName, std::string_view FieldName, const FVector2& Fallback = FVector2(0.0f, 0.0f)) const;
	FVector3    GetVector3(std::string_view RowName, std::string_view FieldName, const FVector3& Fallback = FVector3(0.0f, 0.0f, 0.0f)) const;
	FVector4    GetVector4(std::string_view RowName, std::string_view FieldName, const FVector4& Fallback = FVector4(0.0f, 0.0f, 0.0f, 0.0f)) const; // Vector4/Color

	// ---- 편집 (실패하면 false/nullptr + 오류). 구조체가 없으면 행 추가/값 설정은 실패한다
	FDataRow*   AddRow(const std::string& RowName, int32 Index = -1, std::string* OutError = nullptr); // 모든 값 = 기본값
	FDataRow*   DuplicateRow(std::string_view Source, const std::string& NewName, std::string* OutError = nullptr); // 원본 바로 뒤
	bool        RemoveRow(std::string_view RowName);
	bool        RenameRow(std::string_view From, const std::string& To, std::string* OutError = nullptr);
	bool        MoveRow(int32 From, int32 To);
	bool        SetValue(std::string_view RowName, std::string_view FieldName, const FDataValue& Value, std::string* OutError = nullptr);
	void        SetRows(std::vector<FDataRow> NewRows); // 전체 교체 (CSV 가져오기 등 — 호출자가 이름 고유성과 값 칸 수를 맞춘다)
	std::string MakeUniqueRowName(std::string_view Base) const;
	static bool IsValidRowName(std::string_view RowName);

	// 구조체 교체 + 값 마이그레이션: 이름이 같은(Renames 적용 후) 필드 값은 옮기고(타입이 바뀌면 변환, 실패하면 기본값),
	// 사라진 필드 값은 버리며, 새 필드는 기본값. Unknown 원문도 새 구조체 필드와 이름이 맞으면 되살린다
	// (한 필드로 여러 값이 모이면: 이름이 바뀐 구조체 값 > Unknown 원문 > 이름 그대로인 구조체 값)
	void Rebind(std::shared_ptr<const FDataStruct> NewStruct, const std::vector<FDataFieldRename>& Renames = {});

	// ---- JSON (Resolver로 Struct를 찾는다 — 못 찾으면 경고 후 Struct = nullptr로 읽음)
	static bool FromJsonString(const std::string& Text, const FDataStructResolver& Resolver, FDataTable& Out, FDataLoadReport* Report = nullptr,
	                           std::string* OutError = nullptr);
	std::string ToJsonString() const;
	bool        SaveToFile(const std::filesystem::path& Path, std::string* OutError = nullptr) const;

private:
	void RebuildIndex();

	std::vector<FDataRow>                  Rows;
	std::unordered_map<std::string, int32> RowIndex;
};

class FDataAsset
{
public:
	static constexpr const wchar_t* Extension = L".edata";
	static constexpr int32          Version   = 1;

	std::string                        StructPath;
	std::shared_ptr<const FDataStruct> Struct;
	FDataRecord                        Record;

	const FDataValue* FindValue(std::string_view FieldName) const;
	bool              GetBool(std::string_view FieldName, bool Fallback = false) const;
	int32             GetInt(std::string_view FieldName, int32 Fallback = 0) const;
	float             GetFloat(std::string_view FieldName, float Fallback = 0.0f) const;
	std::string       GetString(std::string_view FieldName, std::string_view Fallback = {}) const;
	FVector2          GetVector2(std::string_view FieldName, const FVector2& Fallback = FVector2(0.0f, 0.0f)) const;
	FVector3          GetVector3(std::string_view FieldName, const FVector3& Fallback = FVector3(0.0f, 0.0f, 0.0f)) const;
	FVector4          GetVector4(std::string_view FieldName, const FVector4& Fallback = FVector4(0.0f, 0.0f, 0.0f, 0.0f)) const;

	bool SetValue(std::string_view FieldName, const FDataValue& Value, std::string* OutError = nullptr);
	void ResetToDefaults(); // 모든 값 = 구조체 기본값 (Unknown 유지)
	void Rebind(std::shared_ptr<const FDataStruct> NewStruct, const std::vector<FDataFieldRename>& Renames = {});

	static bool FromJsonString(const std::string& Text, const FDataStructResolver& Resolver, FDataAsset& Out, FDataLoadReport* Report = nullptr,
	                           std::string* OutError = nullptr);
	std::string ToJsonString() const;
	bool        SaveToFile(const std::filesystem::path& Path, std::string* OutError = nullptr) const;
};

// 값 ↔ 글자 (CSV 칸, 편집기 셀 공용). 규칙은 Scene/DataCsv.h 머리 주석
namespace DataValueText
{
	std::string ToText(const FDataField& Field, const FDataValue& Value);
	// 실패하면 false + 오류 (Out은 그대로). 빈 글자 = 문자열 타입은 "", 그 밖은 필드 기본값
	bool        FromText(const FDataField& Field, std::string_view Text, FDataValue& Out, std::string* OutError = nullptr);
	// 필드 정의가 바뀐 값 변환 (숫자/벡터 사이 직접 변환, 그 밖은 글자 경유). 실패하면 To의 기본값
	FDataValue  Convert(const FDataField& From, const FDataField& To, const FDataValue& Value);
} // namespace DataValueText
