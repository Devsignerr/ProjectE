#include "PackageManifest.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Renderer/MaterialAsset.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

E_DEFINE_LOG_CATEGORY(LogPackageManifest, Log)

namespace
{
	constexpr size_t MaxReferenceLength = 1024; // 이보다 긴 문자열은 경로가 아니다 (base64 등)

	// 텍스트로 훑는 에셋 (나머지는 바이너리/참조 없음 — .eterrain은 JSON이지만 큰 base64 높이 데이터뿐)
	constexpr std::string_view TextExtensions[] = { ".escene", ".eprefab", ".emat", ".eanimgraph", ".ebt", ".eui", ".eparticle", ".esequence",
	                                                ".lua", ".etable", ".edata", ".estruct", ".efoliage", ".ebuilding", ".emeta", ".eimport",
	                                                ".gltf", ".estrings", ".json" };
	constexpr std::string_view ModelExtensions[] = { ".gltf", ".glb", ".fbx" };
	constexpr std::string_view ImageExtensions[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

	std::string ToLowerAscii(std::string_view Text)
	{
		std::string Result(Text);
		for (char& Char : Result)
		{
			if (Char >= 'A' && Char <= 'Z')
			{
				Char = static_cast<char>(Char - 'A' + 'a');
			}
		}
		return Result;
	}

	std::string GetExtensionLower(std::string_view Path)
	{
		const size_t Slash = Path.find_last_of('/');
		const size_t Dot   = Path.find_last_of('.');
		if (Dot == std::string_view::npos || (Slash != std::string_view::npos && Dot < Slash))
		{
			return {};
		}
		return ToLowerAscii(Path.substr(Dot));
	}

	template <size_t N>
	bool IsOneOf(const std::string& Extension, const std::string_view (&List)[N])
	{
		return std::find(std::begin(List), std::end(List), Extension) != std::end(List);
	}

	std::string GetDirectoryPart(const std::string& Path)
	{
		const size_t Slash = Path.find_last_of('/');
		return Slash == std::string::npos ? std::string() : Path.substr(0, Slash);
	}

	std::string JoinPath(const std::string& Directory, std::string_view Name)
	{
		return Directory.empty() ? std::string(Name) : Directory + "/" + std::string(Name);
	}

	void AppendUtf8(std::string& Out, uint32 CodePoint)
	{
		if (CodePoint < 0x80)
		{
			Out += static_cast<char>(CodePoint);
		}
		else if (CodePoint < 0x800)
		{
			Out += static_cast<char>(0xC0 | (CodePoint >> 6));
			Out += static_cast<char>(0x80 | (CodePoint & 0x3F));
		}
		else if (CodePoint < 0x10000)
		{
			Out += static_cast<char>(0xE0 | (CodePoint >> 12));
			Out += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F));
			Out += static_cast<char>(0x80 | (CodePoint & 0x3F));
		}
		else
		{
			Out += static_cast<char>(0xF0 | (CodePoint >> 18));
			Out += static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3F));
			Out += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F));
			Out += static_cast<char>(0x80 | (CodePoint & 0x3F));
		}
	}

	int32 HexValue(char Char)
	{
		if (Char >= '0' && Char <= '9') return Char - '0';
		if (Char >= 'a' && Char <= 'f') return Char - 'a' + 10;
		if (Char >= 'A' && Char <= 'F') return Char - 'A' + 10;
		return -1;
	}

	// Quote에서 시작하는 문자열을 해제한다. 끝나는 따옴표 다음 위치를 돌려준다 (닫히지 않으면 bLua는 줄 끝, JSON은 텍스트 끝)
	size_t ReadQuoted(std::string_view Text, size_t Begin, char Quote, bool bLua, std::string& Out)
	{
		size_t Index = Begin + 1;
		while (Index < Text.size())
		{
			const char Char = Text[Index];
			if (Char == Quote)
			{
				return Index + 1;
			}
			if (bLua && Char == '\n')
			{
				return Index; // 닫히지 않은 Lua 문자열 — 이 줄에서 끝
			}
			if (Char == '\\' && Index + 1 < Text.size())
			{
				const char Next = Text[Index + 1];
				Index += 2;
				switch (Next)
				{
				case 'n': Out += '\n'; break;
				case 't': Out += '\t'; break;
				case 'r': Out += '\r'; break;
				case 'b': Out += '\b'; break;
				case 'f': Out += '\f'; break;
				case 'u':
					if (Index + 4 <= Text.size())
					{
						uint32 CodePoint = 0;
						bool   bValid    = true;
						for (size_t Digit = 0; Digit < 4; ++Digit)
						{
							const int32 Value = HexValue(Text[Index + Digit]);
							bValid &= Value >= 0;
							CodePoint = CodePoint * 16 + static_cast<uint32>(std::max(Value, 0));
						}
						if (bValid)
						{
							AppendUtf8(Out, CodePoint);
							Index += 4;
						}
					}
					break;
				default: Out += Next; break; // \" \\ \/ \' 등
				}
				continue;
			}
			Out += Char;
			++Index;
		}
		return Index;
	}

	// Lua 긴 괄호 "[[", "[=[" ... 의 등호 수 (아니면 -1)
	int32 LongBracketLevel(std::string_view Text, size_t Index)
	{
		if (Index >= Text.size() || Text[Index] != '[')
		{
			return -1;
		}
		size_t Cursor = Index + 1;
		int32  Level  = 0;
		while (Cursor < Text.size() && Text[Cursor] == '=')
		{
			++Level;
			++Cursor;
		}
		return Cursor < Text.size() && Text[Cursor] == '[' ? Level : -1;
	}

	size_t FindLongBracketEnd(std::string_view Text, size_t ContentBegin, int32 Level, size_t& OutContentEnd)
	{
		const std::string Closing = "]" + std::string(static_cast<size_t>(Level), '=') + "]";
		const size_t      Found   = Text.find(Closing, ContentBegin);
		OutContentEnd             = Found == std::string_view::npos ? Text.size() : Found;
		return Found == std::string_view::npos ? Text.size() : Found + Closing.size();
	}

	void AddStringWithNested(std::vector<std::string>& Out, std::string Value)
	{
		const bool bNested = Value.find('"') != std::string::npos;
		Out.push_back(std::move(Value));
		if (bNested)
		{
			std::vector<std::string> Inner = PackageManifest::ExtractStrings(Out.back(), false);
			for (std::string& Item : Inner)
			{
				Out.push_back(std::move(Item));
			}
		}
	}

	std::string PercentDecode(std::string_view Text)
	{
		std::string Result;
		for (size_t Index = 0; Index < Text.size(); ++Index)
		{
			if (Text[Index] == '%' && Index + 2 < Text.size() && HexValue(Text[Index + 1]) >= 0 && HexValue(Text[Index + 2]) >= 0)
			{
				Result += static_cast<char>(HexValue(Text[Index + 1]) * 16 + HexValue(Text[Index + 2]));
				Index += 2;
				continue;
			}
			Result += Text[Index];
		}
		return Result;
	}

	bool ReadTextFile(const std::filesystem::path& Path, std::string& Out)
	{
		std::ifstream Stream(Path, std::ios::binary);
		if (!Stream)
		{
			return false;
		}
		std::ostringstream Buffer;
		Buffer << Stream.rdbuf();
		Out = Buffer.str();
		return true;
	}

	std::vector<std::string> SplitList(std::string_view List)
	{
		std::vector<std::string> Result;
		size_t                   Begin = 0;
		while (Begin <= List.size())
		{
			const size_t End = std::min(List.find(';', Begin), List.size());
			std::string  Item(List.substr(Begin, End - Begin));
			const size_t First = Item.find_first_not_of(" \t\r\n");
			const size_t Last  = Item.find_last_not_of(" \t\r\n");
			if (First != std::string::npos)
			{
				Result.push_back(Item.substr(First, Last - First + 1));
			}
			Begin = End + 1;
		}
		return Result;
	}
} // namespace

