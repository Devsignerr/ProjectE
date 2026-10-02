#include "Scene/DataLibrary.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scene/DataJson.h"
#include "Scene/Prefab.h"

#include <algorithm>
#include <cwctype>
#include <format>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	constexpr size_t MaxReferenceWarnings = 32; // 한 파일에서 남기는 참조 경고 상한 (나머지는 개수만)

	std::wstring LowerWide(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}

	std::wstring LowerExtension(const std::filesystem::path& Path) { return LowerWide(Path.extension().wstring()); }

	std::filesystem::path ResolveAgainst(const std::filesystem::path& ContentDirectory, const std::string& AssetPath)
	{
		const std::filesystem::path Path = FStringConv::ToWide(AssetPath);
		return (Path.is_absolute() ? Path : ContentDirectory / Path).lexically_normal();
	}

	bool ReadDiskText(const std::filesystem::path& Path, std::string& Out)
	{
		std::ifstream File(Path, std::ios::binary);
		if (!File)
		{
			return false;
		}
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		Out = Buffer.str();
		return true;
	}

	bool WriteDiskText(const std::filesystem::path& Path, const std::string& Text, std::string* OutError)
	{
		std::error_code ErrorCode;
		if (Path.has_parent_path())
		{
			std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		}
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		if (File)
		{
			File << Text;
		}
		if (!File)
		{
			if (OutError != nullptr)
			{
				*OutError = "파일을 쓸 수 없습니다: " + FStringConv::ToUtf8(Path.generic_wstring());
			}
			return false;
		}
		return true;
	}

	// 참조 경고 모음 (상한)
	struct FWarnings
	{
		std::vector<std::string> List;
		size_t                   Dropped = 0;

		void Add(std::string Message)
		{
			if (List.size() < MaxReferenceWarnings)
			{
				List.push_back(std::move(Message));
			}
			else
			{
				++Dropped;
			}
		}
		std::vector<std::string> Finish()
		{
			if (Dropped > 0)
			{
				List.push_back(std::format("... 참조 경고 {}건 더", Dropped));
			}
			return std::move(List);
		}
	};
} // namespace

FDataLibrary& FDataLibrary::Get()
{
	static FDataLibrary Instance;
	return Instance;
}

std::filesystem::path FDataLibrary::ResolvePath(const std::string& AssetPath) const
{
	return FPrefabLibrary::Get().ResolveAssetPath(AssetPath);
}

std::wstring FDataLibrary::MakeKey(const std::string& AssetPath)
{
	return LowerWide(FPrefabLibrary::Get().ResolveAssetPath(AssetPath).lexically_normal().generic_wstring());
}

bool FDataLibrary::IsDataExtension(const std::wstring& LowerExtension)
{
	return LowerExtension == FDataStruct::Extension || LowerExtension == FDataTable::Extension || LowerExtension == FDataAsset::Extension;
}

std::string FDataLibrary::MakeDisplayPath(const std::string& AssetPath) const
{
	return FPrefabLibrary::Get().MakeAssetPath(ResolvePath(AssetPath)); // Content 안이면 Content 기준 (Lua가 넘긴 절대 경로도)
}

void FDataLibrary::Log(const std::string& AssetPath, const std::vector<std::string>& Warnings) const
{
	if (Warnings.empty())
	{
		return;
	}
	const std::string Display = MakeDisplayPath(AssetPath);
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogScene, Warning, "[데이터] {}: {}", Display, Warning);
	}
}

void FDataLibrary::Invalidate()
{
	Structs.clear();
	Tables.clear();
	Assets.clear();
	++Generation;
}

void FDataLibrary::Invalidate(const std::string& AssetPath)
{
	const std::wstring Key = MakeKey(AssetPath);
	Structs.erase(Key);
	Tables.erase(Key);
	Assets.erase(Key);
	// 구조체가 바뀌면 그것을 쓰는 테이블/에셋의 값 칸이 달라진다 → 모두 다시 읽게 한다
	if (LowerExtension(FStringConv::ToWide(AssetPath)) == FDataStruct::Extension)
	{
		Tables.clear();
		Assets.clear();
	}
	++Generation;
}

