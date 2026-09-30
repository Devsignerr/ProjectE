#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>
#include <vector>

// 실행 파일 버전 리소스(VS_VERSIONINFO) 문자열. 빈 값은 기록하지 않는다
struct FExecutableVersionInfo
{
	std::string ProductName;
	std::string FileDescription;
	std::string CompanyName;
	std::string LegalCopyright;
	std::string Version;          // "주.부[.수[.빌드]]" — 파일/제품 버전 둘 다
	std::string OriginalFilename; // 예: "Sample.exe"
	std::string InternalName;
};

// 아이콘 한 장 (ICO 항목 = RT_ICON 리소스 하나). Data는 BMP DIB(BITMAPINFOHEADER~) 또는 PNG 바이트
struct FIconEntry
{
	uint32             Size     = 0; // 가로=세로 픽셀 (256까지)
	uint16             BitCount = 32;
	std::vector<uint8> Data;
};

// 패키징: 복사한 런타임 exe에 아이콘·버전 리소스를 써 넣는다 (프로젝트마다 exe를 다시 빌드하지 않음).
// 아이콘 그룹은 RT_GROUP_ICON ID 1 (FWindow가 창 아이콘으로 읽는다), 버전은 RT_VERSION ID 1.
class FExecutableResources
{
public:
	static constexpr uint16 IconGroupId = 1;

	// .ico 파일 바이트 → 항목 목록. 형식이 틀리면 false
	static bool ParseIco(const std::vector<uint8>& FileBytes, std::vector<FIconEntry>& OutEntries);
	// RGBA8(상단 행부터) → 256/64/48/32/16 크기 32비트 BMP 항목 (면적 평균 축소, 정사각형이 아니면 늘린다)
	static std::vector<FIconEntry> MakeIconFromRgba(uint32 Width, uint32 Height, const uint8* Rgba);

	// "1.2.3" → {1, 2, 3, 0}. 숫자가 아니거나 4개 초과, 65535 초과면 false
	static bool ParseVersion(const std::string& Text, uint16 OutParts[4]);
	// VS_VERSIONINFO 바이트 (언어 en-US, 유니코드 코드 페이지). 버전이 잘못되면 0.0.0.0
	static std::vector<uint8> BuildVersionResource(const FExecutableVersionInfo& Info);

	// 아이콘(비어 있으면 생략)·버전 리소스를 exe에 기록. 실패하면 false + OutError
	static bool Stamp(const std::filesystem::path& ExecutablePath, const std::vector<FIconEntry>& Icon,
	                  const FExecutableVersionInfo& Version, std::string& OutError);
};