// ---- 순수 함수

std::vector<std::string> PackageManifest::ExtractStrings(std::string_view Text, bool bLua)
{
	std::vector<std::string> Result;
	size_t                   Index = 0;
	while (Index < Text.size())
	{
		const char Char = Text[Index];
		if (bLua && Char == '-' && Index + 1 < Text.size() && Text[Index + 1] == '-')
		{
			// 주석: --[[ ... ]] 또는 줄 끝까지
			const int32 Level = LongBracketLevel(Text, Index + 2);
			if (Level >= 0)
			{
				size_t ContentEnd = 0;
				Index             = FindLongBracketEnd(Text, Index + 2 + static_cast<size_t>(Level) + 2, Level, ContentEnd);
			}
			else
			{
				const size_t LineEnd = Text.find('\n', Index);
				Index                = LineEnd == std::string_view::npos ? Text.size() : LineEnd + 1;
			}
			continue;
		}
		if (bLua && Char == '[')
		{
			const int32 Level = LongBracketLevel(Text, Index);
			if (Level >= 0)
			{
				const size_t ContentBegin = Index + static_cast<size_t>(Level) + 2;
				size_t       ContentEnd   = 0;
				Index                     = FindLongBracketEnd(Text, ContentBegin, Level, ContentEnd);
				AddStringWithNested(Result, std::string(Text.substr(ContentBegin, ContentEnd - ContentBegin)));
				continue;
			}
		}
		if (Char == '"' || (bLua && Char == '\''))
		{
			std::string Value;
			Index = ReadQuoted(Text, Index, Char, bLua, Value);
			AddStringWithNested(Result, std::move(Value));
			continue;
		}
		++Index;
	}
	return Result;
}

