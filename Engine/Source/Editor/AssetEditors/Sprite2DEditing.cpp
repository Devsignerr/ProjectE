#include "Editor/AssetEditors/Sprite2DEditing.h"

#include "Physics/Physics2DMath.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <format>
#include <limits>

namespace Sprite2DEditing
{
	namespace
	{
		int32 RoundPx(float Value) { return static_cast<int32>(std::lround(Value)); }

		bool IsDigit(char Char) { return Char >= '0' && Char <= '9'; }
	} // namespace

	FPixelRect GetRect(const FSpriteSlice& Slice) { return FPixelRect{ Slice.X, Slice.Y, Slice.W, Slice.H }; }

	void SetRect(FSpriteSlice& Slice, const FPixelRect& Rect)
	{
		Slice.X = Rect.X;
		Slice.Y = Rect.Y;
		Slice.W = Rect.W;
		Slice.H = Rect.H;
		// 크기가 줄면 테두리도 크기 안으로
		Slice.BorderLeft   = FMath::Clamp(Slice.BorderLeft, 0, Rect.W);
		Slice.BorderRight  = FMath::Clamp(Slice.BorderRight, 0, Rect.W - Slice.BorderLeft);
		Slice.BorderTop    = FMath::Clamp(Slice.BorderTop, 0, Rect.H);
		Slice.BorderBottom = FMath::Clamp(Slice.BorderBottom, 0, Rect.H - Slice.BorderTop);
	}

	FPixelRect NormalizeRect(int32 Left, int32 Top, int32 Right, int32 Bottom)
	{
		if (Right < Left)
		{
			std::swap(Left, Right);
		}
		if (Bottom < Top)
		{
			std::swap(Top, Bottom);
		}
		return FPixelRect{ Left, Top, FMath::Max(Right - Left, 1), FMath::Max(Bottom - Top, 1) };
	}

	FPixelRect ClampRect(const FPixelRect& Rect, int32 TextureWidth, int32 TextureHeight)
	{
		FPixelRect Result = Rect;
		if (TextureWidth > 0)
		{
			const int32 Left  = FMath::Clamp(Rect.X, 0, TextureWidth - 1);
			const int32 Right = FMath::Clamp(Rect.X + Rect.W, Left + 1, TextureWidth);
			Result.X          = Left;
			Result.W          = Right - Left;
		}
		if (TextureHeight > 0)
		{
			const int32 Top    = FMath::Clamp(Rect.Y, 0, TextureHeight - 1);
			const int32 Bottom = FMath::Clamp(Rect.Y + Rect.H, Top + 1, TextureHeight);
			Result.Y           = Top;
			Result.H           = Bottom - Top;
		}
		return Result;
	}

	FPixelRect MakeRectFromDrag(const FVector2& Start, const FVector2& End, int32 TextureWidth, int32 TextureHeight)
	{
		int32 Left   = RoundPx(FMath::Min(Start.X, End.X));
		int32 Right  = RoundPx(FMath::Max(Start.X, End.X));
		int32 Top    = RoundPx(FMath::Min(Start.Y, End.Y));
		int32 Bottom = RoundPx(FMath::Max(Start.Y, End.Y));
		if (TextureWidth > 0)
		{
			Left  = FMath::Clamp(Left, 0, TextureWidth);
			Right = FMath::Clamp(Right, 0, TextureWidth);
		}
		if (TextureHeight > 0)
		{
			Top    = FMath::Clamp(Top, 0, TextureHeight);
			Bottom = FMath::Clamp(Bottom, 0, TextureHeight);
		}
		if (Right <= Left || Bottom <= Top)
		{
			return FPixelRect{ Left, Top, 0, 0 };
		}
		return FPixelRect{ Left, Top, Right - Left, Bottom - Top };
	}

