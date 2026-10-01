#include "Editor/ContentBrowser/AssetReferenceUpdater.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <json.hpp>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	using nlohmann::json;

	enum class EReferenceBase : uint8
	{
		None,
		ContentRoot, // .escene, .eproject, .eprefab, .eui, .eanimgraph (미리보기 모델)
		OwnFolder,   // .emat, .eparticle
	};

	std::wstring Lower(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}

	EReferenceBase GetReferenceBase(const std::filesystem::path& File)
	{
		const std::wstring Extension = Lower(File.extension().wstring());
		if (Extension == L".escene" || Extension == L".eproject" || Extension == L".eprefab" || Extension == L".eui" ||
		    Extension == L".eanimgraph")
		{
			return EReferenceBase::ContentRoot;
		}
		if (Extension == L".emat" || Extension == L".eparticle" || Extension == L".eimport")
		{
			return EReferenceBase::OwnFolder;
		}
		return EReferenceBase::None;
	}

	// 비교용 정규화: 절대 + 어휘 정규화 + 소문자 + '/' (Windows 경로는 대소문자 무시)
	std::wstring MakeKey(const std::filesystem::path& Path)
	{
		return Lower(Path.lexically_normal().generic_wstring());
	}

	bool IsSameOrUnder(const std::filesystem::path& Path, const std::filesystem::path& Root)
	{
		const std::wstring PathKey = MakeKey(Path);
		std::wstring       RootKey = MakeKey(Root);
		if (PathKey == RootKey)
		{
			return true;
		}
		if (!RootKey.empty() && RootKey.back() != L'/')
		{
			RootKey.push_back(L'/');
		}
		return PathKey.rfind(RootKey, 0) == 0;
	}

	// 확장자가 있는 문자열만 경로 후보 (primitive:cube, 엔티티 이름 등 제외)
	bool LooksLikeFilePath(const std::string& Value)
	{
		if (Value.empty() || Value.rfind("primitive:", 0) == 0 || Value.find("://") != std::string::npos)
		{
			return false;
		}
		const std::filesystem::path Path = FStringConv::ToWide(Value);
		return Path.has_extension() && Path.extension().wstring().size() > 1;
	}

	std::string ToReferenceString(const std::filesystem::path& Target, const std::filesystem::path& Base, bool bAbsolute)
	{
		if (bAbsolute)
		{
			return FStringConv::ToUtf8(Target.lexically_normal().wstring());
		}
		return FStringConv::ToUtf8(Target.lexically_normal().lexically_relative(Base.lexically_normal()).generic_wstring());
	}

	// 문자열 값 안에 든 JSON (스크립트 PropertyOverrides 등). 객체/배열이 아니면 discarded
	json ParseEmbeddedJson(const std::string& Value)
	{
		if (Value.empty() || (Value.front() != '{' && Value.front() != '['))
		{
			return json(json::value_t::discarded);
		}
		return json::parse(Value, nullptr, false);
	}

	void CollectStrings(const json& Node, std::vector<std::string>& Out)
	{
		if (Node.is_string())
		{
			const std::string& Value    = Node.get_ref<const std::string&>();
			const json         Embedded = ParseEmbeddedJson(Value);
			if (!Embedded.is_discarded())
			{
				CollectStrings(Embedded, Out); // JSON 안의 JSON 문자열도 경로 후보
				return;
			}
			Out.push_back(Value);
		}
		else if (Node.is_object() || Node.is_array())
		{
			for (const json& Child : Node)
			{
				CollectStrings(Child, Out);
			}
		}
	}

	// Text 안의 문자열 토큰 Old → New. JSON 안 JSON 문자열에 이스케이프되어 든 형태(\"...\")도 바꾼다. 반환: 바꾼 개수
	uint32 ReplaceStringToken(std::string& Text, const std::string& Old, const std::string& New)
	{
		uint32 Count = 0;
		for (int32 Level = 0; Level < 2; ++Level)
		{
			std::string OldToken = json(Old).dump();
			std::string NewToken = json(New).dump();
			if (Level == 1)
			{
				// 한 번 더 문자열로 감싼 뒤 바깥 따옴표를 뗀다: "a" → \"a\"
				OldToken = json(OldToken).dump();
				NewToken = json(NewToken).dump();
				OldToken = OldToken.substr(1, OldToken.size() - 2);
				NewToken = NewToken.substr(1, NewToken.size() - 2);
			}
			for (size_t Position = Text.find(OldToken); Position != std::string::npos; Position = Text.find(OldToken, Position + NewToken.size()))
			{
				Text.replace(Position, OldToken.size(), NewToken);
				++Count;
			}
		}
		return Count;
	}

	bool ReadFile(const std::filesystem::path& Path, std::string& Out)
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

	// 참조 파일 목록: Content 아래 .escene/.emat/.eparticle + 프로젝트 파일
	std::vector<std::filesystem::path> CollectReferenceFiles(const std::filesystem::path& ContentDirectory, const std::filesystem::path& ProjectFile)
	{
		std::vector<std::filesystem::path> Files;
		std::error_code                    ErrorCode;
		for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode);
		     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
		{
			if (It->is_regular_file(ErrorCode) && GetReferenceBase(It->path()) != EReferenceBase::None)
			{
				Files.push_back(It->path());
			}
		}
		if (!ProjectFile.empty() && std::filesystem::exists(ProjectFile, ErrorCode))
		{
			Files.push_back(ProjectFile);
		}
		return Files;
	}

	// 파일 안 참조 문자열마다 Visit(원래 값, 옛 절대 대상, 기준 폴더, 절대 경로 여부)
	void ForEachReference(const std::filesystem::path& File, const std::filesystem::path& OriginalFile, const std::filesystem::path& ContentDirectory,
	                      const std::string& Text,
	                      const std::function<void(const std::string&, const std::filesystem::path&, bool)>& Visit)
	{
		const json Document = json::parse(Text, nullptr, false, true);
		if (Document.is_discarded())
		{
			return;
		}
		std::vector<std::string> Strings;
		CollectStrings(Document, Strings);
		std::sort(Strings.begin(), Strings.end());
		Strings.erase(std::unique(Strings.begin(), Strings.end()), Strings.end());

		// 기준 폴더는 "이동 전" 위치 기준으로 해석해야 옛 대상을 알 수 있다
		const EReferenceBase        Base    = GetReferenceBase(File);
		const std::filesystem::path OldBase = Base == EReferenceBase::OwnFolder ? OriginalFile.parent_path() : ContentDirectory;
		for (const std::string& Value : Strings)
		{
			if (!LooksLikeFilePath(Value))
			{
				continue;
			}
			const std::filesystem::path Relative  = FStringConv::ToWide(Value);
			const bool                  bAbsolute = Relative.is_absolute();
			Visit(Value, (bAbsolute ? Relative : OldBase / Relative).lexically_normal(), bAbsolute);
		}
	}
} // namespace