std::vector<std::string> PackageManifest::SplitReferenceCandidates(std::string_view Value)
{
	std::vector<std::string> Result;
	if (Value.empty() || Value.size() > MaxReferenceLength)
	{
		return Result;
	}
	for (std::string Piece : SplitList(Value))
	{
		std::replace(Piece.begin(), Piece.end(), '\\', '/');
		if (Piece.find("://") != std::string::npos || Piece.front() == '/' || Piece.find('\n') != std::string::npos)
		{
			continue; // URL, 절대 경로, 여러 줄 텍스트
		}
		const size_t Colon = Piece.find(':');
		if (Colon == 1)
		{
			continue; // 드라이브 문자 (절대 경로)
		}
		Result.push_back(Piece);
		if (Colon != std::string::npos && Colon > 1)
		{
			Result.push_back(Piece.substr(0, Colon)); // "<모델 경로>:<클립>" → 모델 경로 (내장 "primitive:cube"는 파일이 아니라 무시됨)
		}
	}
	return Result;
}

std::string PackageManifest::NormalizeRelativePath(std::string_view Path)
{
	std::vector<std::string> Parts;
	size_t                   Begin = 0;
	while (Begin <= Path.size())
	{
		size_t End = Path.find_first_of("/\\", Begin);
		if (End == std::string_view::npos)
		{
			End = Path.size();
		}
		const std::string_view Part = Path.substr(Begin, End - Begin);
		if (Part == "..")
		{
			if (Parts.empty())
			{
				return {}; // Content 밖
			}
			Parts.pop_back();
		}
		else if (!Part.empty() && Part != ".")
		{
			Parts.emplace_back(Part);
		}
		Begin = End + 1;
	}
	std::string Result;
	for (const std::string& Part : Parts)
	{
		Result = JoinPath(Result, Part);
	}
	return Result;
}

const char* PackageManifest::GetUsageName(ETextureUsage Usage)
{
	switch (Usage)
	{
	case ETextureUsage::Color:  return "color";
	case ETextureUsage::Linear: return "linear";
	case ETextureUsage::Normal: return "normal";
	case ETextureUsage::Mask:   return "mask";
	}
	return "color";
}

bool PackageManifest::ParseUsageName(std::string_view Name, ETextureUsage& OutUsage)
{
	for (const ETextureUsage Usage : { ETextureUsage::Color, ETextureUsage::Linear, ETextureUsage::Normal, ETextureUsage::Mask })
	{
		if (Name == GetUsageName(Usage))
		{
			OutUsage = Usage;
			return true;
		}
	}
	return false;
}

