// 사망 래그돌 (Physics/Ragdoll.h): 뼈대 → 캡슐/관절 배치(순수 함수), 켜기 → 물리가 뼈를 구동, 끄기 → 원래 포즈 복귀
#include "Core/Testing/TestFramework.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/Ragdoll.h"
#include "Scene/Animation.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <string>
#include <vector>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	// 서 있는 사람 모양 뼈대 (모델 공간 = 월드, 루트 원점):
	//   0 Hips(0,0,100) → 1 Spine(+30) → 2 Neck(+1, 짧음 → 척추에 합쳐짐) → 3 Head(+29)
	//   0 → 4 LegL(0,-15,-10) → 5 ShinL(-45),  0 → 6 LegR(0,15,-10) → 7 ShinR(-45),  0 → 8 Tail(-20,0,0) → 9 TailTip(-20,0,0)
	struct FTestSkeleton
	{
		std::vector<int32>    Parents = { -1, 0, 1, 2, 0, 4, 0, 6, 0, 8 };
		std::vector<FVector3> Locals  = { { 0, 0, 100 }, { 0, 0, 30 }, { 0, 0, 1 },  { 0, 0, 29 }, { 0, -15, -10 },
			                              { 0, 0, -45 }, { 0, 15, -10 }, { 0, 0, -45 }, { -20, 0, 0 }, { -20, 0, 0 } };
		std::vector<const char*> Names = { "Hips", "Spine", "Neck", "Head", "LegL", "ShinL", "LegR", "ShinR", "Tail", "TailTip" };

		FVector3 World(int32 Node) const
		{
			FVector3 Position;
			for (int32 Current = Node; Current >= 0; Current = Parents[Current])
			{
				Position += Locals[Current];
			}
			return Position;
		}
	};

	std::vector<FRagdollBoneInput> MakeInputs(const FTestSkeleton& Skeleton, const FRagdollComponent& Settings)
	{
		std::vector<FRagdollBoneInput> Bones(Skeleton.Parents.size());
		for (size_t Node = 0; Node < Bones.size(); ++Node)
		{
			Bones[Node].Position = Skeleton.World(static_cast<int32>(Node));
			Bones[Node].Parent   = Skeleton.Parents[Node];
			Bones[Node].bJoint   = true;
			Bones[Node].bExclude = RagdollMath::MatchesExclude(Skeleton.Names[Node], Settings.ExcludeBones);
		}
		return Bones;
	}

	const FRagdollPartLayout* FindPart(const std::vector<FRagdollPartLayout>& Parts, int32 Node)
	{
		for (const FRagdollPartLayout& Part : Parts)
		{
			if (Part.Node == Node)
			{
				return &Part;
			}
		}
		return nullptr;
	}

	// 위 뼈대로 모델 엔티티 (모델 로더가 만드는 것과 같은 모양: 노드 엔티티 계층 + 애니메이션 런타임 + 스킨 Joints)
	FEntity BuildModel(FScene& Scene, const FTestSkeleton& Skeleton, std::vector<FEntity>& OutNodes)
	{
		const FEntity Root = Scene.CreateEntity("Model");
		OutNodes.assign(Skeleton.Parents.size(), NullEntity);
		std::vector<FNodePose> RestPose(Skeleton.Parents.size());
		for (size_t Node = 0; Node < Skeleton.Parents.size(); ++Node)
		{
			OutNodes[Node] = Scene.CreateEntity(Skeleton.Names[Node]);
			Scene.SetParent(OutNodes[Node], Skeleton.Parents[Node] >= 0 ? OutNodes[Skeleton.Parents[Node]] : Root);
			Scene.GetTransform(OutNodes[Node]).Position = Skeleton.Locals[Node];
			RestPose[Node].Translation                 = Skeleton.Locals[Node];
		}
		FAnimationComponent& Animation   = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
		Animation.Runtime.Set           = MakeAnimationSet({}, Skeleton.Parents, RestPose);
		Animation.Runtime.NodeEntities  = OutNodes;
		const FEntity   MeshEntity      = Scene.CreateEntity("Mesh");
		Scene.SetParent(MeshEntity, Root);
		FSkinComponent& Skin            = Scene.GetRegistry().Emplace<FSkinComponent>(MeshEntity);
		Skin.Joints                     = OutNodes;
		Skin.InverseBindMatrices.assign(OutNodes.size(), FMatrix4x4());
		Scene.UpdateTransforms();
		return Root;
	}
} // namespace