	FPixelRect ApplyRectDrag(const FPixelRect& Original, ERectHandle Handle, const FVector2& DeltaPixels, int32 TextureWidth, int32 TextureHeight)
	{
		const int32 DX = RoundPx(DeltaPixels.X);
		const int32 DY = RoundPx(DeltaPixels.Y);
		if (Handle == ERectHandle::Move)
		{
			FPixelRect Moved = Original;
			Moved.X += DX;
			Moved.Y += DY;
			// 크기 유지, 텍스처 안으로
			if (TextureWidth > 0)
			{
				Moved.X = FMath::Clamp(Moved.X, 0, FMath::Max(TextureWidth - Moved.W, 0));
			}
			if (TextureHeight > 0)
			{
				Moved.Y = FMath::Clamp(Moved.Y, 0, FMath::Max(TextureHeight - Moved.H, 0));
			}
			return Moved;
		}
		int32 Left   = Original.X;
		int32 Top    = Original.Y;
		int32 Right  = Original.X + Original.W;
		int32 Bottom = Original.Y + Original.H;
		const bool bLeft   = Handle == ERectHandle::Left || Handle == ERectHandle::TopLeft || Handle == ERectHandle::BottomLeft;
		const bool bRight  = Handle == ERectHandle::Right || Handle == ERectHandle::TopRight || Handle == ERectHandle::BottomRight;
		const bool bTop    = Handle == ERectHandle::Top || Handle == ERectHandle::TopLeft || Handle == ERectHandle::TopRight;
		const bool bBottom = Handle == ERectHandle::Bottom || Handle == ERectHandle::BottomLeft || Handle == ERectHandle::BottomRight;
		Left += bLeft ? DX : 0;
		Right += bRight ? DX : 0;
		Top += bTop ? DY : 0;
		Bottom += bBottom ? DY : 0;
		return ClampRect(NormalizeRect(Left, Top, Right, Bottom), TextureWidth, TextureHeight);
	}

	ERectHandle HitTestRect(const FPixelRect& Rect, const FVector2& Point, float HandleRadius)
	{
		const float Left   = static_cast<float>(Rect.X);
		const float Top    = static_cast<float>(Rect.Y);
		const float Right  = static_cast<float>(Rect.X + Rect.W);
		const float Bottom = static_cast<float>(Rect.Y + Rect.H);
		const bool  bNearL = FMath::Abs(Point.X - Left) <= HandleRadius;
		const bool  bNearR = FMath::Abs(Point.X - Right) <= HandleRadius;
		const bool  bNearT = FMath::Abs(Point.Y - Top) <= HandleRadius;
		const bool  bNearB = FMath::Abs(Point.Y - Bottom) <= HandleRadius;
		const bool  bInX   = Point.X >= Left - HandleRadius && Point.X <= Right + HandleRadius;
		const bool  bInY   = Point.Y >= Top - HandleRadius && Point.Y <= Bottom + HandleRadius;
		if (!bInX || !bInY)
		{
			return ERectHandle::None;
		}
		// 아주 작은 사각형은 모서리 핸들이 겹치므로 가까운 쪽을 고른다
		const bool bLeftSide = FMath::Abs(Point.X - Left) <= FMath::Abs(Point.X - Right);
		const bool bTopSide  = FMath::Abs(Point.Y - Top) <= FMath::Abs(Point.Y - Bottom);
		const bool bHorz     = bLeftSide ? bNearL : bNearR;
		const bool bVert     = bTopSide ? bNearT : bNearB;
		if (bHorz && bVert)
		{
			return bLeftSide ? (bTopSide ? ERectHandle::TopLeft : ERectHandle::BottomLeft) : (bTopSide ? ERectHandle::TopRight : ERectHandle::BottomRight);
		}
		if (bHorz)
		{
			return bLeftSide ? ERectHandle::Left : ERectHandle::Right;
		}
		if (bVert)
		{
			return bTopSide ? ERectHandle::Top : ERectHandle::Bottom;
		}
		return ERectHandle::Move;
	}

	FVector2 PivotFromImagePoint(const FPixelRect& Rect, const FVector2& Point, bool bSnap)
	{
		if (Rect.W <= 0 || Rect.H <= 0)
		{
			return FVector2(0.5f, 0.5f);
		}
		float LocalX = Point.X - static_cast<float>(Rect.X);
		float LocalY = Point.Y - static_cast<float>(Rect.Y);
		if (bSnap)
		{
			// 반 픽셀 단위 (픽셀 경계와 픽셀 가운데)
			LocalX = std::round(LocalX * 2.0f) * 0.5f;
			LocalY = std::round(LocalY * 2.0f) * 0.5f;
		}
		LocalX = FMath::Clamp(LocalX, 0.0f, static_cast<float>(Rect.W));
		LocalY = FMath::Clamp(LocalY, 0.0f, static_cast<float>(Rect.H));
		return FVector2(LocalX / static_cast<float>(Rect.W), 1.0f - LocalY / static_cast<float>(Rect.H));
	}

	FVector2 PivotToImagePoint(const FPixelRect& Rect, const FVector2& Pivot)
	{
		return FVector2(static_cast<float>(Rect.X) + Pivot.X * static_cast<float>(Rect.W), static_cast<float>(Rect.Y) + (1.0f - Pivot.Y) * static_cast<float>(Rect.H));
	}