// ---- 매니페스트 파일

std::string FPackageManifest::ToText() const
{
	std::string Text = "# ProjectE 패키지 매니페스트 (ProjectECook --package-manifest, 규칙은 Tools/Cook/Source/PackageManifest.h 머리 주석)\n";
	Text += std::format("# 파일 {}, 모델 {}, 단독 이미지 {}, 모델 내부 {}, 폴더 참조 {}\n", Files.size(), Models.size(), Images.size(), Internal.size(),
	                    Directories.size());
	for (const std::string& Warning : Warnings)
	{
		Text += "W\t" + Warning + "\n";
	}
	for (const std::string& Directory : Directories)
	{
		Text += "D\t" + Directory + "\n";
	}
	for (const std::string& File : Files)
	{
		Text += "F\t" + File + "\n";
	}
	for (const std::string& Model : Models)
	{
		Text += "M\t" + Model + "\n";
	}
	for (const auto& [Image, Usages] : Images)
	{
		std::string Names;
		for (const ETextureUsage Usage : Usages)
		{
			Names += (Names.empty() ? "" : ",") + std::string(PackageManifest::GetUsageName(Usage));
		}
		Text += "T\t" + Image + "\t" + Names + "\n";
	}
	for (const std::string& File : Internal)
	{
		Text += "I\t" + File + "\n";
	}
	return Text;
}

bool FPackageManifest::WriteToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	}
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	const std::string Text = ToText();
	Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
	return static_cast<bool>(Stream);
}

bool FPackageManifest::ParseText(std::string_view Text, FPackageManifest& OutManifest)
{
	OutManifest = {};
	size_t Begin = 0;
	while (Begin < Text.size())
	{
		size_t End = Text.find('\n', Begin);
		if (End == std::string_view::npos)
		{
			End = Text.size();
		}
		std::string_view Line = Text.substr(Begin, End - Begin);
		Begin                 = End + 1;
		if (!Line.empty() && Line.back() == '\r')
		{
			Line.remove_suffix(1);
		}
		if (Line.empty() || Line.front() == '#')
		{
			continue;
		}
		const size_t Tab = Line.find('\t');
		if (Tab != 1)
		{
			return false;
		}
		const char       Kind  = Line.front();
		std::string_view Rest  = Line.substr(2);
		const size_t     Tab2  = Rest.find('\t');
		const std::string Path(Rest.substr(0, Tab2));
		switch (Kind)
		{
		case 'F': OutManifest.Files.insert(Path); break;
		case 'M': OutManifest.Models.insert(Path); break;
		case 'I': OutManifest.Internal.insert(Path); break;
		case 'D': OutManifest.Directories.insert(Path); break;
		case 'W': OutManifest.Warnings.push_back(std::string(Rest)); break;
		case 'T':
		{
			std::set<ETextureUsage>& Usages = OutManifest.Images[Path];
			if (Tab2 != std::string_view::npos)
			{
				std::string List(Rest.substr(Tab2 + 1));
				std::replace(List.begin(), List.end(), ',', ';');
				for (const std::string& Name : SplitList(List))
				{
					ETextureUsage Usage = ETextureUsage::Color;
					if (!PackageManifest::ParseUsageName(Name, Usage))
					{
						return false;
					}
					Usages.insert(Usage);
				}
			}
			if (Usages.empty())
			{
				Usages.insert(ETextureUsage::Color);
			}
			break;
		}
		default: return false;
		}
	}
	return true;
}

bool FPackageManifest::ReadFromFile(const std::filesystem::path& Path, FPackageManifest& OutManifest)
{
	std::string Text;
	return ReadTextFile(Path, Text) && ParseText(Text, OutManifest);
}

// ---- 스캐너

