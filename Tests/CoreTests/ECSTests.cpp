#include "Core/ECS/Registry.h"
#include "Core/Testing/TestFramework.h"

namespace
{
	struct FPosition
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct FVelocity
	{
		float DX = 0.0f;
		float DY = 0.0f;
	};

	struct FTag
	{
	};
} // namespace

E_TEST(ECS_EntityLifetime)
{
	FRegistry Registry;

	const FEntity A = Registry.Create();
	const FEntity B = Registry.Create();
	E_EXPECT_TRUE(Registry.IsValid(A));
	E_EXPECT_TRUE(Registry.IsValid(B));
	E_EXPECT_TRUE(A != B);
	E_EXPECT_EQ(Registry.GetAliveCount(), 2u);

	Registry.Destroy(A);
	E_EXPECT_FALSE(Registry.IsValid(A));
	E_EXPECT_EQ(Registry.GetAliveCount(), 1u);

	// 슬롯 재사용: 같은 인덱스, 다른 세대
	const FEntity C = Registry.Create();
	E_EXPECT_EQ(C.Index, A.Index);
	E_EXPECT_TRUE(C.Generation != A.Generation);
	E_EXPECT_TRUE(Registry.IsValid(C));
	E_EXPECT_FALSE(Registry.IsValid(A));

	// 파괴된 엔티티를 다시 파괴해도 안전
	Registry.Destroy(A);
	E_EXPECT_EQ(Registry.GetAliveCount(), 2u);
}

E_TEST(ECS_ComponentAddGetRemove)
{
	FRegistry Registry;
	const FEntity E = Registry.Create();

	E_EXPECT_FALSE(Registry.Has<FPosition>(E));
	E_EXPECT_TRUE(Registry.TryGet<FPosition>(E) == nullptr);

	FPosition& Pos = Registry.Emplace<FPosition>(E, 1.0f, 2.0f);
	E_EXPECT_TRUE(Registry.Has<FPosition>(E));
	E_EXPECT_NEAR(Pos.X, 1.0f, 0.0f);
	E_EXPECT_NEAR(Registry.Get<FPosition>(E).Y, 2.0f, 0.0f);

	Registry.Get<FPosition>(E).X = 10.0f;
	E_EXPECT_NEAR(Registry.Get<FPosition>(E).X, 10.0f, 0.0f);

	// GetOrEmplace는 기존 컴포넌트 반환
	E_EXPECT_NEAR(Registry.GetOrEmplace<FPosition>(E).X, 10.0f, 0.0f);

	Registry.Remove<FPosition>(E);
	E_EXPECT_FALSE(Registry.Has<FPosition>(E));

	// 파괴 시 컴포넌트도 제거
	Registry.Emplace<FPosition>(E);
	Registry.Emplace<FVelocity>(E);
	Registry.Destroy(E);
	E_EXPECT_FALSE(Registry.Has<FPosition>(E));
	E_EXPECT_FALSE(Registry.Has<FVelocity>(E));
	E_EXPECT_EQ(Registry.GetPool<FPosition>().Size(), static_cast<size_t>(0));
}

E_TEST(ECS_StaleHandleDoesNotAliasReusedSlot)
{
	FRegistry Registry;
	const FEntity Old = Registry.Create();
	Registry.Emplace<FPosition>(Old, 5.0f, 5.0f);
	Registry.Destroy(Old);

	const FEntity Reused = Registry.Create();
	E_EXPECT_EQ(Reused.Index, Old.Index);
	E_EXPECT_FALSE(Registry.Has<FPosition>(Reused));

	Registry.Emplace<FPosition>(Reused, 1.0f, 1.0f);
	// 오래된 핸들로는 새 엔티티의 컴포넌트에 접근할 수 없다
	E_EXPECT_FALSE(Registry.Has<FPosition>(Old));
	E_EXPECT_TRUE(Registry.TryGet<FPosition>(Old) == nullptr);
}

E_TEST(ECS_SwapRemoveKeepsPoolConsistent)
{
	FRegistry Registry;
	FEntity Entities[5];
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Entities[Index] = Registry.Create();
		Registry.Emplace<FPosition>(Entities[Index], static_cast<float>(Index), 0.0f);
	}

	// 가운데 제거 → 마지막 원소가 빈자리로 이동해도 값이 유지되어야 한다
	Registry.Remove<FPosition>(Entities[1]);
	E_EXPECT_EQ(Registry.GetPool<FPosition>().Size(), static_cast<size_t>(4));
	for (int32 Index : { 0, 2, 3, 4 })
	{
		E_EXPECT_TRUE(Registry.Has<FPosition>(Entities[Index]));
		E_EXPECT_NEAR(Registry.Get<FPosition>(Entities[Index]).X, static_cast<float>(Index), 0.0f);
	}
	E_EXPECT_FALSE(Registry.Has<FPosition>(Entities[1]));
}

E_TEST(ECS_ViewIteratesIntersection)
{
	FRegistry Registry;

	// 10개 엔티티: 모두 Position, 짝수만 Velocity, 3의 배수만 Tag
	FEntity Entities[10];
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Entities[Index] = Registry.Create();
		Registry.Emplace<FPosition>(Entities[Index], 0.0f, 0.0f);
		if (Index % 2 == 0)
		{
			Registry.Emplace<FVelocity>(Entities[Index], 1.0f, 2.0f);
		}
		if (Index % 3 == 0)
		{
			Registry.Emplace<FTag>(Entities[Index]);
		}
	}

	// 매크로 인자에 템플릿 쉼표가 들어가지 않도록 지역 변수로 받는다
	const size_t PositionCount = Registry.View<FPosition>().Count();
	const size_t MovingCount   = Registry.View<FPosition, FVelocity>().Count();
	const size_t TaggedCount   = Registry.View<FVelocity, FPosition, FTag>().Count(); // 0, 6
	E_EXPECT_EQ(PositionCount, static_cast<size_t>(10));
	E_EXPECT_EQ(MovingCount, static_cast<size_t>(5));
	E_EXPECT_EQ(TaggedCount, static_cast<size_t>(2));

	// 시스템 형태의 갱신
	Registry.View<FPosition, FVelocity>().Each([](FEntity, FPosition& Pos, FVelocity& Vel) {
		Pos.X += Vel.DX;
		Pos.Y += Vel.DY;
	});
	E_EXPECT_NEAR(Registry.Get<FPosition>(Entities[2]).Y, 2.0f, 0.0f);
	E_EXPECT_NEAR(Registry.Get<FPosition>(Entities[1]).Y, 0.0f, 0.0f);

	// 등록되지 않은 타입이 포함된 뷰는 비어 있다
	struct FNeverAdded {};
	const size_t EmptyCount = Registry.View<FPosition, FNeverAdded>().Count();
	E_EXPECT_EQ(EmptyCount, static_cast<size_t>(0));
}