	std::span<const FPivotPreset> GetPivotPresets()
	{
		static const FPivotPreset Presets[] = {
			{ "가운데", FVector2(0.5f, 0.5f) },   { "발밑", FVector2(0.5f, 0.0f) },     { "위", FVector2(0.5f, 1.0f) },
			{ "왼쪽", FVector2(0.0f, 0.5f) },     { "오른쪽", FVector2(1.0f, 0.5f) },   { "왼쪽 아래", FVector2(0.0f, 0.0f) },
			{ "오른쪽 아래", FVector2(1.0f, 0.0f) }, { "왼쪽 위", FVector2(0.0f, 1.0f) }, { "오른쪽 위", FVector2(1.0f, 1.0f) },
		};
		return Presets;
	}

	void ApplyBorderDrag(FSpriteSlice& Slice, int32 Side, const FVector2& Point)
	{
		const int32 LocalX = RoundPx(Point.X) - Slice.X;
		const int32 LocalY = RoundPx(Point.Y) - Slice.Y;
		switch (Side)
		{
		case 0:
			Slice.BorderLeft = FMath::Clamp(LocalX, 0, Slice.W - Slice.BorderRight);
			break;
		case 1:
			Slice.BorderTop = FMath::Clamp(LocalY, 0, Slice.H - Slice.BorderBottom);
			break;
		case 2:
			Slice.BorderRight = FMath::Clamp(Slice.W - LocalX, 0, Slice.W - Slice.BorderLeft);
			break;
		case 3:
			Slice.BorderBottom = FMath::Clamp(Slice.H - LocalY, 0, Slice.H - Slice.BorderTop);
			break;
		default:
			break;
		}
	}

	bool IsNameTaken(const FSpriteAsset& Asset, std::string_view Name, int32 IgnoreIndex)
	{
		for (size_t Index = 0; Index < Asset.Slices.size(); ++Index)
		{
			if (static_cast<int32>(Index) != IgnoreIndex && Asset.Slices[Index].Name == Name)
			{
				return true;
			}
		}
		return false;
	}

	std::string MakeUniqueName(const FSpriteAsset& Asset, std::string_view Base, int32 IgnoreIndex)
	{
		const std::string BaseName = Base.empty() ? std::string("Slice") : std::string(Base);
		if (!IsNameTaken(Asset, BaseName, IgnoreIndex))
		{
			return BaseName;
		}
		for (int32 Number = 1;; ++Number)
		{
			std::string Candidate = std::format("{}_{}", BaseName, Number);
			if (!IsNameTaken(Asset, Candidate, IgnoreIndex))
			{
				return Candidate;
			}
		}
	}

	std::string ValidateRename(const FSpriteAsset& Asset, int32 Index, std::string_view NewName)
	{
		if (NewName.empty())
		{
			return "이름이 비었습니다";
		}
		if (IsNameTaken(Asset, NewName, Index))
		{
			return std::format("'{}' 이름이 이미 있습니다", NewName);
		}
		return {};
	}

	bool RenameSlice(FSpriteAsset& Asset, int32 Index, std::string_view NewName, std::string* OutError)
	{
		if (Index < 0 || Index >= static_cast<int32>(Asset.Slices.size()))
		{
			if (OutError != nullptr)
			{
				*OutError = "슬라이스 번호가 범위 밖입니다";
			}
			return false;
		}
		const std::string Error = ValidateRename(Asset, Index, NewName);
		if (!Error.empty())
		{
			if (OutError != nullptr)
			{
				*OutError = Error;
			}
			return false;
		}
		Asset.Slices[static_cast<size_t>(Index)].Name = std::string(NewName);
		return true;
	}