E_TEST(RagdollMath_SegmentDistanceAndRotation)
{
	E_EXPECT_NEAR(RagdollMath::SegmentDistance({ 0, 0, 0 }, { 10, 0, 0 }, { 5, 3, 0 }, { 5, 9, 0 }), 3.0f, 1.0e-4f);  // 수직 교차 아님
	E_EXPECT_NEAR(RagdollMath::SegmentDistance({ 0, 0, 0 }, { 10, 0, 0 }, { 12, 0, 0 }, { 20, 0, 0 }), 2.0f, 1.0e-4f); // 끝점끼리
	E_EXPECT_NEAR(RagdollMath::SegmentDistance({ 0, 0, 0 }, { 10, 0, 0 }, { 3, 0, 4 }, { 3, 0, 4 }), 4.0f, 1.0e-4f);   // 점
	for (const FVector3 Direction : { FVector3(1, 0, 0), FVector3(0, -1, 0), FVector3(0, 0, 1), FVector3(0, 0, -1), FVector3(0.6f, 0.0f, 0.8f) })
	{
		E_EXPECT_EQUALS(RagdollMath::RotationFromZ(Direction).RotateVector(FVector3::UpVector), Direction, 1.0e-4f);
	}
	E_EXPECT_TRUE(RagdollMath::MatchesExclude("b_Tail01_012", " ear, tail "));
	E_EXPECT_FALSE(RagdollMath::MatchesExclude("b_Head_05", "Tail,,Ear"));
	E_EXPECT_FALSE(RagdollMath::MatchesExclude("b_Head_05", ""));
}

// 배치: 짧은 목은 척추에 합쳐지고(척추 캡슐이 머리까지), 끝 뼈(머리/정강이)는 부모 방향으로 연장, 제외한 꼬리는 자손까지 빠진다
E_TEST(RagdollMath_LayoutFromSkeleton)
{
	const FTestSkeleton Skeleton;
	FRagdollComponent   Settings;
	Settings.ExcludeBones = "Tail";
	const std::vector<FRagdollPartLayout> Parts = RagdollMath::BuildLayout(MakeInputs(Skeleton, Settings), Settings);
	E_EXPECT_EQ(static_cast<int32>(Parts.size()), 7); // Hips, Spine, Head, LegL, ShinL, LegR, ShinR
	E_EXPECT_TRUE(FindPart(Parts, 2) == nullptr);     // 목 (합쳐짐)
	E_EXPECT_TRUE(FindPart(Parts, 8) == nullptr && FindPart(Parts, 9) == nullptr);

	const FRagdollPartLayout* Spine = FindPart(Parts, 1);
	const FRagdollPartLayout* Head  = FindPart(Parts, 3);
	const FRagdollPartLayout* Shin  = FindPart(Parts, 5);
	const FRagdollPartLayout* Hips  = FindPart(Parts, 0);
	E_EXPECT_TRUE(Spine != nullptr && Head != nullptr && Shin != nullptr && Hips != nullptr);
	if (Spine == nullptr || Head == nullptr || Shin == nullptr || Hips == nullptr)
	{
		return;
	}
	E_EXPECT_EQUALS(Spine->End, FVector3(0, 0, 160), 1.0e-3f);     // 목을 건너 머리 위치까지
	E_EXPECT_EQUALS(Head->End, FVector3(0, 0, 160 + 15), 1.0e-3f); // 부모(척추 30cm)의 절반 연장
	E_EXPECT_EQUALS(Shin->End, FVector3(0, -15, 45 - 22.5f), 1.0e-3f);
	E_EXPECT_TRUE(Parts[Head->ParentPart].Node == 1); // 머리 → 척추 (목 건너뜀)
	E_EXPECT_TRUE(Parts[Shin->ParentPart].Node == 4);
	E_EXPECT_EQ(Hips->ParentPart, -1);
	// 부모가 먼저 온다
	for (size_t Index = 0; Index < Parts.size(); ++Index)
	{
		E_EXPECT_TRUE(Parts[Index].ParentPart < static_cast<int32>(Index));
	}
	E_EXPECT_NEAR(Spine->Radius, 30.0f * Settings.RadiusScale, 1.0e-3f);
	E_EXPECT_EQUALS(Hips->End, FVector3(0, 0, 115), 1.0e-3f); // 자식이 퍼진 골반: 평균(위) 쪽으로 가장 먼 자식(척추 30cm)의 절반
}