FPackageDependencyScanner::FPackageDependencyScanner(const std::filesystem::path& ContentDirectory)
	: Content(ContentDirectory)
{
	std::error_code ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(Content, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
	     It.increment(ErrorCode))
	{
		const std::string Relative = FStringConv::ToUtf8(std::filesystem::relative(It->path(), Content, ErrorCode).generic_wstring());
		if (Relative.empty())
		{
			continue;
		}
		if (It->is_directory(ErrorCode))
		{
			DirectoryIndex[ToLowerAscii(Relative)] = Relative;
		}
		else if (It->is_regular_file(ErrorCode))
		{
			FileIndex[ToLowerAscii(Relative)] = Relative;
		}
	}
}

const std::string* FPackageDependencyScanner::FindFile(const std::string& NormalizedLower) const
{
	const auto Found = FileIndex.find(NormalizedLower);
	return Found == FileIndex.end() ? nullptr : &Found->second;
}

const std::string* FPackageDependencyScanner::FindDirectory(const std::string& NormalizedLower) const
{
	const auto Found = DirectoryIndex.find(NormalizedLower);
	return Found == DirectoryIndex.end() ? nullptr : &Found->second;
}

bool FPackageDependencyScanner::AddRoot(std::string_view ContentRelative, bool bRequired, std::string_view Label)
{
	const std::string Normalized = ToLowerAscii(PackageManifest::NormalizeRelativePath(ContentRelative));
	if (!Normalized.empty())
	{
		if (const std::string* File = FindFile(Normalized))
		{
			AddFile(*File, EContext::Package, true);
			return true;
		}
		if (const std::string* Directory = FindDirectory(Normalized))
		{
			AddDirectory(*Directory);
			return true;
		}
	}
	const std::string Message = std::format("{}: Content에 없는 경로 \"{}\"", Label, ContentRelative);
	Result.Warnings.push_back(Message);
	Result.bHasErrors |= bRequired;
	E_LOG(LogPackageManifest, Warning, "{}", Message);
	return false;
}

void FPackageDependencyScanner::AddRootsFromJsonFile(const std::filesystem::path& JsonFile)
{
	std::string Text;
	if (!ReadTextFile(JsonFile, Text))
	{
		return;
	}
	for (const std::string& Value : PackageManifest::ExtractStrings(Text, false))
	{
		ResolveValue(Value, std::string(), EContext::Package, false);
	}
}

void FPackageDependencyScanner::AddDirectory(const std::string& Directory)
{
	if (!Result.Directories.insert(Directory).second)
	{
		return;
	}
	const std::string Prefix = ToLowerAscii(Directory) + "/";
	for (auto It = FileIndex.lower_bound(Prefix); It != FileIndex.end() && It->first.compare(0, Prefix.size(), Prefix) == 0; ++It)
	{
		AddFile(It->second, EContext::Package, true);
	}
}

void FPackageDependencyScanner::AddFile(const std::string& Path, EContext Context, bool bColorUse)
{
	const std::string Extension = GetExtensionLower(Path);
	const bool        bImage    = IsOneOf(Extension, ImageExtensions);
	if (Context == EContext::ModelSource)
	{
		if (!Shipped.contains(Path) && InternalFiles.insert(Path).second)
		{
			Queue.push_back({ Path, EContext::ModelSource });
		}
		return;
	}

	if (bImage)
	{
		std::set<ETextureUsage>& Usages = Result.Images[Path];
		if (bColorUse)
		{
			Usages.insert(ETextureUsage::Color);
		}
	}
	if (!Shipped.insert(Path).second)
	{
		return;
	}
	Queue.push_back({ Path, EContext::Package });

	if (IsOneOf(Extension, ModelExtensions))
	{
		Result.Models.insert(Path);
		for (const char* Sidecar : { ".emeta", ".eimport" })
		{
			if (const std::string* File = FindFile(ToLowerAscii(Path + Sidecar)))
			{
				AddFile(*File, EContext::Package, true);
			}
		}
	}
	else if (Extension == ".escene")
	{
		const std::string NavMesh = Path.substr(0, Path.size() - Extension.size()) + ".enav";
		if (const std::string* File = FindFile(ToLowerAscii(NavMesh)))
		{
			AddFile(*File, EContext::Package, true);
		}
	}
}

