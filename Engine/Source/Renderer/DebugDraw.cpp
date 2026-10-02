#include "Renderer/DebugDraw.h"

#include "Core/Log.h"
#include "UI/UITypes.h"

#include <algorithm>
#include <cmath>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	uint32 ToByte(float Value)
	{
		return static_cast<uint32>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
	}

	// Direction에 수직인 두 축 (화살촉/원 기준)
	void MakeBasis(const FVector3& Direction, FVector3& OutX, FVector3& OutY)
	{
		const FVector3 Helper = std::abs(Direction.Z) < 0.9f ? FVector3::UpVector : FVector3::ForwardVector;
		OutX                  = FVector3::Cross(Direction, Helper).GetNormalized();
		OutY                  = FVector3::Cross(Direction, OutX).GetNormalized();
	}
} // namespace

FDebugDraw& FDebugDraw::Get()
{
	static FDebugDraw Instance; // 엔진 DLL 안 하나 (게임 모듈도 같은 저장소를 쓴다)
	return Instance;
}

uint32 FDebugDraw::PackColor(const FVector4& SrgbColor)
{
	const FVector4 Linear = UISrgbToLinear(SrgbColor);
	return ToByte(Linear.X) | (ToByte(Linear.Y) << 8) | (ToByte(Linear.Z) << 16) | (ToByte(Linear.W) << 24);
}

void FDebugDraw::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		Clear();
	}
}

void FDebugDraw::AddLine(const FVector3& Start, const FVector3& End, uint32 Color, float Duration, bool bDepthTest)
{
	if (Lines.size() >= MaxLines)
	{
		if (!bOverflowWarned)
		{
			E_LOG(LogRenderer, Warning, "디버그 선 상한({}개) 도달: 새 선을 버립니다 (지속 시간이 긴 선이 쌓였는지 확인)", MaxLines);
			bOverflowWarned = true;
		}
		return;
	}
	Lines.push_back({ Start, End, Color, std::max(Duration, 0.0f), bDepthTest });
}

void FDebugDraw::AddCircle(const FVector3& Center, const FVector3& AxisX, const FVector3& AxisY, float Radius, float StartAngle, float EndAngle,
                           int32 Segments, uint32 Color, float Duration, bool bDepthTest)
{
	const float Step     = (EndAngle - StartAngle) / static_cast<float>(Segments);
	FVector3    Previous = Center + (AxisX * std::cos(StartAngle) + AxisY * std::sin(StartAngle)) * Radius;
	for (int32 Index = 1; Index <= Segments; ++Index)
	{
		const float    Angle = StartAngle + Step * static_cast<float>(Index);
		const FVector3 Point = Center + (AxisX * std::cos(Angle) + AxisY * std::sin(Angle)) * Radius;
		AddLine(Previous, Point, Color, Duration, bDepthTest);
		Previous = Point;
	}
}

void FDebugDraw::DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Duration, bool bDepthTest)
{
	if (bEnabled)
	{
		AddLine(Start, End, PackColor(Color), Duration, bDepthTest);
	}
}

void FDebugDraw::DrawArrow(const FVector3& From, const FVector3& To, const FVector4& Color, float Duration, bool bDepthTest, float HeadSize)
{
	if (!bEnabled)
	{
		return;
	}
	const uint32 Packed = PackColor(Color);
	AddLine(From, To, Packed, Duration, bDepthTest);
	const FVector3 Delta  = To - From;
	const float    Length = Delta.Length();
	if (Length < 1.0e-3f)
	{
		return;
	}
	const FVector3 Direction = Delta / Length;
	const float    Head      = HeadSize > 0.0f ? HeadSize : std::min(Length * 0.25f, 30.0f);
	FVector3       AxisX, AxisY;
	MakeBasis(Direction, AxisX, AxisY);
	const FVector3 Back = To - Direction * Head;
	const float    Half = Head * 0.4f;
	AddLine(To, Back + AxisX * Half, Packed, Duration, bDepthTest);
	AddLine(To, Back - AxisX * Half, Packed, Duration, bDepthTest);
	AddLine(To, Back + AxisY * Half, Packed, Duration, bDepthTest);
	AddLine(To, Back - AxisY * Half, Packed, Duration, bDepthTest);
}