std::shared_ptr<const FDataStruct> FDataLibrary::LoadStruct(const std::string& AssetPath)
{
	const std::wstring Key = MakeKey(AssetPath);
	if (const auto Found = Structs.find(Key); Found != Structs.end())
	{
		return Found->second;
	}
	std::shared_ptr<const FDataStruct> Result;
	std::string                        Text;
	if (!FFileSystem::ReadTextFile(ResolvePath(AssetPath), Text))
	{
		E_LOG(LogScene, Warning, "[데이터] 구조체를 읽을 수 없습니다: {}", MakeDisplayPath(AssetPath));
	}
	else
	{
		auto            Struct = std::make_shared<FDataStruct>();
		FDataLoadReport Report;
		std::string     Error;
		if (FDataStruct::FromJsonString(Text, *Struct, &Report, &Error))
		{
			Result = std::move(Struct);
			Log(AssetPath, Report.Warnings);
		}
		else
		{
			E_LOG(LogScene, Warning, "[데이터] 구조체 형식 오류 ({}): {}", MakeDisplayPath(AssetPath), Error);
		}
	}
	Structs[Key] = Result;
	return Result;
}

std::shared_ptr<const FDataTable> FDataLibrary::LoadTable(const std::string& AssetPath)
{
	const std::wstring Key = MakeKey(AssetPath);
	if (const auto Found = Tables.find(Key); Found != Tables.end())
	{
		return Found->second;
	}
	std::shared_ptr<const FDataTable> Result;
	std::string                       Text;
	if (!FFileSystem::ReadTextFile(ResolvePath(AssetPath), Text))
	{
		E_LOG(LogScene, Warning, "[데이터] 테이블을 읽을 수 없습니다: {}", MakeDisplayPath(AssetPath));
	}
	else
	{
		auto            Table = std::make_shared<FDataTable>();
		FDataLoadReport Report;
		std::string     Error;
		if (FDataTable::FromJsonString(Text, [this](const std::string& Path) { return LoadStruct(Path); }, *Table, &Report, &Error))
		{
			Result = std::move(Table);
			Log(AssetPath, Report.Warnings);
		}
		else
		{
			E_LOG(LogScene, Warning, "[데이터] 테이블 형식 오류 ({}): {}", MakeDisplayPath(AssetPath), Error);
		}
	}
	// 캐시에 먼저 넣고 참조를 검증한다 → 서로(또는 자신을) 참조하는 테이블도 무한 재귀 없이 끝난다
	Tables[Key] = Result;
	if (Result != nullptr)
	{
		Log(AssetPath, ValidateReferences(*Result, AssetPath));
	}
	return Result;
}

std::shared_ptr<const FDataAsset> FDataLibrary::LoadDataAsset(const std::string& AssetPath)
{
	const std::wstring Key = MakeKey(AssetPath);
	if (const auto Found = Assets.find(Key); Found != Assets.end())
	{
		return Found->second;
	}
	std::shared_ptr<const FDataAsset> Result;
	std::string                       Text;
	if (!FFileSystem::ReadTextFile(ResolvePath(AssetPath), Text))
	{
		E_LOG(LogScene, Warning, "[데이터] 데이터 에셋을 읽을 수 없습니다: {}", MakeDisplayPath(AssetPath));
	}
	else
	{
		auto            Asset = std::make_shared<FDataAsset>();
		FDataLoadReport Report;
		std::string     Error;
		if (FDataAsset::FromJsonString(Text, [this](const std::string& Path) { return LoadStruct(Path); }, *Asset, &Report, &Error))
		{
			Result = std::move(Asset);
			Log(AssetPath, Report.Warnings);
		}
		else
		{
			E_LOG(LogScene, Warning, "[데이터] 데이터 에셋 형식 오류 ({}): {}", MakeDisplayPath(AssetPath), Error);
		}
	}
	Assets[Key] = Result;
	if (Result != nullptr)
	{
		Log(AssetPath, ValidateReferences(*Result));
	}
	return Result;
}

