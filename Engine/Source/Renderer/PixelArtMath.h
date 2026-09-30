#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <cmath>

// 픽셀 아트 렌더링의 CPU 측 순수 계산 (PixelArt.hlsl과 같은 식, 단위 테스트 대상)
//
// 좌표 규약:
//   - 저해상도 소스 = 출력을 PixelSize로 나눈 격자(올림) + 사방 Margin 텍셀 여백. 여백은 서브픽셀 보정으로 밀린 만큼을 채운다
//   - 출력 중심과 소스 중심이 일치하도록 확대한다 (출력이 PixelSize로 나누어떨어지지 않아도 대칭)
//   - 텍셀 스냅: 카메라 위치의 Right/Up 성분을 월드 원점 기준 텍셀 격자로 반올림 → 남은 값(Remainder, 텍셀 단위 [-0.5, 0.5])을
//     확대 단계에서 소스 좌표에 더해 화면상 움직임을 부드럽게 만든다 (Right → +X, Up → 화면 아래가 +Y라 부호 반전)
struct FPixelArtMath
{
	static constexpr uint32 Margin       = 1;
	static constexpr int32  MaxPixelSize = 16;

	static uint32 ClampPixelSize(int32 PixelSize)
	{
		return static_cast<uint32>(FMath::Clamp(PixelSize, 1, MaxPixelSize));
	}

	// 출력 한 변 → 저해상도 소스 한 변 (여백 포함)
	static uint32 GetSourceDimension(uint32 OutputDimension, uint32 PixelSize)
	{
		return (OutputDimension + PixelSize - 1) / PixelSize + Margin * 2;
	}

	// 직교: 도트 하나의 월드 크기 (cm). 출력 세로가 OrthoHeight를 담는다
	static float GetTexelWorldSize(float OrthoHeight, uint32 OutputHeight, uint32 PixelSize)
	{
		return OrthoHeight * static_cast<float>(PixelSize) / static_cast<float>(FMath::Max(OutputHeight, 1u));
	}

	// 소스 전체(여백 포함)가 담는 세로 크기 배율 — 직교 높이/원근 tan(반각)에 곱한다. 출력 세로 대비 소스 세로
	static float GetSourceExtentScale(uint32 SourceHeight, uint32 OutputHeight, uint32 PixelSize)
	{
		return static_cast<float>(SourceHeight * PixelSize) / static_cast<float>(FMath::Max(OutputHeight, 1u));
	}

	struct FSnapResult
	{
		FVector3 SnappedPosition;
		FVector2 Remainder;     // (원래 - 스냅) 위치의 Right/Up 성분, 텍셀 단위 [-0.5, 0.5]
		int64    IndexRight = 0; // 스냅된 격자 번호 (디더 무늬를 월드에 고정할 때 사용)
		int64    IndexUp    = 0;
	};

	static FSnapResult SnapToTexelGrid(const FVector3& Position, const FVector3& Right, const FVector3& Up, float TexelWorldSize)
	{
		FSnapResult Result;
		Result.SnappedPosition = Position;
		if (!(TexelWorldSize > 0.0f))
		{
			return Result;
		}
		// 큰 좌표에서도 정밀도를 지키도록 격자 번호는 double로 계산
		const double AlongRight = static_cast<double>(FVector3::Dot(Position, Right)) / TexelWorldSize;
		const double AlongUp    = static_cast<double>(FVector3::Dot(Position, Up)) / TexelWorldSize;
		const double SnapRight  = std::round(AlongRight);
		const double SnapUp     = std::round(AlongUp);

		Result.Remainder       = FVector2(static_cast<float>(AlongRight - SnapRight), static_cast<float>(AlongUp - SnapUp));
		Result.IndexRight      = static_cast<int64>(SnapRight);
		Result.IndexUp         = static_cast<int64>(SnapUp);
		Result.SnappedPosition = Position - Right * (Result.Remainder.X * TexelWorldSize) - Up * (Result.Remainder.Y * TexelWorldSize);
		return Result;
	}

	// 스냅 나머지 → 확대 단계의 소스 좌표 오프셋 (텍셀). 화면 Y는 아래가 +
	static FVector2 GetSubPixelOffset(const FVector2& Remainder)
	{
		return FVector2(Remainder.X, -Remainder.Y);
	}

	// 확대: 출력 픽셀 위치(픽셀 중심 = +0.5) → 소스 텍셀 좌표(연속). 정수부가 읽을 텍셀
	static FVector2 OutputToSource(const FVector2& OutputPosition, uint32 OutputWidth, uint32 OutputHeight, uint32 SourceWidth,
	                               uint32 SourceHeight, uint32 PixelSize, const FVector2& SubPixelOffset)
	{
		const float Scale = 1.0f / static_cast<float>(PixelSize);
		return FVector2((OutputPosition.X - static_cast<float>(OutputWidth) * 0.5f) * Scale + static_cast<float>(SourceWidth) * 0.5f + SubPixelOffset.X,
		                (OutputPosition.Y - static_cast<float>(OutputHeight) * 0.5f) * Scale + static_cast<float>(SourceHeight) * 0.5f + SubPixelOffset.Y);
	}

	// 디더 무늬 원점 (0~3). 음수 격자 번호도 양수 나머지로
	static uint32 PositiveMod4(int64 Value)
	{
		return static_cast<uint32>(((Value % 4) + 4) % 4);
	}
};
