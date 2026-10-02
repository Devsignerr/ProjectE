#include "Core/Testing/TestFramework.h"
#include "Scene/AnimIK.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>

// 애니메이션 IK (Scene/AnimIK.h, Phase 42-4): 최소 회전, 시선 각 제한, 2본 풀이, 시스템 연동(모델 루트 회전·스케일, 골반, 시선)

namespace
{
	constexpr float Tol = 1.0e-3f;

	float AngleBetween(const FVector3& A, const FVector3& B)
	{
		return std::acos(FMath::Clamp(FVector3::Dot(A.GetNormalized(), B.GetNormalized()), -1.0f, 1.0f));
	}
} // namespace

E_TEST(AnimIK_FromToAndLookAtClamp)
{
	const FVector3 Pairs[][2] = { { FVector3(1, 0, 0), FVector3(0, 1, 0) }, { FVector3(0, 0, 2), FVector3(1, 1, 0) }, { FVector3(1, 2, 3), FVector3(-3, 1, 0.5f) },
		                          { FVector3(1, 0, 0), FVector3(-1, 0, 0) }, { FVector3(0, 0, 1), FVector3(0, 0, -5) } };
	for (const auto& Pair : Pairs)
	{
		const FQuat Rotation = AnimIKMath::FromToRotation(Pair[0], Pair[1]);
		E_EXPECT_EQUALS(Rotation.RotateVector(Pair[0].GetNormalized()), Pair[1].GetNormalized(), Tol);
	}
	E_EXPECT_EQUALS(AnimIKMath::FromToRotation(FVector3(1, 0, 0), FVector3(3, 0, 0)), FQuat::Identity, Tol);

	// 시선: 90도 떨어진 목표를 30도에서 자른다 / 가중치 0.5면 절반 / 목표 없음 = 항등
	const FQuat Clamped = AnimIKMath::ComputeLookAtDelta(FVector3::ForwardVector, FVector3::RightVector, FMath::DegreesToRadians(30.0f), 1.0f);
	const FVector3 Turned = Clamped.RotateVector(FVector3::ForwardVector);
	E_EXPECT_NEAR(AngleBetween(Turned, FVector3::ForwardVector), FMath::DegreesToRadians(30.0f), Tol);
	E_EXPECT_NEAR(AngleBetween(Turned, FVector3::RightVector), FMath::DegreesToRadians(60.0f), Tol); // 목표 쪽으로
	const FQuat Half = AnimIKMath::ComputeLookAtDelta(FVector3::ForwardVector, FVector3(0, 1, 1), FMath::Pi, 0.5f);
	E_EXPECT_NEAR(AngleBetween(Half.RotateVector(FVector3::ForwardVector), FVector3::ForwardVector), FMath::DegreesToRadians(45.0f), Tol);
	E_EXPECT_EQUALS(AnimIKMath::ComputeLookAtDelta(FVector3::ForwardVector, FVector3::ZeroVector, 1.0f, 1.0f), FQuat::Identity, Tol);
	// 정반대 목표도 각 제한 안에서 돈다 (NaN 없음)
	const FQuat Behind = AnimIKMath::ComputeLookAtDelta(FVector3::ForwardVector, -FVector3::ForwardVector, FMath::DegreesToRadians(40.0f), 1.0f);
	E_EXPECT_NEAR(AngleBetween(Behind.RotateVector(FVector3::ForwardVector), FVector3::ForwardVector), FMath::DegreesToRadians(40.0f), Tol);

	E_EXPECT_NEAR(AnimIKMath::SmoothTowards(0.0f, 10.0f, 0.0f, 0.1f), 10.0f, Tol); // 속도 0 = 즉시
	E_EXPECT_NEAR(AnimIKMath::SmoothTowards(0.0f, 10.0f, 5.0f, 0.0f), 0.0f, Tol);
	E_EXPECT_NEAR(AnimIKMath::SmoothTowards(0.0f, 10.0f, 1.0f, 1.0f), 10.0f * (1.0f - std::exp(-1.0f)), Tol);
	const std::vector<std::string> Bones = AnimIKMath::SplitBoneList(" A_1 ,B,, C ");
	E_EXPECT_EQ(Bones.size(), static_cast<size_t>(3));
	if (Bones.size() == 3)
	{
		E_EXPECT_TRUE(Bones[0] == "A_1" && Bones[1] == "B" && Bones[2] == "C");
	}
}