	bool NaturalLess(std::string_view A, std::string_view B)
	{
		size_t I = 0;
		size_t J = 0;
		while (I < A.size() && J < B.size())
		{
			if (IsDigit(A[I]) && IsDigit(B[J]))
			{
				// 숫자 덩어리: 앞의 0을 건너뛰고 길이 → 사전순
				size_t EndA = I;
				size_t EndB = J;
				while (EndA < A.size() && IsDigit(A[EndA]))
				{
					++EndA;
				}
				while (EndB < B.size() && IsDigit(B[EndB]))
				{
					++EndB;
				}
				size_t StartA = I;
				size_t StartB = J;
				while (StartA + 1 < EndA && A[StartA] == '0')
				{
					++StartA;
				}
				while (StartB + 1 < EndB && B[StartB] == '0')
				{
					++StartB;
				}
				const std::string_view NumA = A.substr(StartA, EndA - StartA);
				const std::string_view NumB = B.substr(StartB, EndB - StartB);
				if (NumA.size() != NumB.size())
				{
					return NumA.size() < NumB.size();
				}
				if (NumA != NumB)
				{
					return NumA < NumB;
				}
				I = EndA;
				J = EndB;
				continue;
			}
			if (A[I] != B[J])
			{
				return static_cast<unsigned char>(A[I]) < static_cast<unsigned char>(B[J]);
			}
			++I;
			++J;
		}
		return (A.size() - I) < (B.size() - J);
	}

