#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/AssetCache.h"
#include "Renderer/IblMath.h"
#include "Renderer/Image.h"

#include <cmath>

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(Environment_HalfFloatRoundTrip)
{
	// 정확히 표현되는 값은 그대로, 나머지는 상대 오차 2^-11 안
	const float Exact[] = { 0.0f, 1.0f, -2.0f, 0.5f, 65504.0f, 0.25f, 1024.0f };
	for (const float Value : Exact)
	{
		E_EXPECT_NEAR(FHalfFloat::ToFloat(FHalfFloat::FromFloat(Value)), Value, 0.0f);
	}
	const float Values[] = { 0.1f, 3.14159f, 123.456f, 0.001f, 7.0e-5f, 40000.0f };
	for (const float Value : Values)
	{
		const float Back = FHalfFloat::ToFloat(FHalfFloat::FromFloat(Value));
		E_EXPECT_NEAR(Back, Value, std::fabs(Value) * 1.0e-3f + 1.0e-7f);
	}
	// 알려진 비트 패턴: 1.0 = 0x3C00, -2 = 0xC000, 무한대 넘침, 아주 작은 값 0
	E_EXPECT_EQ(FHalfFloat::FromFloat(1.0f), static_cast<uint16>(0x3C00));
	E_EXPECT_EQ(FHalfFloat::FromFloat(-2.0f), static_cast<uint16>(0xC000));
	E_EXPECT_EQ(FHalfFloat::FromFloat(1.0e6f), static_cast<uint16>(0x7C00));
	E_EXPECT_EQ(FHalfFloat::FromFloat(1.0e-9f), static_cast<uint16>(0x0000));
	// 비정규 수 (가장 작은 양수 2^-24)
	E_EXPECT_EQ(FHalfFloat::FromFloat(std::ldexp(1.0f, -24)), static_cast<uint16>(0x0001));
	E_EXPECT_NEAR(FHalfFloat::ToFloat(0x0001), std::ldexp(1.0f, -24), 1.0e-12f);
}

E_TEST(Environment_EquirectMapping)
{
	// +X(앞) = 가운데, +Y(오른쪽)로 돌면 U 증가, 위(+Z) = V 0, 아래 = V 1
	const FVector2 Front = IblMath::DirectionToEquirectUV(FVector3(1.0f, 0.0f, 0.0f), 0.0f);
	E_EXPECT_NEAR(Front.X, 0.5f, Tol);
	E_EXPECT_NEAR(Front.Y, 0.5f, Tol);
	const FVector2 Right = IblMath::DirectionToEquirectUV(FVector3(0.0f, 1.0f, 0.0f), 0.0f);
	E_EXPECT_NEAR(Right.X, 0.75f, Tol);
	const FVector2 Up = IblMath::DirectionToEquirectUV(FVector3(0.0f, 0.0f, 1.0f), 0.0f);
	E_EXPECT_NEAR(Up.Y, 0.0f, Tol);
	const FVector2 Down = IblMath::DirectionToEquirectUV(FVector3(0.0f, 0.0f, -1.0f), 0.0f);
	E_EXPECT_NEAR(Down.Y, 1.0f, Tol);
	// 환경을 +90도 돌리면 원래 앞(U 0.5)에 있던 것이 오른쪽(+Y) 방향에 보인다
	const FVector2 Rotated = IblMath::DirectionToEquirectUV(FVector3(0.0f, 1.0f, 0.0f), FMath::HalfPi);
	E_EXPECT_NEAR(Rotated.X, 0.5f, Tol);
	E_EXPECT_NEAR(Rotated.Y, 0.5f, Tol);
}

E_TEST(Environment_CookedFormatRoundTrip)
{
	FEnvironmentImage Image;
	Image.Width  = 4;
	Image.Height = 2;
	Image.Pixels.resize(4 * 2 * 4);
	for (size_t Index = 0; Index < Image.Pixels.size(); ++Index)
	{
		Image.Pixels[Index] = FHalfFloat::FromFloat(static_cast<float>(Index) * 0.5f);
	}
	FBinaryWriter Writer;
	FAssetCache::WriteEnvironment(Writer, Image);
	FBinaryReader     Reader(Writer.GetBuffer().data(), Writer.GetBuffer().size());
	FEnvironmentImage Loaded;
	E_EXPECT_TRUE(FAssetCache::ReadEnvironment(Reader, Loaded));
	E_EXPECT_EQ(Loaded.Width, 4u);
	E_EXPECT_EQ(Loaded.Height, 2u);
	E_EXPECT_TRUE(Loaded.Pixels == Image.Pixels);

	// 크기가 맞지 않는 데이터는 거부
	FEnvironmentImage Broken = Image;
	Broken.Pixels.pop_back();
	FBinaryWriter BrokenWriter;
	FAssetCache::WriteEnvironment(BrokenWriter, Broken);
	FBinaryReader BrokenReader(BrokenWriter.GetBuffer().data(), BrokenWriter.GetBuffer().size());
	E_EXPECT_FALSE(FAssetCache::ReadEnvironment(BrokenReader, Loaded));
}
