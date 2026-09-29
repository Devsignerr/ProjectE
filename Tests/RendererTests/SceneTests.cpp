#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(Scene_CreateEntityHasDefaultComponents)
{
	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("Test");

	E_EXPECT_TRUE(Scene.GetRegistry().Has<FNameComponent>(Entity));
	E_EXPECT_TRUE(Scene.GetRegistry().Has<FTransformComponent>(Entity));
	E_EXPECT_TRUE(Scene.GetRegistry().Has<FHierarchyComponent>(Entity));
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FNameComponent>(Entity).Name == "Test");
	E_EXPECT_FALSE(Scene.GetParent(Entity).IsValid());
}

E_TEST(Scene_HierarchyTransforms)
{
	FScene        Scene;
	const FEntity Parent = Scene.CreateEntity("Parent");
	const FEntity Child  = Scene.CreateEntity("Child");
	const FEntity Grand  = Scene.CreateEntity("Grandchild");

	Scene.SetParent(Child, Parent);
	Scene.SetParent(Grand, Child);
	E_EXPECT_TRUE(Scene.GetParent(Child) == Parent);
	E_EXPECT_EQ(Scene.GetChildren(Parent).size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Scene.IsAncestorOf(Parent, Grand));
	E_EXPECT_FALSE(Scene.IsAncestorOf(Grand, Parent));

	// 부모: +X로 10 이동 후 Yaw 90 → 자식 로컬 +X 오프셋은 월드 +Y 방향
	Scene.GetTransform(Parent).Position = FVector3(10.0f, 0.0f, 0.0f);
	Scene.GetTransform(Parent).Rotation = FQuat::FromEuler(0.0f, 90.0f, 0.0f);
	Scene.GetTransform(Child).Position  = FVector3(5.0f, 0.0f, 0.0f);
	Scene.GetTransform(Grand).Position  = FVector3(0.0f, 0.0f, 1.0f);
	Scene.GetTransform(Grand).Scale     = FVector3(2.0f);

	Scene.UpdateTransforms();

	E_EXPECT_EQUALS(Scene.GetTransform(Parent).GetWorldPosition(), FVector3(10.0f, 0.0f, 0.0f), Tol);
	E_EXPECT_EQUALS(Scene.GetTransform(Child).GetWorldPosition(), FVector3(10.0f, 5.0f, 0.0f), Tol);
	E_EXPECT_EQUALS(Scene.GetTransform(Grand).GetWorldPosition(), FVector3(10.0f, 5.0f, 1.0f), Tol);
	// 손자의 월드 스케일 2가 축 길이에 반영
	E_EXPECT_NEAR(Scene.GetTransform(Grand).WorldMatrix.GetAxisX().Length(), 2.0f, Tol);
	// 자식의 Forward는 부모 회전을 따라 +Y
	E_EXPECT_EQUALS(Scene.GetTransform(Child).GetWorldForward(), FVector3::RightVector, Tol);
}

E_TEST(Scene_ReparentAndCycleRejection)
{
	FScene        Scene;
	const FEntity A = Scene.CreateEntity("A");
	const FEntity B = Scene.CreateEntity("B");
	const FEntity C = Scene.CreateEntity("C");

	Scene.SetParent(B, A);
	Scene.SetParent(C, B);

	// 순환 거부: A를 C의 자식으로 만들 수 없다
	Scene.SetParent(A, C);
	E_EXPECT_FALSE(Scene.GetParent(A).IsValid());
	Scene.SetParent(A, A);
	E_EXPECT_FALSE(Scene.GetParent(A).IsValid());

	// 재부모화: C를 A 아래로 옮기면 B의 자식 목록에서 빠진다
	Scene.SetParent(C, A);
	E_EXPECT_EQ(Scene.GetChildren(B).size(), static_cast<size_t>(0));
	E_EXPECT_EQ(Scene.GetChildren(A).size(), static_cast<size_t>(2));

	// 루트로
	Scene.SetParent(C, NullEntity);
	E_EXPECT_EQ(Scene.GetChildren(A).size(), static_cast<size_t>(1));
	E_EXPECT_FALSE(Scene.GetParent(C).IsValid());
}

E_TEST(Scene_DestroyEntityRecursive)
{
	FScene        Scene;
	const FEntity Root  = Scene.CreateEntity("Root");
	const FEntity Child = Scene.CreateEntity("Child");
	const FEntity Grand = Scene.CreateEntity("Grand");
	const FEntity Other = Scene.CreateEntity("Other");
	Scene.SetParent(Child, Root);
	Scene.SetParent(Grand, Child);

	Scene.DestroyEntity(Child);
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Child));
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Grand));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(Root));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(Other));
	E_EXPECT_EQ(Scene.GetChildren(Root).size(), static_cast<size_t>(0));
	E_EXPECT_EQ(Scene.GetRegistry().GetAliveCount(), 2u);
}
