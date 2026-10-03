#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <cstring>
#include <memory>
#include <vector>

namespace
{
	constexpr float Tol = 1.0e-4f;

	// 결정적 의사 난수 (테스트 재현용)
	struct FTestRandom
	{
		uint32 State = 12345u;
		uint32 Next()
		{
			State = State * 1664525u + 1013904223u;
			return State >> 8;
		}
		float  Range(float Min, float Max) { return Min + (Max - Min) * static_cast<float>(Next() & 0xFFFF) / 65535.0f; }
		uint32 Index(size_t Count) { return Next() % static_cast<uint32>(Count); }
	};

	// 모든 엔티티의 WorldMatrix가 "로컬 × 부모 월드(소켓 부착이면 소켓)"와 비트 단위로 같은지 = 전체 재계산 결과와 같은지 (부모부터 귀납)
	bool AllWorldMatricesMatchFullRecompute(FScene& Scene, int32& OutMismatches)
	{
		OutMismatches = 0;
		Scene.GetRegistry().View<FTransformComponent, FHierarchyComponent>().Each(
			[&](FEntity Entity, const FTransformComponent& Transform, const FHierarchyComponent&) {
				const FMatrix4x4 Expected = Transform.GetLocalMatrix() * Scene.GetParentWorldMatrix(Entity);
				if (std::memcmp(&Expected, &Transform.WorldMatrix, sizeof(FMatrix4x4)) != 0)
				{
					++OutMismatches;
				}
			});
		return OutMismatches == 0;
	}
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

// 월드 행렬 캐시(로컬 TRS + 부모 월드가 같으면 계산 생략): 프레임마다 무작위 편집(로컬 변경, 같은 값 다시 쓰기, 부모 변경, 생성/파괴,
// 구조체 통째 복사, 월드 행렬 직접 쓰기 + 무효화, 소켓 뼈 이동) 뒤에도 결과가 전체 재계산과 비트 단위로 같아야 한다
E_TEST(Scene_TransformCacheMatchesFullRecompute)
{
	FScene      Scene;
	FRegistry&  Registry = Scene.GetRegistry();
	FTestRandom Random;

	std::vector<FEntity> Entities;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		const FEntity Entity = Scene.CreateEntity("E");
		if (!Entities.empty() && Random.Index(4) != 0)
		{
			Scene.SetParent(Entity, Entities[Random.Index(Entities.size())]);
		}
		FTransformComponent& Transform = Scene.GetTransform(Entity);
		Transform.Position             = FVector3(Random.Range(-100.0f, 100.0f), Random.Range(-100.0f, 100.0f), Random.Range(-100.0f, 100.0f));
		Transform.Rotation             = FQuat::FromEuler(Random.Range(-90.0f, 90.0f), Random.Range(-180.0f, 180.0f), Random.Range(-180.0f, 180.0f));
		Entities.push_back(Entity);
	}

	// 소켓: 모델 루트 + 뼈 + 소켓 부착 엔티티 2개 (하나는 자식을 가진다)
	const FEntity Model = Scene.CreateEntity("Model");
	const FEntity Bone  = Scene.CreateEntity("Hand");
	Scene.SetParent(Bone, Model);
	auto         Metadata = std::make_shared<FModelMetadata>();
	FModelSocket Socket;
	Socket.Name     = "Grip";
	Socket.Bone     = "Hand";
	Socket.Position = FVector3(0.0f, 0.0f, 10.0f);
	Metadata->Sockets.push_back(Socket);
	FModelComponent& ModelComponent    = Registry.Emplace<FModelComponent>(Model);
	ModelComponent.Runtime.Metadata    = Metadata;
	ModelComponent.Runtime.NodeEntities = { Bone };
	const FEntity Sword                = Scene.CreateEntity("Sword");
	Registry.Emplace<FSocketAttachmentComponent>(Sword) = { Model, "Grip" };
	const FEntity SwordChild                            = Scene.CreateEntity("SwordChild");
	Scene.SetParent(SwordChild, Sword);
	Scene.GetTransform(SwordChild).Position = FVector3(3.0f, 0.0f, 0.0f);
	const FEntity Shield                    = Scene.CreateEntity("Shield");
	Registry.Emplace<FSocketAttachmentComponent>(Shield) = { Model, "Grip" };