	void SortSlices(FSpriteAsset& Asset, ESliceSort Sort)
	{
		if (Sort == ESliceSort::Name)
		{
			std::stable_sort(Asset.Slices.begin(), Asset.Slices.end(), [](const FSpriteSlice& A, const FSpriteSlice& B) { return NaturalLess(A.Name, B.Name); });
			return;
		}
		std::stable_sort(Asset.Slices.begin(), Asset.Slices.end(), [](const FSpriteSlice& A, const FSpriteSlice& B) {
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
	}

	int32 AddSlice(FSpriteAsset& Asset, const FPixelRect& Rect, std::string_view BaseName)
	{
		FSpriteSlice Slice;
		Slice.Name = MakeUniqueName(Asset, BaseName);
		SetRect(Slice, Rect);
		Asset.Slices.push_back(std::move(Slice));
		return static_cast<int32>(Asset.Slices.size()) - 1;
	}

	bool IsRegionTransparent(const FImageView& Image, const FPixelRect& Rect)
	{
		if (Image.Width <= 0 || Image.Height <= 0 || Image.Pixels.size() < static_cast<size_t>(Image.Width) * static_cast<size_t>(Image.Height) * 4)
		{
			return false; // 픽셀을 모르면 빈 칸으로 보지 않는다
		}
		const int32 Left   = FMath::Clamp(Rect.X, 0, Image.Width);
		const int32 Right  = FMath::Clamp(Rect.X + Rect.W, 0, Image.Width);
		const int32 Top    = FMath::Clamp(Rect.Y, 0, Image.Height);
		const int32 Bottom = FMath::Clamp(Rect.Y + Rect.H, 0, Image.Height);
		for (int32 Y = Top; Y < Bottom; ++Y)
		{
			const size_t Row = static_cast<size_t>(Y) * static_cast<size_t>(Image.Width);
			for (int32 X = Left; X < Right; ++X)
			{
				if (Image.Pixels[(Row + static_cast<size_t>(X)) * 4 + 3] != 0)
				{
					return false;
				}
			}
		}
		return true;
	}

	void ComputeGridCellSize(int32 TextureWidth, int32 TextureHeight, const FGridSliceOptions& Options, int32& OutCellWidth, int32& OutCellHeight)
	{
		if (!Options.bByCount)
		{
			OutCellWidth  = Options.CellWidth;
			OutCellHeight = Options.CellHeight;
			return;
		}
		const int32 Columns = FMath::Max(Options.Columns, 1);
		const int32 Rows    = FMath::Max(Options.Rows, 1);
		OutCellWidth        = (TextureWidth - 2 * Options.Margin - (Columns - 1) * Options.Spacing) / Columns;
		OutCellHeight       = (TextureHeight - 2 * Options.Margin - (Rows - 1) * Options.Spacing) / Rows;
	}

	std::vector<FSpriteSlice> SliceGridWithOptions(int32 TextureWidth, int32 TextureHeight, const FGridSliceOptions& Options, const FImageView& Image)
	{
		int32 CellWidth  = 0;
		int32 CellHeight = 0;
		ComputeGridCellSize(TextureWidth, TextureHeight, Options, CellWidth, CellHeight);
		if (CellWidth <= 0 || CellHeight <= 0)
		{
			return {};
		}
		std::vector<FSpriteSlice> Cells = SpriteMath::SliceGrid(TextureWidth, TextureHeight, CellWidth, CellHeight, Options.Margin, Options.Spacing, "");
		std::vector<FSpriteSlice> Result;
		Result.reserve(Cells.size());
		for (FSpriteSlice& Cell : Cells)
		{
			if (Options.bSkipEmpty && IsRegionTransparent(Image, GetRect(Cell)))
			{
				continue;
			}
			Cell.Name = std::format("{}{}", Options.NamePrefix, Result.size());
			Result.push_back(std::move(Cell));
		}
		return Result;
	}

	void ApplyGridSlices(FSpriteAsset& Asset, std::vector<FSpriteSlice> Slices, bool bReplace)
	{
		if (bReplace)
		{
			Asset.Slices = std::move(Slices);
			return;
		}
		for (FSpriteSlice& Slice : Slices)
		{
			Slice.Name = MakeUniqueName(Asset, Slice.Name);
			Asset.Slices.push_back(std::move(Slice));
		}
	}

	// ---------------------------------------------------------------- 플립북

	std::vector<int32> NormalizeSelection(std::vector<int32> Selection, int32 Count)
	{
		std::sort(Selection.begin(), Selection.end());
		Selection.erase(std::unique(Selection.begin(), Selection.end()), Selection.end());
		std::erase_if(Selection, [Count](int32 Index) { return Index < 0 || Index >= Count; });
		return Selection;
	}

	namespace
	{
		// 새 프레임 순서(Order[새 번호] = 옛 번호, 옛 번호 -1 = 새 프레임)로 이벤트 프레임 번호를 옮긴다. 사라진 프레임의 이벤트는 지운다
		void RemapEvents(FFlipbookAsset& Asset, const std::vector<int32>& Order, int32 OldCount)
		{
			std::vector<int32> NewIndexOf(static_cast<size_t>(FMath::Max(OldCount, 0)), -1);
			for (size_t NewIndex = 0; NewIndex < Order.size(); ++NewIndex)
			{
				if (Order[NewIndex] >= 0 && Order[NewIndex] < OldCount && NewIndexOf[static_cast<size_t>(Order[NewIndex])] < 0)
				{
					NewIndexOf[static_cast<size_t>(Order[NewIndex])] = static_cast<int32>(NewIndex);
				}
			}
			std::vector<FFlipbookEvent> Kept;
			Kept.reserve(Asset.Events.size());
			for (FFlipbookEvent& Event : Asset.Events)
			{
				if (Event.Frame < 0 || Event.Frame >= OldCount)
				{
					Kept.push_back(std::move(Event)); // 원래 범위 밖(경고 대상)은 그대로 둔다
					continue;
				}
				const int32 NewFrame = NewIndexOf[static_cast<size_t>(Event.Frame)];
				if (NewFrame >= 0)
				{
					Event.Frame = NewFrame;
					Kept.push_back(std::move(Event));
				}
			}
			Asset.Events = std::move(Kept);
		}

		void ApplyOrder(FFlipbookAsset& Asset, const std::vector<int32>& Order, const std::vector<FFlipbookFrame>& NewFrames)
		{
			const int32 OldCount = static_cast<int32>(Asset.Frames.size());
			RemapEvents(Asset, Order, OldCount);
			Asset.Frames = NewFrames;
			Asset.RebuildTimeline();
		}
	} // namespace

	std::vector<int32> InsertFrames(FFlipbookAsset& Asset, int32 Index, std::span<const std::string> SliceNames)
	{
		const int32 Count = static_cast<int32>(Asset.Frames.size());
		if (Index < 0 || Index > Count)
		{
			Index = Count;
		}
		std::vector<int32>          Order;
		std::vector<FFlipbookFrame> Frames;
		std::vector<int32>          Inserted;
		for (int32 Old = 0; Old <= Count; ++Old)
		{
			if (Old == Index)
			{
				for (const std::string& Name : SliceNames)
				{
					FFlipbookFrame Frame;
					Frame.Slice = Name;
					Inserted.push_back(static_cast<int32>(Frames.size()));
					Frames.push_back(std::move(Frame));
					Order.push_back(-1);
				}
			}
			if (Old < Count)
			{
				Frames.push_back(Asset.Frames[static_cast<size_t>(Old)]);
				Order.push_back(Old);
			}
		}
		ApplyOrder(Asset, Order, Frames);
		return Inserted;
	}

	std::vector<int32> MoveFrames(FFlipbookAsset& Asset, std::vector<int32> Selection, int32 InsertBefore)
	{
		const int32 Count = static_cast<int32>(Asset.Frames.size());
		Selection         = NormalizeSelection(std::move(Selection), Count);
		if (Selection.empty())
		{
			return Selection;
		}
		InsertBefore = FMath::Clamp(InsertBefore, 0, Count);
		std::vector<bool> bSelected(static_cast<size_t>(Count), false);
		for (const int32 Index : Selection)
		{
			bSelected[static_cast<size_t>(Index)] = true;
		}
		std::vector<int32> Order;
		std::vector<int32> NewSelection;
		const auto         EmitSelected = [&]() {
			for (const int32 Index : Selection)
			{
				NewSelection.push_back(static_cast<int32>(Order.size()));
				Order.push_back(Index);
			}
		};
		for (int32 Old = 0; Old <= Count; ++Old)
		{
			if (Old == InsertBefore)
			{
				EmitSelected();
			}
			if (Old < Count && !bSelected[static_cast<size_t>(Old)])
			{
				Order.push_back(Old);
			}
		}
		std::vector<FFlipbookFrame> Frames;
		Frames.reserve(Order.size());
		for (const int32 Old : Order)
		{
			Frames.push_back(Asset.Frames[static_cast<size_t>(Old)]);
		}
		ApplyOrder(Asset, Order, Frames);
		return NewSelection;
	}

	std::vector<int32> DuplicateFrames(FFlipbookAsset& Asset, std::vector<int32> Selection)
	{
		const int32 Count = static_cast<int32>(Asset.Frames.size());
		Selection         = NormalizeSelection(std::move(Selection), Count);
		if (Selection.empty())
		{
			return Selection;
		}
		const int32        After = Selection.back();
		std::vector<int32> Order;
		std::vector<FFlipbookFrame> Frames;
		std::vector<int32> Copies;
		for (int32 Old = 0; Old < Count; ++Old)
		{
			Frames.push_back(Asset.Frames[static_cast<size_t>(Old)]);
			Order.push_back(Old);
			if (Old == After)
			{
				for (const int32 Source : Selection)
				{
					Copies.push_back(static_cast<int32>(Frames.size()));
					Frames.push_back(Asset.Frames[static_cast<size_t>(Source)]);
					Order.push_back(-1);
				}
			}
		}
		ApplyOrder(Asset, Order, Frames);
		return Copies;
	}

	int32 RemoveFrames(FFlipbookAsset& Asset, std::vector<int32> Selection)
	{
		const int32 Count = static_cast<int32>(Asset.Frames.size());
		Selection         = NormalizeSelection(std::move(Selection), Count);
		if (Selection.empty())
		{
			return -1;
		}
		std::vector<bool> bSelected(static_cast<size_t>(Count), false);
		for (const int32 Index : Selection)
		{
			bSelected[static_cast<size_t>(Index)] = true;
		}
		std::vector<int32>          Order;
		std::vector<FFlipbookFrame> Frames;
		for (int32 Old = 0; Old < Count; ++Old)
		{
			if (!bSelected[static_cast<size_t>(Old)])
			{
				Order.push_back(Old);
				Frames.push_back(Asset.Frames[static_cast<size_t>(Old)]);
			}
		}
		ApplyOrder(Asset, Order, Frames);
		if (Frames.empty())
		{
			return -1;
		}
		return FMath::Min(Selection.front(), static_cast<int32>(Frames.size()) - 1);
	}

	// ---------------------------------------------------------------- 타일 다각형

	FVector2 SnapTilePoint(const FVector2& Point, int32 TileWidth, int32 TileHeight, bool bSnap)
	{
		FVector2 Result = bSnap ? FVector2(std::round(Point.X), std::round(Point.Y)) : Point;
		Result.X        = FMath::Clamp(Result.X, 0.0f, static_cast<float>(FMath::Max(TileWidth, 0)));
		Result.Y        = FMath::Clamp(Result.Y, 0.0f, static_cast<float>(FMath::Max(TileHeight, 0)));
		return Result;
	}

	int32 FindNearestPoint(std::span<const FVector2> Points, const FVector2& Point, float MaxDistance)
	{
		int32 Best         = -1;
		float BestDistance = MaxDistance * MaxDistance;
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			const float Distance = (Points[Index] - Point).LengthSquared();
			if (Distance <= BestDistance)
			{
				BestDistance = Distance;
				Best         = static_cast<int32>(Index);
			}
		}
		return Best;
	}

	int32 InsertPointOnNearestEdge(std::vector<FVector2>& Points, const FVector2& Point)
	{
		if (Points.size() < 3)
		{
			Points.push_back(Point);
			return static_cast<int32>(Points.size()) - 1;
		}
		size_t BestEdge     = 0;
		float  BestDistance = std::numeric_limits<float>::max();
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			const FVector2& A      = Points[Index];
			const FVector2& B      = Points[(Index + 1) % Points.size()];
			const FVector2  AB     = B - A;
			const float     Length = AB.LengthSquared();
			const float     T      = Length > 0.0f ? FMath::Clamp(FVector2::Dot(Point - A, AB) / Length, 0.0f, 1.0f) : 0.0f;
			const float     Dist   = (A + AB * T - Point).LengthSquared();
			if (Dist < BestDistance)
			{
				BestDistance = Dist;
				BestEdge     = Index;
			}
		}
		Points.insert(Points.begin() + static_cast<std::ptrdiff_t>(BestEdge + 1), Point);
		return static_cast<int32>(BestEdge + 1);
	}