E_TEST(AnimIK_TwoBoneSolve)
{
	const FVector3 Root(0, 0, 100), Mid(0, 5, 55), End(0, 0, 10); // 길이 ≈ 45.3 + 45.3
	const float    Upper = (Mid - Root).Length();
	const float    Lower = (End - Mid).Length();
	const auto     Check = [&](const FVector3& Target, const FVector3& Pole) {
        const AnimIKMath::FTwoBoneResult Result = AnimIKMath::SolveTwoBone(Root, Mid, End, Target, Pole);
        // 델타를 적용한 체인: 무릎 = Root + RootDelta(Mid-Root), 끝 = 새 무릎 + MidDelta*RootDelta(End-Mid)
        const FVector3 NewMid = Root + Result.RootDelta.RotateVector(Mid - Root);
        const FVector3 NewEnd = NewMid + (Result.MidDelta * Result.RootDelta).RotateVector(End - Mid);
        E_EXPECT_NEAR((NewMid - Root).Length(), Upper, Tol); // 뼈 길이 유지
        E_EXPECT_NEAR((NewEnd - NewMid).Length(), Lower, Tol);
        E_EXPECT_EQUALS(NewEnd, Result.EndPosition, 1.0e-2f);
        return std::make_pair(Result, NewMid);
	};
	// 닿는 목표: 끝이 정확히 목표, 무릎은 지금 방향(+Y) 쪽으로 굽는다
	const FVector3 Reachable(10, -5, 30);
	auto [Result, NewMid] = Check(Reachable, FVector3::ZeroVector);
	E_EXPECT_TRUE(Result.bReachable);
	E_EXPECT_EQUALS(Result.EndPosition, Reachable, 1.0e-2f);
	const FVector3 Axis      = (Reachable - Root).GetNormalized();
	const FVector3 KneeSide  = (NewMid - Root) - Axis * FVector3::Dot(NewMid - Root, Axis);
	E_EXPECT_TRUE(KneeSide.Y > 0.0f);
	// 무릎 방향 지정 (+X): 무릎이 +X 쪽
	auto [Posed, PosedMid] = Check(Reachable, FVector3(1, 0, 0));
	const FVector3 PosedSide = (PosedMid - Root) - Axis * FVector3::Dot(PosedMid - Root, Axis);
	E_EXPECT_TRUE(PosedSide.X > 0.0f && FMath::Abs(PosedSide.GetNormalized().Y) < 0.5f);
	// 닿지 않는 목표: 목표 쪽으로 곧게 뻗는다
	const FVector3 Far(0, 0, -200);
	auto [Stretched, StretchedMid] = Check(Far, FVector3::ZeroVector);
	E_EXPECT_FALSE(Stretched.bReachable);
	E_EXPECT_NEAR((Stretched.EndPosition - Root).Length(), (Upper + Lower) * 0.9999f, 1.0e-2f);
	E_EXPECT_TRUE(AngleBetween(Stretched.EndPosition - Root, Far - Root) < 1.0e-3f);
	(void)StretchedMid;
	(void)Posed;
}