std::optional<std::filesystem::path> FAssetReferenceUpdater::MapPath(const std::filesystem::path& Path, const std::vector<FAssetMove>& Moves)
{
	for (const FAssetMove& Move : Moves)
	{
		if (IsSameOrUnder(Path, Move.From))
		{
			const std::filesystem::path Rest = Path.lexically_normal().lexically_relative(Move.From.lexically_normal());
			return (Rest.empty() || Rest == L"." ? Move.To : Move.To / Rest).lexically_normal();
		}
	}
	return std::nullopt;
}

std::optional<std::string> FAssetReferenceUpdater::RemapContentPath(const std::string& Value, const std::filesystem::path& ContentDirectory,
                                                                    const std::vector<FAssetMove>& Moves)
{
	// JSON이 든 문자열 (스크립트 오버라이드): 안쪽 문자열마다 다시 계산해 토큰 치환
	if (const json Embedded = ParseEmbeddedJson(Value); !Embedded.is_discarded())
	{
		std::vector<std::string> Strings;
		CollectStrings(Embedded, Strings);
		std::string Result = Value;
		uint32      Count  = 0;
		for (const std::string& Inner : Strings)
		{
			if (const std::optional<std::string> NewInner = RemapContentPath(Inner, ContentDirectory, Moves); NewInner && *NewInner != Inner)
			{
				Count += ReplaceStringToken(Result, Inner, *NewInner);
			}
		}
		return Count > 0 ? std::optional<std::string>(Result) : std::nullopt;
	}
	if (!LooksLikeFilePath(Value))
	{
		return std::nullopt;
	}
	const std::filesystem::path Relative  = FStringConv::ToWide(Value);
	const bool                  bAbsolute = Relative.is_absolute();
	const std::optional<std::filesystem::path> NewTarget = MapPath(bAbsolute ? Relative : ContentDirectory / Relative, Moves);
	if (!NewTarget)
	{
		return std::nullopt;
	}
	return ToReferenceString(*NewTarget, ContentDirectory, bAbsolute);
}

