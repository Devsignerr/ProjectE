#include "ExeStamp.h"

#include "Core/CoreMinimal.h"
#include "Core/Paths.h"
#include "Core/Platform/ExecutableResources.h"
#include "Core/StringConv.h"
#include "Renderer/Image.h"

#include <fstream>
#include <iterator>

E_DEFINE_LOG_CATEGORY(LogExeStamp, Log)

namespace
{
	// 프로젝트 Icon(.ico 그대로 / 그 밖의 이미지는 여러 크기로 축소). 지정하지 않았으면 빈 목록
	bool LoadProjectIcon(const FProjectDescriptor& Descriptor, std::vector<FIconEntry>& OutIcon)
	{
		OutIcon.clear();
		if (Descriptor.Icon.empty())
		{
			E_LOG(LogExeStamp, Warning, "프로젝트에 Icon이 없어 기본 아이콘을 씁니다 (.eproject \"Icon\")");
			return true;
		}
		const std::filesystem::path IconPath = FPaths::GetProjectDirectory() / FStringConv::ToWide(Descriptor.Icon);
		std::wstring Extension = IconPath.extension().wstring();
		for (wchar_t& Character : Extension)
		{
			Character = static_cast<wchar_t>(towlower(Character));
		}

		if (Extension == L".ico")
		{
			std::ifstream            File(IconPath, std::ios::binary);
			const std::vector<uint8> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
			if (!FExecutableResources::ParseIco(Bytes, OutIcon))
			{
				E_LOG(LogExeStamp, Error, "아이콘 파일을 읽을 수 없습니다: {}", FStringConv::ToUtf8(IconPath.wstring()));
				return false;
			}
			return true;
		}

		FImage Image;
		if (!FImageLoader::LoadFromFile(IconPath, Image))
		{
			E_LOG(LogExeStamp, Error, "아이콘 이미지를 읽을 수 없습니다: {}", FStringConv::ToUtf8(IconPath.wstring()));
			return false;
		}
		if (Image.Width != Image.Height)
		{
			E_LOG(LogExeStamp, Warning, "아이콘 이미지가 정사각형이 아닙니다 ({}x{}) — 늘려서 씁니다", Image.Width, Image.Height);
		}
		OutIcon = FExecutableResources::MakeIconFromRgba(Image.Width, Image.Height, Image.Pixels.data());
		return true;
	}
} // namespace

int StampExecutable(const std::filesystem::path& ExecutablePath)
{
	if (!FPaths::HasProject())
	{
		E_LOG(LogExeStamp, Error, "--stamp-exe에는 --project가 필요합니다");
		return 1;
	}
	const FProjectDescriptor& Descriptor = FPaths::GetProjectDescriptor();

	std::vector<FIconEntry> Icon;
	if (!LoadProjectIcon(Descriptor, Icon))
	{
		return 1;
	}

	FExecutableVersionInfo Info;
	Info.ProductName      = Descriptor.GetDisplayName();
	Info.FileDescription  = Descriptor.GetDisplayName(); // 작업 관리자에 보이는 이름
	Info.CompanyName      = Descriptor.Company;
	Info.LegalCopyright   = Descriptor.Company.empty() ? std::string() : "Copyright (C) " + Descriptor.Company;
	Info.Version          = Descriptor.GetVersion();
	Info.InternalName     = Descriptor.GetExecutableName();
	Info.OriginalFilename = FStringConv::ToUtf8(ExecutablePath.filename().wstring());
	uint16 VersionParts[4] = {};
	if (!FExecutableResources::ParseVersion(Info.Version, VersionParts))
	{
		E_LOG(LogExeStamp, Error, "Version 형식이 잘못되었습니다 (\"주.부.수[.빌드]\", 각 0~65535): {}", Info.Version);
		return 1;
	}

	std::string Error;
	if (!FExecutableResources::Stamp(ExecutablePath, Icon, Info, Error))
	{
		E_LOG(LogExeStamp, Error, "실행 파일 리소스 기록 실패: {} ({})", FStringConv::ToUtf8(ExecutablePath.wstring()), Error);
		return 1;
	}
	E_LOG(LogExeStamp, Display, "실행 파일 스탬프: {} — \"{}\" {}, 아이콘 {}장", FStringConv::ToUtf8(ExecutablePath.filename().wstring()),
	      Info.ProductName, Info.Version, Icon.size());
	return 0;
}