// 켜기: 캡슐/관절이 생기고 애니메이션 포즈를 멈춘 채 쓰러진다(뼈 사이 거리 유지). 끄기: 바디가 사라지고 원래 로컬 포즈로 돌아온다
E_TEST(Ragdoll_EnableFallsAndDisableRestores)
{
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);
	const FTestSkeleton  Skeleton;
	std::vector<FEntity> Nodes;
	const FEntity        Model = BuildModel(Scene, Skeleton, Nodes);
	Scene.GetRegistry().Emplace<FRagdollComponent>(Model).ExcludeBones = "Tail";
	Scene.GetTransform(Model).Rotation = FQuat::FromEuler(0.0f, 0.0f, 25.0f); // 기울여 세워 둔다 (똑바로 서면 균형이 맞아 안 쓰러질 수 있다)
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	const uint32 BodiesBefore = Physics.GetBodyCount();
	E_EXPECT_TRUE(Physics.EnableRagdoll(Scene, Model));
	E_EXPECT_FALSE(Physics.EnableRagdoll(Scene, Model)); // 이미 켜짐
	E_EXPECT_TRUE(Physics.IsRagdollActive(Scene, Model));
	E_EXPECT_EQ(Physics.GetRagdollPartCount(Scene, Model), 7u);
	E_EXPECT_EQ(Physics.GetBodyCount(), BodiesBefore + 7u);
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FAnimationComponent>(Model).Runtime.bPhysicsPose);

	const float SpineLength = FVector3::Distance(Skeleton.World(1), Skeleton.World(3));
	const float ShinLength  = FVector3::Distance(Skeleton.World(4), Skeleton.World(5));
	for (int32 Index = 0; Index < 150; ++Index)
	{
		Physics.Update(Scene, Frame);
		Scene.UpdateTransforms();
	}
	const FVector3 Head = Scene.GetTransform(Nodes[3]).GetWorldPosition();
	E_EXPECT_TRUE(Head.Z < 60.0f);  // 쓰러졌다 (처음 160cm)
	E_EXPECT_TRUE(Head.Z > -5.0f);  // 바닥을 뚫지 않는다
	E_EXPECT_NEAR(FVector3::Distance(Scene.GetTransform(Nodes[1]).GetWorldPosition(), Head), SpineLength, 2.0f); // 관절이 벌어지지 않는다
	E_EXPECT_NEAR(FVector3::Distance(Scene.GetTransform(Nodes[4]).GetWorldPosition(), Scene.GetTransform(Nodes[5]).GetWorldPosition()), ShinLength, 2.0f);
	// 캡슐 없는 꼬리는 엉덩이를 따라간다 (로컬 그대로)
	E_EXPECT_EQUALS(Scene.GetTransform(Nodes[8]).Position, Skeleton.Locals[8], 1.0e-3f);

	Physics.DisableRagdoll(Scene, Model);
	E_EXPECT_FALSE(Physics.IsRagdollActive(Scene, Model));
	E_EXPECT_EQ(Physics.GetBodyCount(), BodiesBefore);
	E_EXPECT_FALSE(Scene.GetRegistry().Get<FAnimationComponent>(Model).Runtime.bPhysicsPose);
	for (size_t Node = 0; Node < Nodes.size(); ++Node)
	{
		E_EXPECT_EQUALS(Scene.GetTransform(Nodes[Node]).Position, Skeleton.Locals[Node], 1.0e-3f);
	}

	// 모델 조상에서 불러도 찾는다, 모델을 지우면 래그돌도 정리된다
	const FEntity Owner = Scene.CreateEntity("Owner");
	Scene.SetParent(Model, Owner);
	E_EXPECT_TRUE(FPhysicsSystem::FindRagdollModel(Scene, Owner) == Model);
	E_EXPECT_TRUE(Physics.EnableRagdoll(Scene, Owner));
	Scene.DestroyEntity(Owner);
	Physics.Update(Scene, Frame);
	E_EXPECT_EQ(Physics.GetBodyCount(), BodiesBefore);
	Physics.End();
}