// 시스템: 모델 루트가 회전·스케일된 상태에서 발이 월드 목표 높이에 닿고, 골반은 낮은 발에 맞춰 내려가며, 시선은 목표를 본다
E_TEST(AnimIK_SystemFootAndLookAt)
{
	FScene        Scene;
	const FEntity Root = Scene.CreateEntity("Model");
	FTransformComponent& RootTransform = Scene.GetTransform(Root);
	RootTransform.Position = FVector3(100.0f, 50.0f, 10.0f);
	RootTransform.Rotation = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(30.0f));
	RootTransform.Scale    = FVector3(0.5f, 0.5f, 0.5f); // 모델 공간 = 월드 × 2
	const float RootZ      = RootTransform.Position.Z;   // 엔티티를 더 만들면 RootTransform 참조는 무효 (아래에서는 다시 얻는다)

	// 0 Hip → 1 Thigh → 2 Knee → 3 Foot,  0 Hip → 4 Neck → 5 Head
	std::vector<FNodePose> Rest(6);
	Rest[0].Translation = FVector3(0, 0, 100);
	Rest[0].Rotation    = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(15.0f));
	Rest[1].Translation = FVector3(0, 10, 0);
	Rest[1].Rotation    = FQuat::FromAxisAngle(FVector3::ForwardVector, FMath::DegreesToRadians(10.0f));
	Rest[2].Translation = FVector3(0, 0, -45);
	Rest[2].Rotation    = FQuat::FromAxisAngle(FVector3::RightVector, FMath::DegreesToRadians(-25.0f));
	Rest[3].Translation = FVector3(0, 0, -45);
	Rest[4].Translation = FVector3(10, 0, 30);
	Rest[5].Translation = FVector3(10, 0, 10);
	const std::vector<int32>       Parents = { -1, 0, 1, 2, 0, 4 };
	const std::vector<const char*> Names   = { "Hip", "Thigh", "Knee", "Foot", "Neck", "Head" };
	FAnimationClip                 Idle;
	Idle.Name     = "Idle";
	Idle.Duration = 1.0f;
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Runtime.Set          = MakeAnimationSet({ Idle }, Parents, Rest);
	for (size_t Node = 0; Node < Rest.size(); ++Node)
	{
		const FEntity Entity = Scene.CreateEntity(Names[Node]);
		Scene.SetParent(Entity, Parents[Node] < 0 ? Root : Animation.Runtime.NodeEntities[static_cast<size_t>(Parents[Node])]);
		FTransformComponent& Transform = Scene.GetTransform(Entity);
		Transform.Position             = Rest[Node].Translation;
		Transform.Rotation             = Rest[Node].Rotation;
		Animation.Runtime.NodeEntities.push_back(Entity);
	}
	const FEntity Hip  = Animation.Runtime.NodeEntities[0];
	const FEntity Foot = Animation.Runtime.NodeEntities[3];
	const FEntity Head = Animation.Runtime.NodeEntities[5];
	FFootIkComponent& FootIk = Scene.GetRegistry().Emplace<FFootIkComponent>(Root);
	FootIk.FootBones         = "Foot";
	FootIk.PelvisBone        = "Hip";
	FootIk.InterpSpeed       = 0.0f; // 즉시
	FootIk.MaxAdjust         = 100.0f;
	FootIk.bAlignToGround    = false;

	Scene.UpdateTransforms();
	const FVector3 AnimatedFoot = Scene.GetTransform(Foot).GetWorldPosition();
	const FVector3 AnimatedHip  = Scene.GetTransform(Hip).GetWorldPosition();
	FAnimationSystem::Update(Scene, 1.0f / 60.0f); // 뼈 찾기 + 탐색 위치 기록 (탐색 결과 없음 → 그대로)
	Scene.UpdateTransforms();
	E_EXPECT_EQ(FootIk.Runtime.Feet.size(), static_cast<size_t>(1));
	E_EXPECT_EQUALS(Scene.GetTransform(Foot).GetWorldPosition(), AnimatedFoot, Tol);
	if (FootIk.Runtime.Feet.size() != 1)
	{
		return;
	}
	// World가 넣는 탐색 위치 = IK 전 발 (모델 공간 → 루트 월드 행렬이면 애니메이션 발 월드 위치)
	E_EXPECT_EQUALS(Scene.GetTransform(Root).WorldMatrix.TransformPosition(FootIk.Runtime.Feet[0].ProbeModelPosition), AnimatedFoot, 1.0e-2f);

	// 바닥이 루트보다 20cm 높다 → 발 +20cm (골반은 내리기만 하므로 그대로)
	const auto Probe = [&](float GroundZ) {
		FFootIkFoot& Data       = FootIk.Runtime.Feet[0];
		Data.bHit               = true;
		Data.HitPoint           = FVector3(AnimatedFoot.X, AnimatedFoot.Y, GroundZ);
		Data.HitNormal          = FVector3::UpVector;
		FootIk.Runtime.ProbeAge = 0.0f;
		FAnimationSystem::Update(Scene, 1.0f / 60.0f);
		Scene.UpdateTransforms();
	};
	Probe(RootZ + 20.0f);
	E_EXPECT_EQUALS(Scene.GetTransform(Foot).GetWorldPosition(), AnimatedFoot + FVector3(0, 0, 20.0f), 2.0e-2f);
	E_EXPECT_EQUALS(Scene.GetTransform(Hip).GetWorldPosition(), AnimatedHip, 2.0e-2f);
	// 15cm 낮다 → 골반 -15, 발 -15
	Probe(RootZ - 15.0f);
	E_EXPECT_NEAR(FootIk.Runtime.PelvisOffset, -15.0f, Tol);
	E_EXPECT_EQUALS(Scene.GetTransform(Hip).GetWorldPosition(), AnimatedHip - FVector3(0, 0, 15.0f), 2.0e-2f);
	E_EXPECT_EQUALS(Scene.GetTransform(Foot).GetWorldPosition(), AnimatedFoot - FVector3(0, 0, 15.0f), 2.0e-2f);
	// 탐색이 오래되면(에디터 편집 중 등) 보정이 0으로 돌아간다
	FootIk.Runtime.ProbeAge = 10.0f;
	FAnimationSystem::Update(Scene, 1.0f / 60.0f);
	Scene.UpdateTransforms();
	E_EXPECT_EQUALS(Scene.GetTransform(Foot).GetWorldPosition(), AnimatedFoot, 2.0e-2f);
	// 래그돌 중에는 갱신하지 않고 보정도 0
	Probe(RootZ - 15.0f);
	Animation.Runtime.bPhysicsPose = true;
	FAnimationSystem::Update(Scene, 1.0f / 60.0f);
	E_EXPECT_NEAR(FootIk.Runtime.PelvisOffset, 0.0f, Tol);
	E_EXPECT_NEAR(FootIk.Runtime.Feet[0].Offset, 0.0f, Tol);
	Animation.Runtime.bPhysicsPose = false;
	Scene.GetRegistry().Remove<FFootIkComponent>(Root);

	// 시선: 기본 포즈 Head 앞 = 모델 +X. 목표 엔티티를 머리 옆 60도에 두면 (제한 90) 정확히 본다, 제한 20이면 20도만
	FLookAtComponent& LookAt = Scene.GetRegistry().Emplace<FLookAtComponent>(Root);
	LookAt.Bones             = "Neck, Head";
	LookAt.BlendSpeed        = 0.0f;
	LookAt.MaxAngle          = 90.0f;
	FAnimationSystem::Update(Scene, 1.0f / 60.0f);
	Scene.UpdateTransforms();
	const FVector3 HeadPosition = Scene.GetTransform(Head).GetWorldPosition();
	const FVector3 RestForward  = Scene.GetTransform(Head).WorldMatrix.TransformVector(LookAt.Runtime.LocalForward).GetNormalized(); // 머리 로컬 앞 축 (기본 포즈에서 모델 +X)
	const FVector3 Side         = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(60.0f)).RotateVector(RestForward);
	const FEntity  Ball         = Scene.CreateEntity("Ball");
	Scene.GetTransform(Ball).Position = HeadPosition + Side * 300.0f;
	Scene.UpdateTransforms();
	LookAt.Target = Ball;
	FAnimationSystem::Update(Scene, 1.0f / 60.0f);
	Scene.UpdateTransforms();
	const auto HeadForward = [&]() { return Scene.GetTransform(Head).WorldMatrix.TransformVector(LookAt.Runtime.LocalForward).GetNormalized(); };
	const auto ToBall      = [&]() { return Scene.GetTransform(Ball).GetWorldPosition() - Scene.GetTransform(Head).GetWorldPosition(); };
	E_EXPECT_TRUE(AngleBetween(HeadForward(), ToBall()) < FMath::DegreesToRadians(2.0f)); // 머리가 조금 움직이므로 근사
	LookAt.MaxAngle = 20.0f;
	FAnimationSystem::Update(Scene, 1.0f / 60.0f);
	Scene.UpdateTransforms();
	E_EXPECT_NEAR(AngleBetween(HeadForward(), RestForward), FMath::DegreesToRadians(20.0f), FMath::DegreesToRadians(1.0f));
	// Lua/C++ 목표(점)가 엔티티보다 먼저, 지우면 다시 엔티티
	LookAt.MaxAngle = 90.0f;
	const FVector3 Up = HeadPosition + FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(-45.0f)).RotateVector(RestForward) * 300.0f;
	E_EXPECT_TRUE(FAnimationSystem::SetLookAtTarget(Scene, Root, Up));
	FAnimationSystem::Update(Scene, 1.0f / 60.0f);
	Scene.UpdateTransforms();
	E_EXPECT_TRUE(AngleBetween(HeadForward(), Up - Scene.GetTransform(Head).GetWorldPosition()) < FMath::DegreesToRadians(2.0f));
	E_EXPECT_TRUE(FAnimationSystem::ClearLookAtTarget(Scene, Root));
	LookAt.Target = NullEntity;
	FAnimationSystem::Update(Scene, 1.0f / 60.0f); // 목표 없음 → 가중치 0 (BlendSpeed 0 = 즉시)
	Scene.UpdateTransforms();
	E_EXPECT_TRUE(AngleBetween(HeadForward(), RestForward) < 1.0e-3f);
	E_EXPECT_FALSE(FAnimationSystem::SetLookAtTarget(Scene, Ball, Up)); // 컴포넌트 없음
}