	std::string GetPolygonWarning(std::span<const FVector2> Points)
	{
		if (Points.size() < 3)
		{
			return "점이 3개보다 적어 충돌 모양이 없습니다";
		}
		const std::vector<FVector2> List(Points.begin(), Points.end());
		const bool                  bConvex = Physics2DMath::IsConvexPolygon(List);
		if (Points.size() > MaxPhysicsPolygonPoints)
		{
			return std::format("점이 {}개라 {}개를 넘습니다 — 물리가 볼록 껍질({}점 이하)로 바꿉니다", Points.size(), MaxPhysicsPolygonPoints, MaxPhysicsPolygonPoints);
		}
		if (!bConvex)
		{
			return "오목하거나 같은 직선 위 점이 있습니다 — 물리가 볼록 껍질로 바꿉니다";
		}
		return {};
	}

	FTileDefinition GetTile(const FTilesetAsset& Asset, int32 Id)
	{
		if (const FTileDefinition* Found = Asset.FindTile(Id))
		{
			return *Found;
		}
		FTileDefinition Tile;
		Tile.Id = Id;
		return Tile;
	}

	void SetTile(FTilesetAsset& Asset, const FTileDefinition& Tile)
	{
		auto Found = std::find_if(Asset.Tiles.begin(), Asset.Tiles.end(), [&Tile](const FTileDefinition& Existing) { return Existing.Id == Tile.Id; });
		if (Found != Asset.Tiles.end())
		{
			*Found = Tile;
		}
		else
		{
			Asset.Tiles.push_back(Tile);
		}
		Asset.Normalize();
	}
	// ---------------------------------------------------------------- 격자 대화 기본값

