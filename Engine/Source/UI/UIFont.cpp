#include "UI/UIFont.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "UI/UISdf.h"
#include "UI/Widget.h"

#pragma warning(push, 0)
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC // imgui도 같은 구현을 정적으로 포함하므로 심볼을 내보내지 않는다
#include <imstb_truetype.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>

E_DEFINE_LOG_CATEGORY(LogUI, Log)

struct FUIFont::FStbFont
{
	stbtt_fontinfo Info{};
};

namespace
{
	constexpr uint32 GAtlasSpacing = 1; // 글리프 사이 여백 (선형 필터 번짐 방지)

	bool IsSpace(uint32 Codepoint) { return Codepoint == ' ' || Codepoint == '\t' || Codepoint == 0x3000; }

	std::filesystem::path GetSystemFontPath()
	{
		wchar_t* WinDir = nullptr;
		size_t   Length = 0;
		if (_wdupenv_s(&WinDir, &Length, L"WINDIR") != 0 || WinDir == nullptr)
		{
			return L"C:\\Windows\\Fonts\\malgun.ttf";
		}
		std::filesystem::path Path = std::filesystem::path(WinDir) / L"Fonts" / L"malgun.ttf";
		std::free(WinDir);
		return Path;
	}

	std::wstring MakeKey(const std::filesystem::path& Path)
	{
		std::wstring Key = Path.lexically_normal().generic_wstring();
		std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Key;
	}
} // namespace

std::vector<uint32> DecodeUtf8(std::string_view Utf8)
{
	std::vector<uint32> Result;
	Result.reserve(Utf8.size());
	size_t Index = 0;
	while (Index < Utf8.size())
	{
		const uint8 Lead = static_cast<uint8>(Utf8[Index]);
		uint32      Codepoint;
		size_t      Extra;
		if (Lead < 0x80)
		{
			Codepoint = Lead;
			Extra     = 0;
		}
		else if ((Lead & 0xE0) == 0xC0)
		{
			Codepoint = Lead & 0x1F;
			Extra     = 1;
		}
		else if ((Lead & 0xF0) == 0xE0)
		{
			Codepoint = Lead & 0x0F;
			Extra     = 2;
		}
		else if ((Lead & 0xF8) == 0xF0)
		{
			Codepoint = Lead & 0x07;
			Extra     = 3;
		}
		else
		{
			Result.push_back(0xFFFD);
			++Index;
			continue;
		}
		if (Index + Extra >= Utf8.size())
		{
			// 끝에서 잘린 다중 바이트
			Result.push_back(0xFFFD);
			break;
		}
		bool bValid = true;
		for (size_t Offset = 1; Offset <= Extra; ++Offset)
		{
			const uint8 Next = static_cast<uint8>(Utf8[Index + Offset]);
			if ((Next & 0xC0) != 0x80)
			{
				bValid = false;
				break;
			}
			Codepoint = (Codepoint << 6) | (Next & 0x3F);
		}
		if (!bValid)
		{
			Result.push_back(0xFFFD);
			++Index;
			continue;
		}
		Result.push_back(Codepoint);
		Index += Extra + 1;
	}
	return Result;
}

std::string EncodeUtf8(const std::vector<uint32>& Codepoints)
{
	std::string Result;
	Result.reserve(Codepoints.size());
	for (const uint32 Codepoint : Codepoints)
	{
		if (Codepoint < 0x80)
		{
			Result.push_back(static_cast<char>(Codepoint));
		}
		else if (Codepoint < 0x800)
		{
			Result.push_back(static_cast<char>(0xC0 | (Codepoint >> 6)));
			Result.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
		}
		else if (Codepoint < 0x10000)
		{
			Result.push_back(static_cast<char>(0xE0 | (Codepoint >> 12)));
			Result.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F)));
			Result.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
		}
		else
		{
			Result.push_back(static_cast<char>(0xF0 | (Codepoint >> 18)));
			Result.push_back(static_cast<char>(0x80 | ((Codepoint >> 12) & 0x3F)));
			Result.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F)));
			Result.push_back(static_cast<char>(0x80 | (Codepoint & 0x3F)));
		}
	}
	return Result;
}

