#include "Core/SaveGame.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace
{
	std::filesystem::path GDirectoryOverride; // 테스트/자동 검증 (Core DLL 안에서만 접근)

	bool IsReservedDeviceName(std::string_view Slot)
	{
		std::string Upper(Slot);
		std::transform(Upper.begin(), Upper.end(), Upper.begin(), [](char Char) { return Char >= 'a' && Char <= 'z' ? static_cast<char>(Char - 'a' + 'A') : Char; });
		static constexpr std::array<std::string_view, 4> Fixed = { "CON", "PRN", "AUX", "NUL" };
		if (std::find(Fixed.begin(), Fixed.end(), Upper) != Fixed.end())
		{
			return true;
		}
		return Upper.size() == 4 && (Upper.starts_with("COM") || Upper.starts_with("LPT")) && Upper[3] >= '0' && Upper[3] <= '9';
	}
} // namespace

bool FSaveGame::IsValidSlotName(std::string_view Slot)
{
	if (Slot.empty() || Slot.size() > MaxSlotNameLength || IsReservedDeviceName(Slot))
	{
		return false;
	}
	for (const char Char : Slot)
	{
		const unsigned char Byte = static_cast<unsigned char>(Char);
		const bool bAllowed = (Byte >= 'a' && Byte <= 'z') || (Byte >= 'A' && Byte <= 'Z') || (Byte >= '0' && Byte <= '9') || Byte == '_' || Byte == '-' ||
		                      Byte >= 0x80; // UTF-8 다바이트 문자 (한글 등)
		if (!bAllowed)
		{
			return false; // '/', '\\', '.', ':', 공백, 제어 문자 등
		}
	}
	return true;
}

std::filesystem::path FSaveGame::GetDirectory()
{
	return GDirectoryOverride.empty() ? FPaths::GetSavedDirectory() / L"SaveGames" : GDirectoryOverride;
}

void FSaveGame::SetDirectoryOverride(const std::filesystem::path& Directory)
{
	GDirectoryOverride = Directory;
}

std::filesystem::path FSaveGame::GetSlotPath(std::string_view Slot)
{
	if (!IsValidSlotName(Slot))
	{
		return {};
	}
	return GetDirectory() / (FStringConv::ToWide(std::string(Slot)) + L".json");
}

bool FSaveGame::Save(std::string_view Slot, std::string_view JsonText, std::string* OutError)
{
	const auto Fail = [OutError](std::string Message) {
		E_LOG(LogCore, Warning, "세이브 실패: {}", Message);
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	const std::filesystem::path Path = GetSlotPath(Slot);
	if (Path.empty())
	{
		return Fail(std::format("잘못된 슬롯 이름 \"{}\"", Slot));
	}
	const nlohmann::json Root = nlohmann::json::parse(JsonText, nullptr, false);
	if (Root.is_discarded())
	{
		return Fail(std::format("슬롯 \"{}\": JSON이 아닙니다", Slot));
	}

	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::filesystem::path TempPath = Path;
	TempPath += L".tmp";
	{
		std::ofstream File(TempPath, std::ios::binary | std::ios::trunc);
		if (!File)
		{
			return Fail(std::format("파일을 열 수 없습니다: {}", FStringConv::ToUtf8(TempPath.wstring())));
		}
		File << Root.dump(2);
		if (!File.good())
		{
			return Fail(std::format("쓰기 실패: {}", FStringConv::ToUtf8(TempPath.wstring())));
		}
	}
	std::filesystem::rename(TempPath, Path, ErrorCode); // 기존 세이브를 바꿔치기 (MSVC: 덮어쓰기)
	if (ErrorCode)
	{
		std::filesystem::remove(TempPath, ErrorCode);
		return Fail(std::format("슬롯 \"{}\" 파일 교체 실패", Slot));
	}
	E_LOG(LogCore, Log, "세이브: {}", FStringConv::ToUtf8(Path.wstring()));
	return true;
}

std::optional<std::string> FSaveGame::Load(std::string_view Slot)
{
	const std::filesystem::path Path = GetSlotPath(Slot);
	if (Path.empty())
	{
		return std::nullopt;
	}
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		return std::nullopt;
	}
	std::string Text((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	if (nlohmann::json::parse(Text, nullptr, false).is_discarded())
	{
		E_LOG(LogCore, Warning, "세이브 슬롯 \"{}\"이 손상되었습니다 (JSON 아님)", Slot);
		return std::nullopt;
	}
	return Text;
}

bool FSaveGame::Exists(std::string_view Slot)
{
	const std::filesystem::path Path = GetSlotPath(Slot);
	std::error_code             ErrorCode;
	return !Path.empty() && std::filesystem::is_regular_file(Path, ErrorCode);
}

bool FSaveGame::Delete(std::string_view Slot)
{
	const std::filesystem::path Path = GetSlotPath(Slot);
	std::error_code             ErrorCode;
	return !Path.empty() && std::filesystem::remove(Path, ErrorCode);
}

std::vector<std::string> FSaveGame::List()
{
	std::vector<std::string> Result;
	std::error_code          ErrorCode;
	for (std::filesystem::directory_iterator It(GetDirectory(), ErrorCode), End; !ErrorCode && It != End; It.increment(ErrorCode))
	{
		if (!It->is_regular_file(ErrorCode) || It->path().extension() != L".json")
		{
			continue;
		}
		std::string Slot = FStringConv::ToUtf8(It->path().stem().wstring());
		if (IsValidSlotName(Slot))
		{
			Result.push_back(std::move(Slot));
		}
	}
	std::sort(Result.begin(), Result.end());
	return Result;
}
