#include "Physics/Physics2DMath.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace Physics2DMath
{
	FQuat RotationFromAngle(float AngleRadians)
	{
		return FQuat::FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), -AngleRadians);
	}

	float AngleFromRotation(const FQuat& Rotation, bool* bOutTilted)
	{
		const FQuat    Normalized = Rotation.GetNormalized();
		const FVector3 LocalY     = Normalized.RotateVector(FVector3(0.0f, 1.0f, 0.0f));
		if (bOutTilted != nullptr)
		{
			*bOutTilted = std::abs(LocalY.Y) < 0.9999f;
		}
		FVector3 LocalX = Normalized.RotateVector(FVector3(1.0f, 0.0f, 0.0f));
		if (LocalX.X * LocalX.X + LocalX.Z * LocalX.Z < 1.0e-8f)
		{
			// 로컬 +X가 평면에 수직 (깊이 방향) — 로컬 +Z로 대신 정한다 (+Z = 각 + 90도)
			const FVector3 LocalZ = Normalized.RotateVector(FVector3(0.0f, 0.0f, 1.0f));
			return std::atan2(LocalZ.Z, LocalZ.X) - FMath::HalfPi;
		}
		return std::atan2(LocalX.Z, LocalX.X);
	}

	FVector2 Rotate(const FVector2& V, float AngleRadians)
	{
		const float C = std::cos(AngleRadians);
		const float S = std::sin(AngleRadians);
		return FVector2(V.X * C - V.Y * S, V.X * S + V.Y * C);
	}

	namespace
	{
		std::string_view Trim(std::string_view Text)
		{
			while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r' || Text.front() == '\n'))
			{
				Text.remove_prefix(1);
			}
			while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r' || Text.back() == '\n'))
			{
				Text.remove_suffix(1);
			}
			return Text;
		}

		bool ParseFloat(std::string_view Text, float& OutValue)
		{
			Text = Trim(Text);
			if (!Text.empty() && Text.front() == '+')
			{
				Text.remove_prefix(1);
			}
			if (Text.empty())
			{
				return false;
			}
			const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), OutValue);
			return Error == std::errc() && End == Text.data() + Text.size() && std::isfinite(OutValue);
		}
	} // namespace

	bool ParsePoints(std::string_view Text, std::vector<FVector2>& OutPoints, std::string* OutError)
	{
		OutPoints.clear();
		size_t Start = 0;
		while (Start <= Text.size())
		{
			const size_t           End   = std::min(Text.find(';', Start), Text.size());
			const std::string_view Entry = Trim(Text.substr(Start, End - Start));
			Start                        = End + 1;
			if (Entry.empty())
			{
				continue;
			}
			const size_t Comma = Entry.find(',');
			FVector2     Point;
			if (Comma == std::string_view::npos || !ParseFloat(Entry.substr(0, Comma), Point.X) || !ParseFloat(Entry.substr(Comma + 1), Point.Y))
			{
				if (OutError != nullptr)
				{
					*OutError = std::format("점 '{}'을(를) 읽을 수 없습니다 (형식: \"x,z; x,z; ...\")", Entry);
				}
				return false;
			}
			OutPoints.push_back(Point);
		}
		return true;
	}

	std::string FormatPoints(const std::vector<FVector2>& Points)
	{
		std::string Result;
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			Result += std::format("{}{},{}", Index == 0 ? "" : "; ", Points[Index].X, Points[Index].Y);
		}
		return Result;
	}

	float SignedArea(const std::vector<FVector2>& Points)
	{
		float Twice = 0.0f;
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			Twice += FVector2::Cross(Points[Index], Points[(Index + 1) % Points.size()]);
		}
		return Twice * 0.5f;
	}

	std::vector<FVector2> ComputeConvexHull(const std::vector<FVector2>& Points)
	{
		std::vector<FVector2> Sorted = Points;
		std::sort(Sorted.begin(), Sorted.end(), [](const FVector2& A, const FVector2& B) { return A.X < B.X || (A.X == B.X && A.Y < B.Y); });
		Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());
		if (Sorted.size() < 3)
		{
			return {};
		}
		std::vector<FVector2> Hull(Sorted.size() * 2);
		size_t                Count = 0;
		const auto Turn = [](const FVector2& O, const FVector2& A, const FVector2& B) { return FVector2::Cross(A - O, B - O); };
		for (const FVector2& Point : Sorted) // 아래 사슬
		{
			while (Count >= 2 && Turn(Hull[Count - 2], Hull[Count - 1], Point) <= 0.0f)
			{
				--Count;
			}
			Hull[Count++] = Point;
		}
		const size_t Lower = Count + 1;
		for (size_t Index = Sorted.size() - 1; Index-- > 0;) // 위 사슬
		{
			while (Count >= Lower && Turn(Hull[Count - 2], Hull[Count - 1], Sorted[Index]) <= 0.0f)
			{
				--Count;
			}
			Hull[Count++] = Sorted[Index];
		}
		Hull.resize(Count - 1); // 마지막 = 처음
		if (Hull.size() < 3)
		{
			return {};
		}
		return Hull;
	}

	std::vector<FVector2> ReduceConvexPolygon(std::vector<FVector2> Hull, uint32 MaxPoints)
	{
		MaxPoints = std::max(MaxPoints, 3u);
		while (Hull.size() > MaxPoints)
		{
			size_t Best     = 0;
			float  BestArea = 0.0f;
			for (size_t Index = 0; Index < Hull.size(); ++Index)
			{
				const FVector2& Previous = Hull[(Index + Hull.size() - 1) % Hull.size()];
				const FVector2& Next     = Hull[(Index + 1) % Hull.size()];
				const float     Area     = std::abs(FVector2::Cross(Hull[Index] - Previous, Next - Previous));
				if (Index == 0 || Area < BestArea)
				{
					Best     = Index;
					BestArea = Area;
				}
			}
			Hull.erase(Hull.begin() + static_cast<std::ptrdiff_t>(Best));
		}
		return Hull;
	}

	bool IsConvexPolygon(const std::vector<FVector2>& Points)
	{
		if (Points.size() < 3)
		{
			return false;
		}
		float Sign = 0.0f;
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			const FVector2& A     = Points[Index];
			const FVector2& B     = Points[(Index + 1) % Points.size()];
			const FVector2& C     = Points[(Index + 2) % Points.size()];
			const float     Cross = FVector2::Cross(B - A, C - B);
			if (std::abs(Cross) <= 1.0e-6f)
			{
				return false; // 같은 직선 위 점 (또는 겹친 점)
			}
			if (Sign == 0.0f)
			{
				Sign = Cross;
			}
			else if ((Cross > 0.0f) != (Sign > 0.0f))
			{
				return false;
			}
		}
		// 자기 교차(별 모양 등)는 회전 부호가 같아도 감긴 횟수가 1보다 크다 — 넓이와 껍질 넓이로 거른다
		const float Area     = std::abs(SignedArea(Points));
		const float HullArea = std::abs(SignedArea(ComputeConvexHull(Points)));
		return std::abs(Area - HullArea) <= 1.0e-3f * std::max(1.0f, HullArea);
	}
}