	void EstimateGridCellSize(const FSpriteAsset& Asset, int32& OutWidth, int32& OutHeight)
	{
		if (!Asset.Slices.empty())
		{
			// 가장 흔한 크기 (같은 수면 먼저 나온 것)
			std::vector<std::pair<std::pair<int32, int32>, int32>> Counts;
			for (const FSpriteSlice& Slice : Asset.Slices)
			{
				const std::pair<int32, int32> Size(Slice.W, Slice.H);
				auto It = std::find_if(Counts.begin(), Counts.end(), [&Size](const auto& Entry) { return Entry.first == Size; });
				if (It == Counts.end())
				{
					Counts.push_back({ Size, 1 });
				}
				else
				{
					++It->second;
				}
			}
			const auto Best = std::max_element(Counts.begin(), Counts.end(), [](const auto& A, const auto& B) { return A.second < B.second; });
			OutWidth  = std::max(Best->first.first, 1);
			OutHeight = std::max(Best->first.second, 1);
			return;
		}
		static constexpr int32 Candidates[] = { 16, 32, 8 };
		const int32 Width  = Asset.TextureWidth;
		const int32 Height = Asset.TextureHeight;
		if (Width <= 0 || Height <= 0)
		{
			OutWidth = OutHeight = 16;
			return;
		}
		for (const int32 Candidate : Candidates)
		{
			if (Width % Candidate == 0 && Height % Candidate == 0)
			{
				OutWidth = OutHeight = Candidate;
				return;
			}
		}
		const auto PerAxis = [](int32 Size) {
			for (const int32 Candidate : Candidates)
			{
				if (Size % Candidate == 0)
				{
					return Candidate;
				}
			}
			return Size;
		};
		OutWidth  = PerAxis(Width);
		OutHeight = PerAxis(Height);
	}

	// ---------------------------------------------------------------- 슬라이스 이름 변경 전파