namespace
{
	// 레코드 하나의 RowRef/Asset 값 검사. Self = 자기 자신 테이블(경로 키가 같으면 캐시 대신 이것)
	void ValidateRecord(FDataLibrary& Library, const FDataStruct& Struct, const FDataRecord& Record, const std::string& Context,
	                    const std::wstring& SelfKey, const FDataTable* Self, FWarnings& Warnings,
	                    const std::function<std::wstring(const std::string&)>& MakeKey)
	{
		for (size_t Index = 0; Index < Struct.Fields.size() && Index < Record.Values.size(); ++Index)
		{
			const FDataField&    Field     = Struct.Fields[Index];
			const EDataFieldType ValueType = Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type;
			if (ValueType != EDataFieldType::RowRef && ValueType != EDataFieldType::Asset)
			{
				continue;
			}
			std::vector<std::string> Values;
			if (Field.Type == EDataFieldType::Array)
			{
				for (const FDataValue& Item : Record.Values[Index].AsArray())
				{
					Values.push_back(Item.AsString());
				}
			}
			else
			{
				Values.push_back(Record.Values[Index].AsString());
			}

			if (ValueType == EDataFieldType::RowRef)
			{
				if (Field.Table.empty())
				{
					continue; // 구조체 수준 경고로 한 번만
				}
				const FDataTable*                 Target = nullptr;
				std::shared_ptr<const FDataTable> Holder;
				if (Self != nullptr && !SelfKey.empty() && MakeKey(Field.Table) == SelfKey)
				{
					Target = Self;
				}
				else
				{
					Holder = Library.LoadTable(Field.Table);
					Target = Holder.get();
				}
				for (const std::string& Value : Values)
				{
					if (Value.empty())
					{
						continue; // 참조 없음
					}
					if (Target == nullptr)
					{
						Warnings.Add(std::format("{} 필드 '{}': 대상 테이블을 읽을 수 없습니다 ({})", Context, Field.Name, Field.Table));
						break;
					}
					if (!Target->HasRow(Value))
					{
						Warnings.Add(std::format("{} 필드 '{}': 없는 행 '{}' ({})", Context, Field.Name, Value, Field.Table));
					}
				}
				continue;
			}

			const std::vector<std::string> Extensions = Field.GetFilterExtensions();
			for (const std::string& Value : Values)
			{
				if (Value.empty() || Value.rfind("primitive:", 0) == 0)
				{
					continue;
				}
				std::wstring Extension = LowerExtension(FStringConv::ToWide(Value));
				if (!Extensions.empty() &&
				    std::find(Extensions.begin(), Extensions.end(), FStringConv::ToUtf8(Extension)) == Extensions.end())
				{
					Warnings.Add(std::format("{} 필드 '{}': 필터({})에 맞지 않는 에셋 '{}'", Context, Field.Name, Field.Filter, Value));
				}
				else if (!FFileSystem::Exists(Library.ResolvePath(Value)))
				{
					Warnings.Add(std::format("{} 필드 '{}': 에셋 파일이 없습니다 '{}'", Context, Field.Name, Value));
				}
			}
		}
	}

	void ValidateStructFields(const FDataStruct& Struct, FWarnings& Warnings)
	{
		for (const FDataField& Field : Struct.Fields)
		{
			const EDataFieldType ValueType = Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type;
			if (ValueType == EDataFieldType::RowRef && Field.Table.empty())
			{
				Warnings.Add(std::format("필드 '{}': RowRef 대상 Table이 지정되지 않았습니다", Field.Name));
			}
		}
	}
} // namespace

std::vector<std::string> FDataLibrary::ValidateReferences(const FDataTable& Table, const std::string& SelfPath)
{
	FWarnings Warnings;
	if (Table.Struct == nullptr)
	{
		return {}; // 구조체 없음은 읽을 때 이미 경고
	}
	ValidateStructFields(*Table.Struct, Warnings);
	const std::wstring SelfKey = SelfPath.empty() ? std::wstring() : MakeKey(SelfPath);
	for (const FDataRow& Row : Table.GetRows())
	{
		ValidateRecord(*this, *Table.Struct, Row.Record, "행 '" + Row.Name + "'", SelfKey, &Table, Warnings, &FDataLibrary::MakeKey);
	}
	return Warnings.Finish();
}

std::vector<std::string> FDataLibrary::ValidateReferences(const FDataAsset& Asset)
{
	FWarnings Warnings;
	if (Asset.Struct == nullptr)
	{
		return {};
	}
	ValidateStructFields(*Asset.Struct, Warnings);
	ValidateRecord(*this, *Asset.Struct, Asset.Record, "값", std::wstring(), nullptr, Warnings, &FDataLibrary::MakeKey);
	return Warnings.Finish();
}

bool FDataLibrary::SaveStruct(const std::string& AssetPath, const FDataStruct& Struct, std::string* OutError)
{
	const bool bOk = WriteDiskText(ResolvePath(AssetPath), Struct.ToJsonString(), OutError);
	Invalidate(AssetPath);
	return bOk;
}

bool FDataLibrary::SaveTable(const std::string& AssetPath, const FDataTable& Table, std::string* OutError)
{
	const bool bOk = Table.SaveToFile(ResolvePath(AssetPath), OutError);
	Invalidate(AssetPath);
	return bOk;
}

bool FDataLibrary::SaveDataAsset(const std::string& AssetPath, const FDataAsset& Asset, std::string* OutError)
{
	const bool bOk = Asset.SaveToFile(ResolvePath(AssetPath), OutError);
	Invalidate(AssetPath);
	return bOk;
}