void FPackageDependencyScanner::ResolveValue(std::string_view Value, const std::string& ReferrerDirectory, EContext Context, bool bFromMaterial)
{
	for (const std::string& Candidate : PackageManifest::SplitReferenceCandidates(Value))
	{
		std::vector<std::string> Variants = { Candidate };
		if (Context == EContext::ModelSource && Candidate.find('%') != std::string::npos)
		{
			Variants.push_back(PercentDecode(Candidate)); // glTF uri
		}
		for (const std::string& Variant : Variants)
		{
			// ① Content 기준 ② 참조한 파일 폴더 기준
			const std::string FromRoot = ToLowerAscii(PackageManifest::NormalizeRelativePath(Variant));
			if (!FromRoot.empty())
			{
				if (const std::string* File = FindFile(FromRoot))
				{
					AddFile(*File, Context, !bFromMaterial);
					continue;
				}
			}
			if (!ReferrerDirectory.empty())
			{
				const std::string FromFile = ToLowerAscii(PackageManifest::NormalizeRelativePath(JoinPath(ReferrerDirectory, Variant)));
				if (const std::string* File = FromFile.empty() ? nullptr : FindFile(FromFile))
				{
					AddFile(*File, Context, !bFromMaterial);
					continue;
				}
			}
			// 폴더 참조 (동적 경로 — Content 기준, 경로 구분자가 있어야: JSON 키 "Asset" 같은 단어는 제외)
			if (Context == EContext::Package && !FromRoot.empty() && Variant.find('/') != std::string::npos)
			{
				if (const std::string* Directory = FindDirectory(FromRoot))
				{
					AddDirectory(*Directory);
				}
			}
		}
	}
}

void FPackageDependencyScanner::ScanMaterialUsages(const std::string& Path)
{
	// 부모 체인을 해석한 결과로 본다: 인스턴스의 텍스처 파라미터는 Usage를 적지 않고 부모 선언을 따른다 (텍스처는 이 파일 폴더 기준으로 다시 씀)
	const std::filesystem::path FilePath = Content / FStringConv::ToWide(Path);
	FMaterialAsset              Asset;
	FMaterialAsset              Material;
	const auto Loader = [](const std::filesystem::path& ParentPath, FMaterialAsset& OutAsset) { return OutAsset.LoadFromFile(ParentPath); };
	if (!Asset.LoadFromFile(FilePath))
	{
		Result.Warnings.push_back(std::format("머티리얼을 읽지 못함: {}", Path));
		return;
	}
	if (!FMaterialAsset::Resolve(Asset, FilePath, Loader, Material))
	{
		Material = Asset; // 끊긴 체인: 자기 값만 (런타임도 읽은 데까지)
	}
	const std::string Directory = GetDirectoryPart(Path);
	const auto        AddUsage  = [&](const std::string& Texture, ETextureUsage Usage)
	{
		const std::string  Normalized = ToLowerAscii(PackageManifest::NormalizeRelativePath(JoinPath(Directory, Texture)));
		const std::string* File       = Normalized.empty() ? nullptr : FindFile(Normalized);
		if (File == nullptr)
		{
			Result.Warnings.push_back(std::format("머티리얼 텍스처 없음: {} → {}", Path, Texture));
			return;
		}
		AddFile(*File, EContext::Package, false);
		if (const auto Found = Result.Images.find(*File); Found != Result.Images.end())
		{
			Found->second.insert(Usage);
		}
	};
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		if (!Material.TexturePaths[Slot].empty())
		{
			AddUsage(Material.TexturePaths[Slot], FMaterialAsset::GetSlotUsage(Slot));
		}
	}
	for (const FMaterialParameter& Parameter : Material.Parameters)
	{
		if (Parameter.Type == EMaterialParameterType::Texture && !Parameter.Texture.empty())
		{
			AddUsage(Parameter.Texture, Parameter.Usage);
		}
	}
}