void FDebugDraw::DrawBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const FVector4& Color, float Duration, bool bDepthTest)
{
	if (!bEnabled)
	{
		return;
	}
	const uint32 Packed = PackColor(Color);
	FVector3     Corners[8];
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const FVector3 Local((Index & 1) ? HalfExtents.X : -HalfExtents.X, (Index & 2) ? HalfExtents.Y : -HalfExtents.Y,
		                     (Index & 4) ? HalfExtents.Z : -HalfExtents.Z);
		Corners[Index] = Center + Rotation.RotateVector(Local);
	}
	// 비트 하나만 다른 꼭짓점끼리 = 모서리 12개
	for (int32 Index = 0; Index < 8; ++Index)
	{
		for (int32 Bit = 1; Bit < 8; Bit <<= 1)
		{
			if ((Index & Bit) == 0)
			{
				AddLine(Corners[Index], Corners[Index | Bit], Packed, Duration, bDepthTest);
			}
		}
	}
}

void FDebugDraw::DrawSphere(const FVector3& Center, float Radius, const FVector4& Color, float Duration, bool bDepthTest, int32 Segments)
{
	if (!bEnabled)
	{
		return;
	}
	const uint32 Packed = PackColor(Color);
	const int32  Count  = std::clamp(Segments, 4, 64);
	const float  Full   = 2.0f * FMath::Pi;
	AddCircle(Center, FVector3::ForwardVector, FVector3::RightVector, Radius, 0.0f, Full, Count, Packed, Duration, bDepthTest);
	AddCircle(Center, FVector3::ForwardVector, FVector3::UpVector, Radius, 0.0f, Full, Count, Packed, Duration, bDepthTest);
	AddCircle(Center, FVector3::RightVector, FVector3::UpVector, Radius, 0.0f, Full, Count, Packed, Duration, bDepthTest);
}

void FDebugDraw::DrawCapsule(const FVector3& Center, float Radius, float HalfHeight, const FQuat& Rotation, const FVector4& Color, float Duration,
                             bool bDepthTest, int32 Segments)
{
	if (!bEnabled)
	{
		return;
	}
	const uint32   Packed = PackColor(Color);
	const int32    Count  = std::clamp(Segments, 4, 64);
	const FVector3 X      = Rotation.RotateVector(FVector3::ForwardVector);
	const FVector3 Y      = Rotation.RotateVector(FVector3::RightVector);
	const FVector3 Z      = Rotation.RotateVector(FVector3::UpVector);
	const FVector3 Top    = Center + Z * HalfHeight;
	const FVector3 Bottom = Center - Z * HalfHeight;
	const float    Pi     = FMath::Pi;
	// 위/아래 원, 세로 선 4개, 반구 호 (X-Z / Y-Z 평면)
	AddCircle(Top, X, Y, Radius, 0.0f, 2.0f * Pi, Count, Packed, Duration, bDepthTest);
	AddCircle(Bottom, X, Y, Radius, 0.0f, 2.0f * Pi, Count, Packed, Duration, bDepthTest);
	for (const FVector3& Side : { X, -X, Y, -Y })
	{
		AddLine(Top + Side * Radius, Bottom + Side * Radius, Packed, Duration, bDepthTest);
	}
	const int32 Half = std::max(Count / 2, 2);
	AddCircle(Top, X, Z, Radius, 0.0f, Pi, Half, Packed, Duration, bDepthTest);
	AddCircle(Top, Y, Z, Radius, 0.0f, Pi, Half, Packed, Duration, bDepthTest);
	AddCircle(Bottom, X, Z, Radius, Pi, 2.0f * Pi, Half, Packed, Duration, bDepthTest);
	AddCircle(Bottom, Y, Z, Radius, Pi, 2.0f * Pi, Half, Packed, Duration, bDepthTest);
}

void FDebugDraw::Tick(float DeltaSeconds)
{
	for (FDebugLine& Line : Lines)
	{
		Line.Remaining -= DeltaSeconds;
	}
	std::erase_if(Lines, [](const FDebugLine& Line) { return Line.Remaining <= 0.0f; }); // 지속 시간 0 = 한 번 그린 뒤 다음 Tick
	if (Lines.size() < MaxLines)
	{
		bOverflowWarned = false;
	}
}

void FDebugDraw::Clear()
{
	Lines.clear();
	bOverflowWarned = false;
}