	int32 Mismatches = 0;
	Scene.UpdateTransforms();
	E_EXPECT_TRUE(AllWorldMatricesMatchFullRecompute(Scene, Mismatches));

	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		// 로컬 변경 (일부만)
		for (const FEntity Entity : Entities)
		{
			if (!Registry.IsValid(Entity))
			{
				continue;
			}
			FTransformComponent& Transform = Scene.GetTransform(Entity);
			switch (Random.Index(10))
			{
			case 0: Transform.Position.X += Random.Range(-5.0f, 5.0f); break;
			case 1: Transform.Rotation = FQuat::FromEuler(Random.Range(-90.0f, 90.0f), Random.Range(-180.0f, 180.0f), 0.0f); break;
			case 2: Transform.Scale = FVector3(Random.Range(0.5f, 2.0f)); break;
			case 3: Transform.Position = Transform.Position; break; // 같은 값 다시 쓰기
			case 4: Transform.Position.Y = (Random.Index(2) == 0) ? 0.0f : -0.0f; break; // 부호 있는 0도 비트로 구분
			default: break;
			}
		}
		// 부모 변경 (순환은 SetParent가 거부)
		for (int32 Count = 0; Count < 3; ++Count)
		{
			const FEntity Child  = Entities[Random.Index(Entities.size())];
			const FEntity Parent = Random.Index(5) == 0 ? NullEntity : Entities[Random.Index(Entities.size())];
			if (Registry.IsValid(Child) && (!Parent.IsValid() || (Registry.IsValid(Parent) && Parent != Child)))
			{
				Scene.SetParent(Child, Parent);
			}
		}
		// 파괴 + 생성 (풀의 빈자리 메우기로 컴포넌트가 옮겨진다)
		if (const FEntity Victim = Entities[Random.Index(Entities.size())]; Registry.IsValid(Victim) && Frame % 3 == 0)
		{
			Scene.DestroyEntity(Victim);
		}
		for (int32 Count = 0; Count < 2; ++Count)
		{
			const FEntity Created = Scene.CreateEntity("New");
			const FEntity Parent  = Entities[Random.Index(Entities.size())];
			if (Registry.IsValid(Parent))
			{
				Scene.SetParent(Created, Parent);
			}
			Scene.GetTransform(Created).Position = FVector3(Random.Range(-10.0f, 10.0f), 0.0f, 0.0f);
			Entities.push_back(Created);
		}
		// 구조체 통째 복사 (리플렉션 CopyComponent와 같은 방식 — 캐시도 함께 옮겨진다)
		{
			const FEntity From = Entities[Random.Index(Entities.size())];
			const FEntity To   = Entities[Random.Index(Entities.size())];
			if (Registry.IsValid(From) && Registry.IsValid(To))
			{
				Scene.GetTransform(To) = Scene.GetTransform(From);
			}
		}
		// 월드 행렬 직접 쓰기 (AI SetWorldPose처럼) + 무효화
		if (const FEntity Direct = Entities[Random.Index(Entities.size())]; Registry.IsValid(Direct))
		{
			FTransformComponent& Transform = Scene.GetTransform(Direct);
			Transform.WorldMatrix          = FMatrix4x4::MakeTranslation(FVector3(1.0e4f, 0.0f, 0.0f));
			Transform.InvalidateWorldCache();
		}
		// 뼈 이동 (애니메이션) — 소켓 부착 엔티티가 따라가야 한다
		if (Frame % 2 == 0)
		{
			Scene.GetTransform(Bone).Rotation = FQuat::FromEuler(0.0f, static_cast<float>(Frame) * 7.0f, 0.0f);
		}

		Scene.UpdateTransforms();
		E_EXPECT_TRUE(AllWorldMatricesMatchFullRecompute(Scene, Mismatches));
		E_EXPECT_EQ(Mismatches, 0);
		// 바뀐 것이 없는 두 번째 갱신 (모두 건너뜀)도 같은 결과
		Scene.UpdateTransforms();
		E_EXPECT_TRUE(AllWorldMatricesMatchFullRecompute(Scene, Mismatches));
	}
	// 소켓 부착 엔티티가 뼈를 따라간 위치 (소켓 로컬 (0,0,10) × 뼈 월드)
	FMatrix4x4 GripWorld;
	E_EXPECT_TRUE(Scene.GetSocketWorldMatrix(Model, "Grip", GripWorld));
	E_EXPECT_EQUALS(Scene.GetTransform(Sword).GetWorldPosition(), GripWorld.GetOrigin(), Tol);
	E_EXPECT_EQUALS(Scene.GetTransform(SwordChild).GetWorldPosition(), (FMatrix4x4::MakeTranslation(FVector3(3.0f, 0.0f, 0.0f)) * GripWorld).GetOrigin(), Tol);
}
