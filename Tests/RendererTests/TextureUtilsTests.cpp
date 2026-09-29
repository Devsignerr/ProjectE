#include "Core/Testing/TestFramework.h"
#include "RHI/TextureUtils.h"

namespace
{
	// constexpr 결과를 매크로 조건에 직접 넣으면 C4127(상수 조건)이 나므로 런타임 값으로 감싼다
	uint32 MipCount(uint32 Width, uint32 Height) { return CalculateMipCount(Width, Height); }
	uint32 MipDimension(uint32 Base, uint32 Mip) { return GetMipDimension(Base, Mip); }
} // namespace

E_TEST(TextureUtils_MipCount)
{
	E_EXPECT_EQ(MipCount(1, 1), 1u);
	E_EXPECT_EQ(MipCount(2, 2), 2u);
	E_EXPECT_EQ(MipCount(3, 3), 2u);
	E_EXPECT_EQ(MipCount(4, 4), 3u);
	E_EXPECT_EQ(MipCount(256, 256), 9u);
	E_EXPECT_EQ(MipCount(300, 200), 9u);  // floor(log2(300)) = 8
	E_EXPECT_EQ(MipCount(1024, 1), 11u);
	E_EXPECT_EQ(MipCount(1, 2048), 12u);
	E_EXPECT_EQ(MipCount(0, 0), 1u);

	// 컴파일 타임에도 계산 가능
	static_assert(CalculateMipCount(512, 512) == 10);
}

E_TEST(TextureUtils_MipDimension)
{
	E_EXPECT_EQ(MipDimension(256, 0), 256u);
	E_EXPECT_EQ(MipDimension(256, 1), 128u);
	E_EXPECT_EQ(MipDimension(256, 8), 1u);
	E_EXPECT_EQ(MipDimension(256, 12), 1u); // 체인 끝을 넘어도 최소 1
	E_EXPECT_EQ(MipDimension(300, 1), 150u);
	E_EXPECT_EQ(MipDimension(300, 2), 75u);
	E_EXPECT_EQ(MipDimension(75, 1), 37u);  // 홀수는 내림

	// 마지막 밉은 항상 1x1
	const uint32 LastMip = MipCount(300, 200) - 1;
	E_EXPECT_EQ(MipDimension(300, LastMip), 1u);
	E_EXPECT_EQ(MipDimension(200, LastMip), 1u);
}
