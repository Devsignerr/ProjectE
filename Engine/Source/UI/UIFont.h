#pragma once

#include "UI/UILayout.h"
#include "UI/UITypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 아틀라스 안 글리프 하나 (기준 크기 BasePixelHeight에서 만든 SDF)
struct FUIGlyph
{
	uint16 AtlasX  = 0; // 텍셀
	uint16 AtlasY  = 0;
	uint16 Width   = 0; // 0이면 모양 없음 (공백)
	uint16 Height  = 0;
	float  OffsetX = 0.0f; // 기준선 위 펜 위치 → 사각형 좌상단 (기준 픽셀)
	float  OffsetY = 0.0f;
	float  Advance = 0.0f;
	int32  GlyphIndex = 0;
};

// 배치된 글리프 (텍스트 블록 좌상단 기준 UI 단위, 정렬 전)
struct FUIPlacedGlyph
{
	FVector2 Position;
	FVector2 Size;
	uint16   AtlasX = 0;
	uint16   AtlasY = 0;
	uint16   AtlasWidth  = 0;
	uint16   AtlasHeight = 0;
	uint32   Line = 0;
};

struct FUITextLine
{
	float  Width      = 0.0f;
	uint32 FirstGlyph = 0;
	uint32 GlyphCount = 0;
};

struct FUITextLayout
{
	std::vector<FUIPlacedGlyph> Glyphs; // 모양 있는 글리프만
	std::vector<FUITextLine>    Lines;
	FVector2                    Size;       // 블록 크기 (가장 긴 줄 너비, 줄 수 × 줄 높이)
	float                       LineHeight = 0.0f;
};

// TTF/OTF 글꼴 하나 + 동적 SDF 아틀라스 (R8, 필요한 글자만 처음 쓸 때 추가 — 한글 전체를 굽지 않는다).
// 아틀라스가 차면 두 배로 키운다(최대 4096). 바뀔 때마다 GetAtlasVersion이 오르고 렌더러가 다시 올린다.
class FUIFont
{
public:
	static constexpr float  BasePixelHeight = 48.0f; // SDF를 만드는 기준 크기 (어센트-디센트)
	static constexpr int32  SdfPadding      = 6;     // 모양 바깥 거리 범위 (기준 픽셀)
	static constexpr uint32 InitialAtlasSize = 1024;
	static constexpr uint32 MaxAtlasSize     = 4096;

	FUIFont();
	~FUIFont();
	FUIFont(const FUIFont&)            = delete;
	FUIFont& operator=(const FUIFont&) = delete;

	bool LoadFromFile(const std::filesystem::path& Path);
	bool LoadFromMemory(std::vector<uint8> Data);
	bool IsLoaded() const { return FontInfo != nullptr; }

	// UTF-8 문자열 배치. WrapWidth > 0이면 그 너비에서 줄바꿈 (공백 기준, 한 단어가 넘치면 글자 단위)
	void     Layout(std::string_view Utf8, float FontSize, float WrapWidth, FUITextLayout& Out);
	FVector2 Measure(std::string_view Utf8, float FontSize, float WrapWidth);
	float    GetLineHeight(float FontSize) const;
	// 한 줄 배치의 글자 경계 X (코드 포인트 N개 → N+1개, 0 = 시작). 텍스트 상자 캐럿/클릭 위치용
	void     GetCaretPositions(std::string_view Utf8, float FontSize, std::vector<float>& OutPositions);
	// 글자를 미리 굽는다 (UI 인스턴스를 만들 때 — 첫 프레임에 한꺼번에 굽는 끊김 방지)
	void     Prebake(std::string_view Utf8);
	// 셰이더용: SDF 값 0~1 차이 1이 화면에서 몇 UI 단위인지 (= 2 × 패딩 × 크기 비율)
	static float GetDistanceRange(float FontSize) { return 2.0f * static_cast<float>(SdfPadding) * FontSize / BasePixelHeight; }

	const std::vector<uint8>& GetAtlasPixels() const { return AtlasPixels; }
	uint32                    GetAtlasSize() const { return AtlasSize; }
	uint32                    GetAtlasVersion() const { return AtlasVersion; }
	size_t                    GetGlyphCount() const { return Glyphs.size(); }

private:
	struct FStbFont;

	const FUIGlyph& GetGlyph(uint32 Codepoint);
	bool            AllocateAtlasRect(uint32 Width, uint32 Height, uint32& OutX, uint32& OutY);
	void            GrowAtlas();
	float           GetKerning(int32 FirstGlyphIndex, int32 SecondGlyphIndex) const;

	std::vector<uint8>                   FileData;
	std::unique_ptr<FStbFont>            FontInfo;
	float                                BaseScale = 0.0f; // 글꼴 단위 → 기준 픽셀
	float                                Ascent    = 0.0f; // 기준 픽셀
	float                                LineGap   = 0.0f; // 기준 픽셀 (어센트 - 디센트 + 줄 간격)
	std::unordered_map<uint32, FUIGlyph> Glyphs;

	std::vector<uint8> AtlasPixels;
	uint32             AtlasSize    = 0;
	uint32             AtlasVersion = 1;
	uint32             ShelfX       = 0;
	uint32             ShelfY       = 0;
	uint32             ShelfHeight  = 0;
	bool               bWarnedFull  = false;
};

// 글꼴 캐시 (경로별 하나, 프로세스 전역 — 편집기 미리보기와 게임이 아틀라스를 공유).
// 위젯 Font가 비었거나 로드에 실패하면 기본 글꼴(프로젝트 지정 → 엔진 번들 Noto Sans KR → Windows 맑은 고딕)을 쓴다.
class FUIFontLibrary final : public IUITextMeasurer
{
public:
	static FUIFontLibrary& Get();

	// 상대 경로 글꼴의 기준 폴더 (프로젝트 Content)
	void                         SetContentDirectory(const std::filesystem::path& Directory) { ContentDirectory = Directory; }
	const std::filesystem::path& GetContentDirectory() const { return ContentDirectory; }
	// 기본 글꼴 파일 (절대 경로 또는 Content 기준). 비우면 시스템 글꼴
	void SetDefaultFontPath(const std::filesystem::path& Path);

	// 실패 시 기본 글꼴, 기본 글꼴도 없으면 nullptr
	FUIFont* GetFont(std::string_view FontPath);
	FUIFont* GetDefaultFont();
	// 기본 글꼴 파일 경로 (없으면 빈 경로)
	std::filesystem::path GetDefaultFontFile();
	// 로드된 글꼴 전체 (렌더러가 아틀라스를 올릴 때)
	std::vector<FUIFont*> GetLoadedFonts() const;

	FVector2 MeasureText(const FUIWidgetData& TextWidget, float WrapWidth) override;

	// 모든 글꼴 해제 (렌더러 종료 후, 테스트)
	void Clear();

private:
	FUIFont* LoadCached(const std::filesystem::path& Path);

	std::filesystem::path                                    ContentDirectory;
	std::filesystem::path                                    DefaultFontPath;
	std::unordered_map<std::wstring, std::unique_ptr<FUIFont>> Fonts; // 키: 정규화 경로 (실패한 경로는 nullptr)
	FUIFont*                                                 DefaultFont        = nullptr;
	std::filesystem::path                                    DefaultFontFile;
	bool                                                     bDefaultFontLoaded = false;
};

// UTF-8 → 코드 포인트 (잘못된 바이트는 U+FFFD)
std::vector<uint32> DecodeUtf8(std::string_view Utf8);
// 코드 포인트 → UTF-8
std::string EncodeUtf8(const std::vector<uint32>& Codepoints);