void FPackageDependencyScanner::ScanFile(const std::string& Path, EContext Context)
{
	const std::string Extension = GetExtensionLower(Path);
	if (!IsOneOf(Extension, TextExtensions))
	{
		return;
	}
	if (Context == EContext::ModelSource && !ScannedInternal.insert(Path).second)
	{
		return;
	}
	std::string Text;
	if (!ReadTextFile(Content / FStringConv::ToWide(Path), Text))
	{
		Result.Warnings.push_back(std::format("읽지 못함: {}", Path));
		return;
	}
	// 모델 원본(glTF uri, .eimport 추가 입력)이 가리키는 파일은 쿠킹 입력
	const EContext RefContext = (Extension == ".gltf" || Extension == ".eimport") ? EContext::ModelSource : Context;
	const bool     bMaterial  = Extension == ".emat";
	const std::string Directory = GetDirectoryPart(Path);
	for (const std::string& Value : PackageManifest::ExtractStrings(Text, Extension == ".lua"))
	{
		ResolveValue(Value, Directory, RefContext, bMaterial);
	}
	if (bMaterial && Context == EContext::Package)
	{
		ScanMaterialUsages(Path);
	}
}

FPackageManifest FPackageDependencyScanner::Run()
{
	while (!Queue.empty())
	{
		const FPending Pending = std::move(Queue.back());
		Queue.pop_back();
		ScanFile(Pending.Path, Pending.Context);
	}

	Result.Files = Shipped;
	for (const std::string& File : InternalFiles)
	{
		if (!Shipped.contains(File))
		{
			Result.Internal.insert(File);
		}
	}
	for (auto& [Image, Usages] : Result.Images)
	{
		if (Usages.empty())
		{
			Usages.insert(ETextureUsage::Color);
		}
	}
	return Result;
}

FPackageManifest BuildProjectPackageManifest()
{
	FPackageDependencyScanner Scanner(FPaths::GetProjectContentDirectory());
	const FProjectSettings&   Settings = FProjectSettings::Get();

	Scanner.AddRoot(Settings.Maps.GameDefaultMap, true, "게임 기본 맵");
	if (const std::string Server = Settings.GetServerDefaultMap(); !Server.empty() && Server != Settings.Maps.GameDefaultMap)
	{
		Scanner.AddRoot(Server, true, "서버 기본 맵");
	}
	if (!Settings.Maps.PlayerPrefab.empty())
	{
		Scanner.AddRoot(Settings.Maps.PlayerPrefab, true, "플레이어 프리팹");
	}
	if (!Settings.Localization.StringTables.empty())
	{
		for (const std::string& Table : SplitList(Settings.Localization.StringTables))
		{
			Scanner.AddRoot(Table, true, "문자열 표");
		}
	}
	else
	{
		// 런타임 규칙과 같다: 비면 Content/Localization/*.estrings
		std::error_code ErrorCode;
		for (const auto& Entry : std::filesystem::directory_iterator(FPaths::GetProjectContentDirectory() / L"Localization", ErrorCode))
		{
			if (Entry.is_regular_file(ErrorCode) && Entry.path().extension() == L".estrings")
			{
				Scanner.AddRoot(FStringConv::ToUtf8((std::filesystem::path(L"Localization") / Entry.path().filename()).generic_wstring()), true, "문자열 표");
			}
		}
	}
	for (const std::string& Extra : SplitList(Settings.Packaging.AdditionalAssets))
	{
		Scanner.AddRoot(Extra, true, "추가 에셋(Packaging.AdditionalAssets)");
	}

	// 나머지 프로젝트 설정 파일의 문자열 (위에서 따로 다룬 섹션 제외 — Maps의 에디터 시작 맵은 런타임 루트가 아니다)
	std::error_code ErrorCode;
	for (const auto& Entry : std::filesystem::directory_iterator(FPaths::GetProjectConfigDirectory(), ErrorCode))
	{
		const std::wstring Name = Entry.path().filename().wstring();
		if (Entry.is_regular_file(ErrorCode) && Entry.path().extension() == L".json" && Name != L"Maps.json" && Name != L"Packaging.json" &&
		    Name != L"Localization.json")
		{
			Scanner.AddRootsFromJsonFile(Entry.path());
		}
	}
	return Scanner.Run();
}