std::vector<std::filesystem::path> FDataLibrary::FindStructUsers(const std::filesystem::path& ContentDirectory, const std::string& StructPath)
{
	std::vector<std::filesystem::path> Users;
	const std::wstring                 StructKey = LowerWide(ResolveAgainst(ContentDirectory, StructPath).generic_wstring());
	std::error_code                    ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode);
	     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
	{
		const std::wstring Extension = LowerExtension(It->path());
		if (!It->is_regular_file(ErrorCode) || (Extension != FDataTable::Extension && Extension != FDataAsset::Extension))
		{
			continue;
		}
		std::string Text;
		if (!ReadDiskText(It->path(), Text))
		{
			continue;
		}
		const DataJson::FJson Root = DataJson::Parse(Text);
		const std::string     Uses = DataJson::GetString(Root, "Struct");
		if (!Uses.empty() && LowerWide(ResolveAgainst(ContentDirectory, Uses).generic_wstring()) == StructKey)
		{
			Users.push_back(It->path());
		}
	}
	std::sort(Users.begin(), Users.end());
	return Users;
}

bool FDataLibrary::SaveStructAndMigrate(const std::string& StructPath, const FDataStruct& NewStruct, const std::vector<FDataFieldRename>& Renames,
                                        const std::filesystem::path& ContentDirectory, std::vector<std::filesystem::path>* OutChangedFiles,
                                        std::string* OutError)
{
	const std::filesystem::path StructFile = ResolveAgainst(ContentDirectory, StructPath);
	const std::wstring          StructKey  = LowerWide(StructFile.generic_wstring());

	// 1) 저장 전 디스크의 옛 구조체 (새 파일이면 없음 — 사용자 값은 Unknown 원문에서 이름으로 되살린다)
	std::shared_ptr<const FDataStruct> OldStruct;
	if (std::string OldText; ReadDiskText(StructFile, OldText))
	{
		auto Parsed = std::make_shared<FDataStruct>();
		if (FDataStruct::FromJsonString(OldText, *Parsed))
		{
			OldStruct = std::move(Parsed);
		}
	}
	const FDataStructResolver Resolver = [&](const std::string& Path) -> std::shared_ptr<const FDataStruct> {
		if (LowerWide(ResolveAgainst(ContentDirectory, Path).generic_wstring()) == StructKey)
		{
			return OldStruct;
		}
		return LoadStruct(Path);
	};

	// 2) 사용자 파일을 옛 구조체로 읽는다
	struct FUser
	{
		std::filesystem::path Path;
		bool                  bTable = true;
		FDataTable            Table;
		FDataAsset            Asset;
	};
	std::vector<FUser> Users;
	for (const std::filesystem::path& Path : FindStructUsers(ContentDirectory, StructPath))
	{
		std::string Text;
		if (!ReadDiskText(Path, Text))
		{
			continue;
		}
		FUser User;
		User.Path   = Path;
		User.bTable = LowerExtension(Path) == FDataTable::Extension;
		const bool bOk = User.bTable ? FDataTable::FromJsonString(Text, Resolver, User.Table) : FDataAsset::FromJsonString(Text, Resolver, User.Asset);
		if (bOk)
		{
			Users.push_back(std::move(User));
		}
		else
		{
			E_LOG(LogScene, Warning, "[데이터] 마이그레이션 건너뜀 (형식 오류): {}", FStringConv::ToUtf8(Path.generic_wstring()));
		}
	}

	// 3) 구조체 저장 → 4) 사용자 파일 Rebind + 저장
	bool bOk = WriteDiskText(StructFile, NewStruct.ToJsonString(), OutError);
	if (bOk && OutChangedFiles != nullptr)
	{
		OutChangedFiles->push_back(StructFile);
	}
	if (bOk)
	{
		const auto Shared = std::make_shared<const FDataStruct>(NewStruct);
		for (FUser& User : Users)
		{
			std::string Error;
			bool        bSaved = false;
			if (User.bTable)
			{
				User.Table.Rebind(Shared, Renames);
				bSaved = User.Table.SaveToFile(User.Path, &Error);
			}
			else
			{
				User.Asset.Rebind(Shared, Renames);
				bSaved = User.Asset.SaveToFile(User.Path, &Error);
			}
			if (!bSaved)
			{
				bOk = false;
				if (OutError != nullptr)
				{
					*OutError = Error;
				}
				continue;
			}
			if (OutChangedFiles != nullptr)
			{
				OutChangedFiles->push_back(User.Path);
			}
		}
		E_LOG(LogScene, Display, "[데이터] 구조체 저장 + 마이그레이션: {} (사용 파일 {}개)", StructPath, Users.size());
	}
	Invalidate();
	return bOk;
}
