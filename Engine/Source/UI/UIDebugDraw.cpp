#include "UI/UIDebugDraw.h"

#include "UI/UIFont.h"

#include <algorithm>

namespace
{
	// 줄을 '\t'로 나눈 열
	std::vector<std::string_view> SplitColumns(std::string_view Line)
	{
		std::vector<std::string_view> Columns;
		size_t                        Begin = 0;
		while (true)
		{
			const size_t End = Line.find('\t', Begin);
			Columns.push_back(Line.substr(Begin, End == std::string_view::npos ? std::string_view::npos : End - Begin));
			if (End == std::string_view::npos)
			{
				break;
			}
			Begin = End + 1;
		}
		return Columns;
	}

	float GetColumnStep(const std::vector<float>& ColumnWidths, size_t Index)
	{
		if (ColumnWidths.empty())
		{
			return 0.0f;
		}
		return ColumnWidths[std::min(Index, ColumnWidths.size() - 1)];
	}
} // namespace

FUIFont* FUIDebugDraw::GetFont()
{
	return FUIFontLibrary::Get().GetDefaultFont();
}

void FUIDebugDraw::AddRect(FUIDrawList& Out, const FUIRect& Rect, const FVector4& SrgbColor, const FUIRect& Clip)
{
	if (Rect.IsEmpty())
	{
		return;
	}
	FUIDrawQuad Quad;
	Quad.Rect           = FVector4(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y);
	Quad.UV             = FVector4(0.0f, 0.0f, 1.0f, 1.0f);
	Quad.Color          = UISrgbToLinear(SrgbColor);
	Quad.SecondaryColor = FVector4();
	Quad.Params         = FVector4(0.0f, 0.0f, static_cast<float>(EUIDrawMode::Box), 0.0f);
	Out.AddQuad(Quad, FUITextureRef{}, Clip);
}

float FUIDebugDraw::AddText(FUIDrawList& Out, std::string_view Utf8, const FVector2& Position, float FontSize, const FVector4& SrgbColor,
                            const FUIRect& Clip)
{
	FUIFont* Font = GetFont();
	if (Font == nullptr || Utf8.empty())
	{
		return 0.0f;
	}
	FUITextLayout Layout;
	Font->Layout(Utf8, FontSize, 0.0f, Layout);
	FUITextureRef Texture;
	Texture.Font          = Font;
	const FVector4 Color  = UISrgbToLinear(SrgbColor);
	const float    Range  = FUIFont::GetDistanceRange(FontSize);
	for (const FUIPlacedGlyph& Glyph : Layout.Glyphs)
	{
		const FVector2 Min = Position + Glyph.Position;
		const FVector2 Max = Min + Glyph.Size;
		FUIDrawQuad    Quad;
		Quad.Rect           = FVector4(Min.X, Min.Y, Max.X, Max.Y);
		Quad.UV             = FVector4(static_cast<float>(Glyph.AtlasX), static_cast<float>(Glyph.AtlasY), static_cast<float>(Glyph.AtlasX + Glyph.AtlasWidth),
		                               static_cast<float>(Glyph.AtlasY + Glyph.AtlasHeight));
		Quad.Color          = Color;
		Quad.SecondaryColor = FVector4();
		Quad.Params         = FVector4(Range, 0.0f, static_cast<float>(EUIDrawMode::SdfText), 0.0f);
		Out.AddQuad(Quad, Texture, Clip);
	}
	return Layout.Size.X;
}

float FUIDebugDraw::MeasureText(std::string_view Utf8, float FontSize)
{
	FUIFont* Font = GetFont();
	return Font != nullptr && !Utf8.empty() ? Font->Measure(Utf8, FontSize, 0.0f).X : 0.0f;
}

float FUIDebugDraw::GetLineHeight(float FontSize)
{
	FUIFont* Font = GetFont();
	return Font != nullptr ? Font->GetLineHeight(FontSize) : FontSize * 1.25f;
}

FUIRect FUIDebugDraw::AddTextPanel(FUIDrawList& Out, const std::vector<std::string>& Lines, const FVector2& Anchor, bool bRightAlign, float FontSize,
                                   const std::vector<float>& ColumnWidths, const FUIRect& Clip)
{
	if (Lines.empty())
	{
		return {};
	}
	constexpr float Padding    = 6.0f;
	const float     LineHeight = GetLineHeight(FontSize);

	// 너비: 열 시작 위치 + 마지막 열 글자 너비 중 최대
	float Width = 0.0f;
	for (const std::string& Line : Lines)
	{
		const std::vector<std::string_view> Columns = SplitColumns(Line);
		float                               X       = 0.0f;
		for (size_t Index = 0; Index + 1 < Columns.size(); ++Index)
		{
			X += GetColumnStep(ColumnWidths, Index);
		}
		Width = std::max(Width, X + MeasureText(Columns.back(), FontSize));
	}
	const FVector2 Size(Width + Padding * 2.0f, LineHeight * static_cast<float>(Lines.size()) + Padding * 2.0f);
	const FVector2 Min = bRightAlign ? FVector2(Anchor.X - Size.X, Anchor.Y) : Anchor;
	const FUIRect  Panel(Min, Min + Size);
	AddRect(Out, Panel, FVector4(0.0f, 0.0f, 0.0f, 0.6f), Clip);

	FVector2 Cursor = Min + FVector2(Padding, Padding);
	for (const std::string& Line : Lines)
	{
		const std::vector<std::string_view> Columns = SplitColumns(Line);
		float                               X       = Cursor.X;
		for (size_t Index = 0; Index < Columns.size(); ++Index)
		{
			// 첫 열은 왼쪽 정렬, 숫자 열(두 번째부터)도 왼쪽 정렬 — 고정 폭 글꼴이 아니라 열 시작만 맞춘다
			AddText(Out, Columns[Index], FVector2(X, Cursor.Y), FontSize, FVector4(0.85f, 1.0f, 0.85f, 1.0f), Clip);
			X += GetColumnStep(ColumnWidths, Index);
		}
		Cursor.Y += LineHeight;
	}
	return Panel;
}