	std::vector<FSliceRename> DetectSliceRenames(const FSpriteAsset& Saved, const FSpriteAsset& Current)
	{
		std::vector<const FSpriteSlice*> Removed;
		std::vector<const FSpriteSlice*> Added;
		for (const FSpriteSlice& Slice : Saved.Slices)
		{
			if (Current.FindSlice(Slice.Name) < 0)
			{
				Removed.push_back(&Slice);
			}
		}
		for (const FSpriteSlice& Slice : Current.Slices)
		{
			if (Saved.FindSlice(Slice.Name) < 0)
			{
				Added.push_back(&Slice);
			}
		}
		std::vector<FSliceRename> Renames;
		for (auto RemovedIt = Removed.begin(); RemovedIt != Removed.end();)
		{
			const FPixelRect Rect    = GetRect(**RemovedIt);
			const auto       AddedIt = std::find_if(Added.begin(), Added.end(), [&Rect](const FSpriteSlice* Slice) { return GetRect(*Slice) == Rect; });
			if (AddedIt == Added.end())
			{
				++RemovedIt;
				continue;
			}
			Renames.push_back({ (*RemovedIt)->Name, (*AddedIt)->Name });
			Added.erase(AddedIt);
			RemovedIt = Removed.erase(RemovedIt);
		}
		if (Removed.size() == 1 && Added.size() == 1)
		{
			Renames.push_back({ Removed.front()->Name, Added.front()->Name });
		}
		return Renames;
	}

	bool ApplySliceRename(std::string& InOutName, std::span<const FSliceRename> Renames)
	{
		for (const FSliceRename& Rename : Renames)
		{
			if (InOutName == Rename.From)
			{
				InOutName = Rename.To;
				return true; // 동시 적용: 처음 맞는 것 하나만 (바꾼 결과를 다시 바꾸지 않는다)
			}
		}
		return false;
	}

	bool IsSameAssetPath(std::string_view A, std::string_view B)
	{
		const auto Normalize = [](std::string_view Path) {
			std::string Out;
			Out.reserve(Path.size());
			for (const char Char : Path)
			{
				Out.push_back(Char == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(Char))));
			}
			while (Out.starts_with("./"))
			{
				Out.erase(0, 2);
			}
			return Out;
		};
		return !A.empty() && Normalize(A) == Normalize(B);
	}

	int32 RenameSliceRefsInFlipbook(const std::string& Text, const std::string& FlipbookPath, const std::string& AtlasPath,
	                                std::span<const FSliceRename> Renames, std::string& OutText)
	{
		FFlipbookAsset Asset;
		if (Renames.empty() || !FFlipbookAsset::FromJsonString(Text, Asset) ||
		    !IsSameAssetPath(FSprite2DLibrary::ResolveReference(FlipbookPath, Asset.Sprite), AtlasPath))
		{
			return 0;
		}
		int32 Changed = 0;
		for (FFlipbookFrame& Frame : Asset.Frames)
		{
			Changed += ApplySliceRename(Frame.Slice, Renames) ? 1 : 0;
		}
		if (Changed > 0)
		{
			OutText = Asset.ToJsonString();
		}
		return Changed;
	}

	int32 RenameSliceRefsInEntityJson(const std::string& Text, const std::string& AtlasPath, std::span<const FSliceRename> Renames, std::string& OutText)
	{
		if (Renames.empty())
		{
			return 0;
		}
		nlohmann::ordered_json Document = nlohmann::ordered_json::parse(Text, nullptr, false);
		if (Document.is_discarded())
		{
			return 0;
		}
		int32      Changed = 0;
		const auto Visit   = [&](auto& Self, nlohmann::ordered_json& Node) -> void {
			if (Node.is_object())
			{
				for (auto It = Node.begin(); It != Node.end(); ++It)
				{
					nlohmann::ordered_json& Value = It.value();
					if (It.key() == "SpriteComponent" && Value.is_object())
					{
						const auto SpriteIt = Value.find("Sprite");
						const auto SliceIt  = Value.find("Slice");
						if (SpriteIt != Value.end() && SpriteIt->is_string() && SliceIt != Value.end() && SliceIt->is_string() &&
						    IsSameAssetPath(SpriteIt->get<std::string>(), AtlasPath))
						{
							std::string Slice = SliceIt->get<std::string>();
							if (ApplySliceRename(Slice, Renames))
							{
								*SliceIt = Slice;
								++Changed;
							}
						}
					}
					Self(Self, Value);
				}
			}
			else if (Node.is_array())
			{
				for (nlohmann::ordered_json& Child : Node)
				{
					Self(Self, Child);
				}
			}
		};
		Visit(Visit, Document);
		if (Changed > 0)
		{
			OutText = Document.dump(2);
		}
		return Changed;
	}
} // namespace Sprite2DEditing