FAssetReferenceUpdater::FResult FAssetReferenceUpdater::UpdateAfterMove(const std::filesystem::path& ContentDirectory,
                                                                         const std::filesystem::path& ProjectFile,
                                                                         const std::vector<FAssetMove>& Moves)
{
	FResult Result;
	// 이동 후 위치 → 이동 전 위치 (역방향)
	std::vector<FAssetMove> Reverse;
	for (const FAssetMove& Move : Moves)
	{
		Reverse.push_back({ Move.To, Move.From });
	}

	for (const std::filesystem::path& File : CollectReferenceFiles(ContentDirectory, ProjectFile))
	{
		std::string Text;
		if (!ReadFile(File, Text))
		{
			continue;
		}
		const std::filesystem::path OriginalFile = MapPath(File, Reverse).value_or(File);
		const bool                  bOwnFolder   = GetReferenceBase(File) == EReferenceBase::OwnFolder;
		const std::filesystem::path NewBase      = bOwnFolder ? File.parent_path() : ContentDirectory;
		const bool                  bFileMoved   = MakeKey(OriginalFile) != MakeKey(File);

		std::vector<std::pair<std::string, std::string>> Replacements;
		ForEachReference(File, OriginalFile, ContentDirectory, Text, [&](const std::string& Value, const std::filesystem::path& OldTarget, bool bAbsolute) {
			const std::optional<std::filesystem::path> Mapped = MapPath(OldTarget, Moves);
			// 대상이 옮겨졌거나, 파일 기준 상대 경로인데 파일 자신이 옮겨졌으면 다시 계산
			if (!Mapped && !(bOwnFolder && bFileMoved && !bAbsolute))
			{
				return;
			}
			const std::string NewValue = ToReferenceString(Mapped.value_or(OldTarget), NewBase, bAbsolute);
			if (NewValue != Value)
			{
				Replacements.emplace_back(Value, NewValue);
			}
		});
		if (Replacements.empty())
		{
			continue;
		}

		// JSON 문자열 토큰 단위 치환 ("값" 전체가 같은 것만)
		for (const auto& [Old, New] : Replacements)
		{
			Result.ReplacedCount += ReplaceStringToken(Text, Old, New);
		}
		std::ofstream Out(File, std::ios::binary | std::ios::trunc);
		if (!Out)
		{
			E_LOG(LogEditor, Error, "참조 갱신 실패 (쓰기 불가): {}", FStringConv::ToUtf8(File.wstring()));
			continue;
		}
		Out << Text;
		Result.ChangedFiles.push_back(File);
	}
	return Result;
}

std::vector<std::filesystem::path> FAssetReferenceUpdater::FindReferencingFiles(const std::filesystem::path& ContentDirectory,
                                                                                const std::filesystem::path& ProjectFile,
                                                                                const std::vector<std::filesystem::path>& Targets)
{
	std::vector<FAssetMove> AsMoves; // 대상 판정만 쓰므로 To는 의미 없음
	for (const std::filesystem::path& Target : Targets)
	{
		AsMoves.push_back({ Target, Target });
	}

	std::vector<std::filesystem::path> Referencing;
	for (const std::filesystem::path& File : CollectReferenceFiles(ContentDirectory, ProjectFile))
	{
		if (MapPath(File, AsMoves))
		{
			continue; // 삭제 대상 자신은 제외
		}
		std::string Text;
		if (!ReadFile(File, Text))
		{
			continue;
		}
		bool bReferences = false;
		ForEachReference(File, File, ContentDirectory, Text, [&](const std::string&, const std::filesystem::path& Target, bool) {
			bReferences = bReferences || MapPath(Target, AsMoves).has_value();
		});
		if (bReferences)
		{
			Referencing.push_back(File);
		}
	}
	return Referencing;
}

uint32 FAssetReferenceUpdater::RemapSceneJson(std::string& Json, const std::filesystem::path& ContentDirectory, const std::vector<FAssetMove>& Moves)
{
	const json Document = json::parse(Json, nullptr, false, true);
	if (Document.is_discarded())
	{
		return 0;
	}
	std::vector<std::string> Strings;
	CollectStrings(Document, Strings);
	std::sort(Strings.begin(), Strings.end());
	Strings.erase(std::unique(Strings.begin(), Strings.end()), Strings.end());

	uint32 Count = 0;
	for (const std::string& Value : Strings)
	{
		const std::optional<std::string> NewValue = RemapContentPath(Value, ContentDirectory, Moves);
		if (!NewValue || *NewValue == Value)
		{
			continue;
		}
		Count += ReplaceStringToken(Json, Value, *NewValue);
	}
	return Count;
}