// ---------------------------------------------------------------- FUIFont

FUIFont::FUIFont()  = default;
FUIFont::~FUIFont() = default;

bool FUIFont::LoadFromFile(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		return false;
	}
	// 한 번에 읽는다 (istreambuf_iterator는 Debug에서 13MB 한글 글꼴에 1초 가까이 걸린다)
	File.seekg(0, std::ios::end);
	const std::streamoff Size = File.tellg();
	File.seekg(0, std::ios::beg);
	std::vector<uint8> Data(Size > 0 ? static_cast<size_t>(Size) : 0);
	if (Size <= 0 || !File.read(reinterpret_cast<char*>(Data.data()), Size))
	{
		return false;
	}
	if (!LoadFromMemory(std::move(Data)))
	{
		E_LOG(LogUI, Error, "글꼴을 읽지 못했습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}

bool FUIFont::LoadFromMemory(std::vector<uint8> Data)
{
	FileData = std::move(Data);
	auto Font = std::make_unique<FStbFont>();
	const int32 Offset = FileData.empty() ? -1 : stbtt_GetFontOffsetForIndex(FileData.data(), 0);
	if (Offset < 0 || !stbtt_InitFont(&Font->Info, FileData.data(), Offset))
	{
		FileData.clear();
		return false;
	}
	FontInfo = std::move(Font);

	int32 FontAscent  = 0;
	int32 FontDescent = 0;
	int32 FontLineGap = 0;
	stbtt_GetFontVMetrics(&FontInfo->Info, &FontAscent, &FontDescent, &FontLineGap);
	BaseScale = stbtt_ScaleForPixelHeight(&FontInfo->Info, BasePixelHeight);
	Ascent    = static_cast<float>(FontAscent) * BaseScale;
	LineGap   = static_cast<float>(FontAscent - FontDescent + FontLineGap) * BaseScale;

	Glyphs.clear();
	AtlasSize = InitialAtlasSize;
	AtlasPixels.assign(static_cast<size_t>(AtlasSize) * AtlasSize, 0);
	ShelfX      = GAtlasSpacing;
	ShelfY      = GAtlasSpacing;
	ShelfHeight = 0;
	++AtlasVersion;
	return true;
}

float FUIFont::GetLineHeight(float FontSize) const
{
	return LineGap * FontSize / BasePixelHeight;
}

float FUIFont::GetKerning(int32 FirstGlyphIndex, int32 SecondGlyphIndex) const
{
	if (FirstGlyphIndex == 0 || SecondGlyphIndex == 0)
	{
		return 0.0f;
	}
	return static_cast<float>(stbtt_GetGlyphKernAdvance(&FontInfo->Info, FirstGlyphIndex, SecondGlyphIndex)) * BaseScale;
}

bool FUIFont::AllocateAtlasRect(uint32 Width, uint32 Height, uint32& OutX, uint32& OutY)
{
	for (;;)
	{
		if (ShelfX + Width + GAtlasSpacing > AtlasSize)
		{
			// 다음 줄
			ShelfY += ShelfHeight + GAtlasSpacing;
			ShelfX      = GAtlasSpacing;
			ShelfHeight = 0;
		}
		if (ShelfY + Height + GAtlasSpacing <= AtlasSize && Width + 2 * GAtlasSpacing <= AtlasSize)
		{
			OutX = ShelfX;
			OutY = ShelfY;
			ShelfX += Width + GAtlasSpacing;
			ShelfHeight = FMath::Max(ShelfHeight, Height);
			return true;
		}
		if (AtlasSize >= MaxAtlasSize)
		{
			return false;
		}
		GrowAtlas();
	}
}

void FUIFont::GrowAtlas()
{
	// 기존 텍셀 좌표를 그대로 두고 오른쪽/아래로 넓힌다 (다음 줄부터 새 너비 사용)
	const uint32       NewSize = AtlasSize * 2;
	std::vector<uint8> NewPixels(static_cast<size_t>(NewSize) * NewSize, 0);
	for (uint32 Row = 0; Row < AtlasSize; ++Row)
	{
		std::memcpy(&NewPixels[static_cast<size_t>(Row) * NewSize], &AtlasPixels[static_cast<size_t>(Row) * AtlasSize], AtlasSize);
	}
	AtlasPixels = std::move(NewPixels);
	AtlasSize   = NewSize;
	E_LOG(LogUI, Log, "글꼴 아틀라스 확장: {0}x{0}", AtlasSize);
}

const FUIGlyph& FUIFont::GetGlyph(uint32 Codepoint)
{
	const auto It = Glyphs.find(Codepoint);
	if (It != Glyphs.end())
	{
		return It->second;
	}

	FUIGlyph Glyph;
	Glyph.GlyphIndex = stbtt_FindGlyphIndex(&FontInfo->Info, static_cast<int32>(Codepoint));
	int32 AdvanceWidth    = 0;
	int32 LeftSideBearing = 0;
	stbtt_GetGlyphHMetrics(&FontInfo->Info, Glyph.GlyphIndex, &AdvanceWidth, &LeftSideBearing);
	Glyph.Advance = static_cast<float>(AdvanceWidth) * BaseScale;

	if (!IsSpace(Codepoint) && Codepoint != '\n')
	{
		// 4배 해상도 래스터 → 거리 변환 SDF (UISdf). stb_truetype의 SDF 함수는 3차 곡선(OTF/CFF)을 처리하지 못한다.
		// 가장자리 값 128, 거리 1픽셀당 128/패딩 → 모양 바깥 패딩 픽셀까지 0~255로 표현
		constexpr int32    Oversample = 4;
		std::vector<uint8> Sdf;
		int32              Width   = 0;
		int32              Height  = 0;
		int32              OffsetX = 0;
		int32              OffsetY = 0;
		int32              X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
		stbtt_GetGlyphBitmapBox(&FontInfo->Info, Glyph.GlyphIndex, BaseScale, BaseScale, &X0, &Y0, &X1, &Y1);
		if (X1 > X0 && Y1 > Y0)
		{
			Width               = X1 - X0 + 2 * SdfPadding;
			Height              = Y1 - Y0 + 2 * SdfPadding;
			OffsetX             = X0 - SdfPadding;
			OffsetY             = Y0 - SdfPadding;
			const float HiScale = BaseScale * static_cast<float>(Oversample);
			int32       HX0 = 0, HY0 = 0, HX1 = 0, HY1 = 0;
			stbtt_GetGlyphBitmapBox(&FontInfo->Info, Glyph.GlyphIndex, HiScale, HiScale, &HX0, &HY0, &HX1, &HY1);
			const int32        CanvasWidth  = Width * Oversample;
			const int32        CanvasHeight = Height * Oversample;
			std::vector<uint8> Coverage(static_cast<size_t>(CanvasWidth) * CanvasHeight, 0);
			// 고해상도 상자를 기준 상자(+패딩) 안에 놓는다 (반올림 차이만큼 잘라 안전하게)
			const int32 PlaceX = std::clamp(HX0 - OffsetX * Oversample, 0, CanvasWidth - 1);
			const int32 PlaceY = std::clamp(HY0 - OffsetY * Oversample, 0, CanvasHeight - 1);
			const int32 DrawW  = std::min(HX1 - HX0, CanvasWidth - PlaceX);
			const int32 DrawH  = std::min(HY1 - HY0, CanvasHeight - PlaceY);
			if (DrawW > 0 && DrawH > 0)
			{
				stbtt_MakeGlyphBitmap(&FontInfo->Info, &Coverage[static_cast<size_t>(PlaceY) * CanvasWidth + PlaceX], DrawW, DrawH, CanvasWidth, HiScale,
				                      HiScale, Glyph.GlyphIndex);
				UISdf::MakeSdf(Coverage.data(), CanvasWidth, CanvasHeight, Oversample, static_cast<float>(SdfPadding), Sdf);
			}
		}
		uint32 AtlasX = 0;
		uint32 AtlasY = 0;
		if (!Sdf.empty())
		{
			if (AllocateAtlasRect(static_cast<uint32>(Width), static_cast<uint32>(Height), AtlasX, AtlasY))
			{
				for (int32 Row = 0; Row < Height; ++Row)
				{
					std::memcpy(&AtlasPixels[(static_cast<size_t>(AtlasY) + static_cast<size_t>(Row)) * AtlasSize + AtlasX],
					            &Sdf[static_cast<size_t>(Row) * static_cast<size_t>(Width)], static_cast<size_t>(Width));
				}
				Glyph.AtlasX  = static_cast<uint16>(AtlasX);
				Glyph.AtlasY  = static_cast<uint16>(AtlasY);
				Glyph.Width   = static_cast<uint16>(Width);
				Glyph.Height  = static_cast<uint16>(Height);
				Glyph.OffsetX = static_cast<float>(OffsetX);
				Glyph.OffsetY = static_cast<float>(OffsetY);
				++AtlasVersion;
			}
			else if (!bWarnedFull)
			{
				E_LOG(LogUI, Warning, "글꼴 아틀라스가 가득 차 일부 글자를 그리지 못합니다 ({0}x{0})", AtlasSize);
				bWarnedFull = true;
			}
		}
	}
	return Glyphs.emplace(Codepoint, Glyph).first->second;
}

void FUIFont::Layout(std::string_view Utf8, float FontSize, float WrapWidth, FUITextLayout& Out)
{
	Out.Glyphs.clear();
	Out.Lines.clear();
	Out.Size       = FVector2::ZeroVector;
	Out.LineHeight = IsLoaded() ? GetLineHeight(FontSize) : FontSize;
	if (!IsLoaded())
	{
		return;
	}

	const float               Scale      = FontSize / BasePixelHeight;
	const std::vector<uint32> Codepoints = DecodeUtf8(Utf8);

	// 단어(공백 뒤에서 끊을 수 있는 구간) 단위로 줄에 넣는다
	struct FPending
	{
		uint32 Codepoint;
		float  X; // 줄 시작 기준 펜 위치 (UI 단위)
	};
	std::vector<FPending> LineGlyphs;
	float                 PenX          = 0.0f;
	int32                 PrevGlyph     = 0;
	size_t                LastBreak     = 0; // LineGlyphs에서 마지막 공백 다음 위치 (0 = 없음)
	float                 LastBreakPenX = 0.0f;

	const auto FlushLine = [&](size_t Count, float LineWidth) {
		FUITextLine Line;
		Line.FirstGlyph = static_cast<uint32>(Out.Glyphs.size());
		const float Top = static_cast<float>(Out.Lines.size()) * Out.LineHeight;
		// 줄 끝 공백은 너비에서 뺀다
		for (size_t Index = 0; Index < Count; ++Index)
		{
			const FUIGlyph& Glyph = GetGlyph(LineGlyphs[Index].Codepoint);
			if (Glyph.Width == 0)
			{
				continue;
			}
			FUIPlacedGlyph Placed;
			Placed.Position    = FVector2(LineGlyphs[Index].X + Glyph.OffsetX * Scale, Top + (Ascent + Glyph.OffsetY) * Scale);
			Placed.Size        = FVector2(static_cast<float>(Glyph.Width), static_cast<float>(Glyph.Height)) * Scale;
			Placed.AtlasX      = Glyph.AtlasX;
			Placed.AtlasY      = Glyph.AtlasY;
			Placed.AtlasWidth  = Glyph.Width;
			Placed.AtlasHeight = Glyph.Height;
			Placed.Line        = static_cast<uint32>(Out.Lines.size());
			Out.Glyphs.push_back(Placed);
		}
		Line.GlyphCount = static_cast<uint32>(Out.Glyphs.size()) - Line.FirstGlyph;
		Line.Width      = LineWidth;
		Out.Lines.push_back(Line);
		Out.Size.X = FMath::Max(Out.Size.X, LineWidth);
	};
	// 공백을 뺀 줄 너비
	const auto TrimmedWidth = [&](size_t Count, float EndPen) {
		float Width = EndPen;
		for (size_t Index = Count; Index > 0; --Index)
		{
			if (!IsSpace(LineGlyphs[Index - 1].Codepoint))
			{
				break;
			}
			Width = LineGlyphs[Index - 1].X;
		}
		return Width;
	};

	for (const uint32 Codepoint : Codepoints)
	{
		if (Codepoint == '\r')
		{
			continue;
		}
		if (Codepoint == '\n')
		{
			FlushLine(LineGlyphs.size(), TrimmedWidth(LineGlyphs.size(), PenX));
			LineGlyphs.clear();
			PenX      = 0.0f;
			PrevGlyph = 0;
			LastBreak = 0;
			continue;
		}
		const FUIGlyph& Glyph   = GetGlyph(Codepoint);
		const float     Kerning = GetKerning(PrevGlyph, Glyph.GlyphIndex) * Scale;
		const float     X       = PenX + Kerning;
		const float     Advance = Glyph.Advance * Scale;

		if (WrapWidth > 0.0f && !IsSpace(Codepoint) && X + Advance > WrapWidth && !LineGlyphs.empty())
		{
			// 넘친다: 마지막 공백에서 끊고 나머지 단어는 다음 줄로, 공백이 없으면 이 글자 앞에서 끊는다
			const size_t BreakAt   = LastBreak > 0 ? LastBreak : LineGlyphs.size();
			const float  BreakPenX = LastBreak > 0 ? LastBreakPenX : X;
			FlushLine(BreakAt, TrimmedWidth(BreakAt, BreakPenX));
			std::vector<FPending> Carry(LineGlyphs.begin() + static_cast<std::ptrdiff_t>(BreakAt), LineGlyphs.end());
			const float           Shift = Carry.empty() ? X : Carry.front().X;
			for (FPending& Pending : Carry)
			{
				Pending.X -= Shift;
			}
			LineGlyphs = std::move(Carry);
			PenX       = X - Shift;
			LastBreak  = 0;
			LineGlyphs.push_back({ Codepoint, PenX });
			PenX += Advance;
			PrevGlyph = Glyph.GlyphIndex;
			continue;
		}

		LineGlyphs.push_back({ Codepoint, X });
		PenX      = X + Advance;
		PrevGlyph = Glyph.GlyphIndex;
		if (IsSpace(Codepoint))
		{
			LastBreak     = LineGlyphs.size();
			LastBreakPenX = PenX;
		}
	}
	FlushLine(LineGlyphs.size(), TrimmedWidth(LineGlyphs.size(), PenX));
	Out.Size.Y = static_cast<float>(Out.Lines.size()) * Out.LineHeight;
}

void FUIFont::GetCaretPositions(std::string_view Utf8, float FontSize, std::vector<float>& OutPositions)
{
	OutPositions.clear();
	OutPositions.push_back(0.0f);
	if (!IsLoaded())
	{
		return;
	}
	const float Scale     = FontSize / BasePixelHeight;
	float       PenX      = 0.0f;
	int32       PrevGlyph = 0;
	for (const uint32 Codepoint : DecodeUtf8(Utf8))
	{
		const FUIGlyph& Glyph = GetGlyph(Codepoint);
		PenX += (GetKerning(PrevGlyph, Glyph.GlyphIndex) + Glyph.Advance) * Scale;
		PrevGlyph = Glyph.GlyphIndex;
		OutPositions.push_back(PenX);
	}
}

void FUIFont::Prebake(std::string_view Utf8)
{
	if (!IsLoaded())
	{
		return;
	}
	for (const uint32 Codepoint : DecodeUtf8(Utf8))
	{
		GetGlyph(Codepoint);
	}
}

FVector2 FUIFont::Measure(std::string_view Utf8, float FontSize, float WrapWidth)
{
	FUITextLayout Layout;
	this->Layout(Utf8, FontSize, WrapWidth, Layout);
	return Layout.Size;
}

// ---------------------------------------------------------------- FUIFontLibrary

FUIFontLibrary& FUIFontLibrary::Get()
{
	static FUIFontLibrary Library;
	return Library;
}

void FUIFontLibrary::SetDefaultFontPath(const std::filesystem::path& Path)
{
	DefaultFontPath    = Path;
	DefaultFont        = nullptr;
	DefaultFontFile.clear();
	bDefaultFontLoaded = false;
}

FUIFont* FUIFontLibrary::LoadCached(const std::filesystem::path& Path)
{
	const std::wstring Key = MakeKey(Path);
	const auto         It  = Fonts.find(Key);
	if (It != Fonts.end())
	{
		return It->second.get();
	}
	auto Font = std::make_unique<FUIFont>();
	if (!Font->LoadFromFile(Path))
	{
		Font.reset();
	}
	FUIFont* Raw = Font.get();
	Fonts.emplace(Key, std::move(Font));
	return Raw;
}

FUIFont* FUIFontLibrary::GetDefaultFont()
{
	if (bDefaultFontLoaded)
	{
		return DefaultFont;
	}
	bDefaultFontLoaded = true;
	if (!DefaultFontPath.empty())
	{
		const std::filesystem::path Path = DefaultFontPath.is_absolute() ? DefaultFontPath : ContentDirectory / DefaultFontPath;
		DefaultFont                      = LoadCached(Path);
		DefaultFontFile                  = DefaultFont != nullptr ? Path : std::filesystem::path();
	}
	if (DefaultFont == nullptr)
	{
		// 엔진 번들 글꼴 (Noto Sans KR, OFL) — 맑은 고딕이 없는 PC/배포에서도 한글
		const std::filesystem::path Bundled = FPaths::GetEngineDirectory() / L"Engine" / L"Content" / L"Fonts" / L"NotoSansKR-Regular.otf";
		std::error_code             ErrorCode;
		if (std::filesystem::exists(Bundled, ErrorCode))
		{
			DefaultFontFile = Bundled;
			DefaultFont     = LoadCached(Bundled);
		}
	}
	if (DefaultFont == nullptr)
	{
		DefaultFontFile = GetSystemFontPath();
		DefaultFont     = LoadCached(DefaultFontFile);
		if (DefaultFont == nullptr)
		{
			DefaultFontFile.clear();
		}
	}
	if (DefaultFont == nullptr)
	{
		E_LOG(LogUI, Error, "UI 기본 글꼴을 찾지 못해 텍스트를 그리지 않습니다");
	}
	return DefaultFont;
}

std::filesystem::path FUIFontLibrary::GetDefaultFontFile()
{
	GetDefaultFont();
	return DefaultFontFile;
}

FUIFont* FUIFontLibrary::GetFont(std::string_view FontPath)
{
	if (FontPath.empty())
	{
		return GetDefaultFont();
	}
	const std::filesystem::path Relative = FStringConv::ToWide(FontPath);
	FUIFont*                    Font     = LoadCached(Relative.is_absolute() ? Relative : ContentDirectory / Relative);
	return Font != nullptr ? Font : GetDefaultFont();
}

std::vector<FUIFont*> FUIFontLibrary::GetLoadedFonts() const
{
	std::vector<FUIFont*> Result;
	for (const auto& [Key, Font] : Fonts)
	{
		if (Font != nullptr)
		{
			Result.push_back(Font.get());
		}
	}
	return Result;
}

FVector2 FUIFontLibrary::MeasureText(const FUIWidgetData& TextWidget, float WrapWidth)
{
	FUIFont* Font = GetFont(TextWidget.Font);
	if (Font == nullptr || TextWidget.Text.empty())
	{
		return FVector2(0.0f, Font != nullptr ? Font->GetLineHeight(TextWidget.FontSize) : TextWidget.FontSize);
	}
	return Font->Measure(TextWidget.Text, TextWidget.FontSize, TextWidget.bWrap ? WrapWidth : 0.0f);
}

void FUIFontLibrary::Clear()
{
	Fonts.clear();
	DefaultFont        = nullptr;
	DefaultFontFile.clear();
	bDefaultFontLoaded = false;
}
