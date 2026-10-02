#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Scene/AnimRetarget.h"
#include "Scene/AnimRootMotion.h"
#include "Scene/Animation.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

// 리타기팅(Scene/AnimRetarget.h) + 루트 모션(Scene/AnimRootMotion.h) 순수 식과 시스템 연동

namespace
{
	constexpr float Tol = 1.0e-3f;

	struct FBoneDesc
	{
		const char* Name;
		int32       Parent;
		FVector3    ModelPosition;
		FQuat       ModelRotation = FQuat::Identity;
	};

	// 모델 공간 위치/회전으로 기본 포즈 구성 (균등 스케일 1)
	std::shared_ptr<FAnimationSet> MakeSkeletonSet(const std::vector<FBoneDesc>& Bones, std::vector<FAnimationClip> Clips = {})
	{
		std::vector<int32>       Parents;
		std::vector<FNodePose>   Rest;
		std::vector<std::string> Names;
		for (const FBoneDesc& Bone : Bones)
		{
			FNodePose Pose;
			if (Bone.Parent >= 0)
			{
				const FBoneDesc& Parent = Bones[static_cast<size_t>(Bone.Parent)];
				Pose.Rotation           = (Parent.ModelRotation.Inverse() * Bone.ModelRotation).GetNormalized();
				Pose.Translation        = Parent.ModelRotation.UnrotateVector(Bone.ModelPosition - Parent.ModelPosition);
			}
			else
			{
				Pose.Rotation    = Bone.ModelRotation;
				Pose.Translation = Bone.ModelPosition;
			}
			Parents.push_back(Bone.Parent);
			Rest.push_back(Pose);
			Names.emplace_back(Bone.Name);
		}
		return MakeAnimationSet(std::move(Clips), std::move(Parents), std::move(Rest), std::move(Names));
	}

	FAnimationChannel MakeChannel(int32 Node, EAnimationPath Path, std::vector<float> Times, std::vector<FVector4> Values)
	{
		FAnimationChannel Channel;
		Channel.Node   = Node;
		Channel.Path   = Path;
		Channel.Times  = std::move(Times);
		Channel.Values = std::move(Values);
		return Channel;
	}

	FVector4 ToVector(const FQuat& Q) { return FVector4(Q.X, Q.Y, Q.Z, Q.W); }

	int32 Map(const FHumanoidMapping& Mapping, EHumanoidBone Bone) { return Mapping[static_cast<size_t>(Bone)]; }

	// 소스: Mixamo 이름, 전신 1배 (Hips 높이 100), T 포즈 팔 (+Y = 왼쪽), 모든 뼈 로컬 회전 항등
	std::vector<FBoneDesc> MakeSourceBones()
	{
		return {
			{ "mixamorig:Hips", -1, FVector3(0, 0, 100) },          // 0
			{ "mixamorig:Spine", 0, FVector3(0, 0, 120) },          // 1
			{ "mixamorig:Head", 1, FVector3(0, 0, 150) },           // 2
			{ "mixamorig:LeftUpLeg", 0, FVector3(0, 10, 100) },     // 3
			{ "mixamorig:LeftLeg", 3, FVector3(0, 10, 55) },        // 4
			{ "mixamorig:LeftFoot", 4, FVector3(0, 10, 10) },       // 5
			{ "mixamorig:RightUpLeg", 0, FVector3(0, -10, 100) },   // 6
			{ "mixamorig:RightLeg", 6, FVector3(0, -10, 55) },      // 7
			{ "mixamorig:RightFoot", 7, FVector3(0, -10, 10) },     // 8
			{ "mixamorig:LeftArm", 1, FVector3(0, 15, 140) },       // 9
			{ "mixamorig:LeftForeArm", 9, FVector3(0, 40, 140) },   // 10
			{ "mixamorig:LeftHand", 10, FVector3(0, 65, 140) },     // 11
		};
	}

	// 대상: UE 이름, 절반 크기 (pelvis 높이 50), A 포즈 팔 (아래 45°), 뼈마다 다른 로컬 축 (기본 회전이 제각각)
	std::vector<FBoneDesc> MakeTargetBones()
	{
		const FQuat    AxisA = FQuat::FromAxisAngle(FVector3::ForwardVector, FMath::DegreesToRadians(90.0f));
		const FQuat    AxisB = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(-60.0f));
		const FVector3 ArmDirection(0.0f, std::sqrt(0.5f), -std::sqrt(0.5f));
		return {
			{ "root", -1, FVector3(0, 0, 0) },                                       // 0
			{ "pelvis", 0, FVector3(0, 0, 50), AxisA },                              // 1
			{ "spine_01", 1, FVector3(0, 0, 60), AxisB },                            // 2
			{ "head", 2, FVector3(0, 0, 75), AxisA },                                // 3
			{ "thigh_l", 1, FVector3(0, 5, 50), AxisB },                             // 4
			{ "calf_l", 4, FVector3(0, 5, 27.5f), AxisA },                           // 5
			{ "foot_l", 5, FVector3(0, 5, 5), AxisB },                               // 6
			{ "thigh_r", 1, FVector3(0, -5, 50), AxisA },                            // 7
			{ "calf_r", 7, FVector3(0, -5, 27.5f), AxisB },                          // 8
			{ "foot_r", 8, FVector3(0, -5, 5), AxisA },                              // 9
			{ "upperarm_l", 2, FVector3(0, 7.5f, 70), AxisB },                       // 10
			{ "lowerarm_l", 10, FVector3(0, 7.5f, 70) + ArmDirection * 12.5f, AxisA }, // 11
			{ "hand_l", 11, FVector3(0, 7.5f, 70) + ArmDirection * 25.0f, AxisB },     // 12
		};
	}

	struct FRetargetFixture
	{
		std::shared_ptr<FAnimationSet> Source;
		std::shared_ptr<FAnimationSet> Target;
		FRetargetSkeleton              SourceSkeleton;
		FRetargetSkeleton              TargetSkeleton;
		FRetargetPlan                  Plan;
	};

	void InitFixture(FRetargetFixture& Fixture, std::vector<FAnimationClip> Clips)
	{
		Fixture.Source         = MakeSkeletonSet(MakeSourceBones(), std::move(Clips));
		Fixture.Target         = MakeSkeletonSet(MakeTargetBones());
		Fixture.SourceSkeleton = AnimRetargetMath::MakeSkeleton(*Fixture.Source, AnimRetargetMath::ResolveMapping(*Fixture.Source, nullptr));
		Fixture.TargetSkeleton = AnimRetargetMath::MakeSkeleton(*Fixture.Target, AnimRetargetMath::ResolveMapping(*Fixture.Target, nullptr));
		Fixture.Plan           = AnimRetargetMath::MakePlan(Fixture.SourceSkeleton, Fixture.TargetSkeleton);
	}

	// 대상 세트에서 리타기팅 클립을 Time에 샘플한 모델 행렬
	std::vector<FMatrix4x4> EvaluateTarget(const FAnimationSet& Target, const FAnimationClip& Clip, float Time)
	{
		std::vector<FNodePose> Pose = Target.RestPose;
		AnimationMath::SampleClip(Clip, Time, Pose);
		std::vector<FMatrix4x4> Matrices;
		AnimationMath::ComputeModelMatrices(Pose, Target.NodeParents, Matrices);
		return Matrices;
	}

	FVector3 Direction(const std::vector<FMatrix4x4>& Matrices, int32 From, int32 To)
	{
		return (Matrices[static_cast<size_t>(To)].GetOrigin() - Matrices[static_cast<size_t>(From)].GetOrigin()).GetNormalized();
	}
} // namespace

// ---------------------------------------------------------------- 리타기팅

E_TEST(AnimRetarget_ParseBoneNameConventions)
{
	const FBoneNameInfo Mixamo = AnimRetargetMath::ParseBoneName("mixamorig:LeftUpLeg");
	E_EXPECT_TRUE(Mixamo.Side == EBoneSide::Left && Mixamo.Base == "upleg");
	const FBoneNameInfo Unreal = AnimRetargetMath::ParseBoneName("upperarm_r");
	E_EXPECT_TRUE(Unreal.Side == EBoneSide::Right && Unreal.Base == "upperarm");
	const FBoneNameInfo KayKit = AnimRetargetMath::ParseBoneName("lowerleg.l");
	E_EXPECT_TRUE(KayKit.Side == EBoneSide::Left && KayKit.Base == "lowerleg");
	const FBoneNameInfo Blender = AnimRetargetMath::ParseBoneName("Armature|Shoulder.R");
	E_EXPECT_TRUE(Blender.Side == EBoneSide::Right && Blender.Base == "shoulder");
	const FBoneNameInfo Prefix = AnimRetargetMath::ParseBoneName("L_Foot");
	E_EXPECT_TRUE(Prefix.Side == EBoneSide::Left && Prefix.Base == "foot");
	const FBoneNameInfo Numbered = AnimRetargetMath::ParseBoneName("spine_02");
	E_EXPECT_TRUE(Numbered.Side == EBoneSide::None && Numbered.Base == "spine");
	const FBoneNameInfo Center = AnimRetargetMath::ParseBoneName("Hips");
	E_EXPECT_TRUE(Center.Side == EBoneSide::None && Center.Base == "hips");
	E_EXPECT_TRUE(AnimRetargetMath::FindBone("LeftUpperArm") == EHumanoidBone::LeftUpperArm);
	E_EXPECT_TRUE(AnimRetargetMath::FindBone("Tail") == EHumanoidBone::Count);
}

E_TEST(AnimRetarget_AutoMapNameRulesAndHierarchy)
{
	// UE 마네킹: spine_01~03 사슬 → Spine/Chest/UpperChest, clavicle → Shoulder, ball → Toes, 손가락은 매핑 안 함
	const std::shared_ptr<FAnimationSet> Unreal = MakeSkeletonSet({
		{ "root", -1, FVector3() },                    // 0
		{ "pelvis", 0, FVector3(0, 0, 90) },           // 1
		{ "spine_01", 1, FVector3(0, 0, 100) },        // 2
		{ "spine_02", 2, FVector3(0, 0, 110) },        // 3
		{ "spine_03", 3, FVector3(0, 0, 120) },        // 4
		{ "neck_01", 4, FVector3(0, 0, 140) },         // 5
		{ "head", 5, FVector3(0, 0, 150) },            // 6
		{ "clavicle_l", 4, FVector3(0, 5, 135) },      // 7
		{ "upperarm_l", 7, FVector3(0, 15, 135) },     // 8
		{ "lowerarm_l", 8, FVector3(0, 40, 135) },     // 9
		{ "hand_l", 9, FVector3(0, 65, 135) },         // 10
		{ "index_01_l", 10, FVector3(0, 70, 135) },    // 11
		{ "thigh_r", 1, FVector3(0, -10, 90) },        // 12
		{ "calf_r", 12, FVector3(0, -10, 50) },        // 13
		{ "foot_r", 13, FVector3(0, -10, 10) },        // 14
		{ "ball_r", 14, FVector3(-10, -10, 0) },       // 15
	});
	const FHumanoidMapping U = AnimRetargetMath::ResolveMapping(*Unreal, nullptr);
	E_EXPECT_EQ(Map(U, EHumanoidBone::Root), 0);
	E_EXPECT_EQ(Map(U, EHumanoidBone::Hips), 1);
	E_EXPECT_EQ(Map(U, EHumanoidBone::Spine), 2);
	E_EXPECT_EQ(Map(U, EHumanoidBone::Chest), 3);
	E_EXPECT_EQ(Map(U, EHumanoidBone::UpperChest), 4);
	E_EXPECT_EQ(Map(U, EHumanoidBone::Neck), 5);
	E_EXPECT_EQ(Map(U, EHumanoidBone::Head), 6);
	E_EXPECT_EQ(Map(U, EHumanoidBone::LeftShoulder), 7);
	E_EXPECT_EQ(Map(U, EHumanoidBone::LeftHand), 10);
	E_EXPECT_EQ(Map(U, EHumanoidBone::RightUpperLeg), 12);
	E_EXPECT_EQ(Map(U, EHumanoidBone::RightToes), 15);
	E_EXPECT_EQ(Map(U, EHumanoidBone::RightUpperArm), -1);

	// KayKit: wrist가 손 (hand보다 우선), 목 없음 → 척추 사슬 spine/chest, IK 뼈는 무시
	const std::shared_ptr<FAnimationSet> KayKit = MakeSkeletonSet({
		{ "Rig", -1, FVector3() },                     // 0
		{ "root", 0, FVector3() },                     // 1
		{ "hips", 1, FVector3(0, 0, 40) },             // 2
		{ "spine", 2, FVector3(0, 0, 60) },            // 3
		{ "chest", 3, FVector3(0, 0, 95) },            // 4
		{ "head", 4, FVector3(0, 0, 120) },            // 5
		{ "upperarm.l", 4, FVector3(0, 20, 110) },     // 6
		{ "lowerarm.l", 6, FVector3(0, 40, 110) },     // 7
		{ "wrist.l", 7, FVector3(0, 60, 110) },        // 8
		{ "hand.l", 8, FVector3(0, 67, 110) },         // 9
		{ "upperleg.l", 2, FVector3(0, 17, 40) },      // 10
		{ "lowerleg.l", 10, FVector3(0, 17, 20) },     // 11
		{ "foot.l", 11, FVector3(0, 17, 5) },          // 12
		{ "IK-foot.l", 1, FVector3(0, 17, 5) },        // 13
	});
	const FHumanoidMapping K = AnimRetargetMath::ResolveMapping(*KayKit, nullptr);
	E_EXPECT_EQ(Map(K, EHumanoidBone::Root), 1);
	E_EXPECT_EQ(Map(K, EHumanoidBone::Hips), 2);
	E_EXPECT_EQ(Map(K, EHumanoidBone::Spine), 3);
	E_EXPECT_EQ(Map(K, EHumanoidBone::Chest), 4);
	E_EXPECT_EQ(Map(K, EHumanoidBone::UpperChest), -1);
	E_EXPECT_EQ(Map(K, EHumanoidBone::Neck), -1);
	E_EXPECT_EQ(Map(K, EHumanoidBone::Head), 5);
	E_EXPECT_EQ(Map(K, EHumanoidBone::LeftShoulder), -1); // 위팔 부모가 척추 사슬
	E_EXPECT_EQ(Map(K, EHumanoidBone::LeftHand), 8);
	E_EXPECT_EQ(Map(K, EHumanoidBone::LeftFoot), 12);

	// 좌우 토큰이 없는 이름: 기본 포즈 Y로 (+Y = 왼쪽)
	const std::shared_ptr<FAnimationSet> Plain = MakeSkeletonSet({
		{ "Hips", -1, FVector3(0, 0, 90) },
		{ "Thigh", 0, FVector3(0, 9, 90) },
		{ "Thigh", 0, FVector3(0, -9, 90) },
	});
	const FHumanoidMapping P = AnimRetargetMath::ResolveMapping(*Plain, nullptr);
	E_EXPECT_EQ(Map(P, EHumanoidBone::LeftUpperLeg), 1);
	E_EXPECT_EQ(Map(P, EHumanoidBone::RightUpperLeg), 2);

	// .emeta 수동 지정이 자동 추정을 덮는다 (빈 이름 = 매핑 안 함)
	FModelMetadata Metadata;
	const std::string Wrist = "hand.l";
	const std::string None;
	Metadata.SetRetargetBone("LeftHand", &Wrist);
	Metadata.SetRetargetBone("Head", &None);
	const FHumanoidMapping Overridden = AnimRetargetMath::ResolveMapping(*KayKit, &Metadata);
	E_EXPECT_EQ(Map(Overridden, EHumanoidBone::LeftHand), 9);
	E_EXPECT_EQ(Map(Overridden, EHumanoidBone::Head), -1);
	// JSON 왕복 (형식 버전 2)
	FModelMetadata Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Metadata.ToJsonString()));
	E_EXPECT_EQ(Loaded.RetargetBones.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Loaded.FindRetargetBone("LeftHand") != nullptr && Loaded.FindRetargetBone("LeftHand")->Node == "hand.l");
	E_EXPECT_FALSE(Loaded.IsEmpty());
}

E_TEST(AnimRetarget_BindCorrectedRotationAndHeightRatio)
{
	// Gd = Gs · Gs(bind)⁻¹ · Gd(ref): 소스가 바인드에서 R만큼 돌면 대상도 자기 기준에서 R만큼
	const FQuat SourceBind = FQuat::FromAxisAngle(FVector3::ForwardVector, 0.7f);
	const FQuat TargetRef  = FQuat::FromAxisAngle(FVector3::UpVector, -1.1f);
	const FQuat Delta      = FQuat::FromAxisAngle(FVector3(0.0f, 0.6f, 0.8f), 0.5f);
	E_EXPECT_EQUALS(AnimRetargetMath::RetargetRotation(SourceBind, Delta * SourceBind, TargetRef), Delta * TargetRef, Tol);
	E_EXPECT_EQUALS(AnimRetargetMath::RetargetRotation(SourceBind, SourceBind, TargetRef), TargetRef, Tol);

	E_EXPECT_NEAR(AnimRetargetMath::ComputeHeightRatio(100.0f, 50.0f), 0.5f, Tol);
	E_EXPECT_NEAR(AnimRetargetMath::ComputeHeightRatio(0.5f, 50.0f), 1.0f, Tol); // 너무 낮으면 1

	const FQuat Between = AnimRetargetMath::MakeRotationBetween(FVector3(0, 1, 0), FVector3(0, 0, -1));
	E_EXPECT_EQUALS(Between.RotateVector(FVector3(0, 1, 0)), FVector3(0, 0, -1), Tol);
	E_EXPECT_EQUALS(AnimRetargetMath::MakeRotationBetween(FVector3(1, 0, 0), FVector3(-1, 0, 0)).RotateVector(FVector3(1, 0, 0)), FVector3(-1, 0, 0), Tol);
}

E_TEST(AnimRetarget_ClipBetweenDifferentSkeletons)
{
	// 소스 클립: 1초 동안 왼다리를 앞으로 (모델 Y축 기준 -40° — 발이 -X(정면)로), Hips 이동 X -20
	const FQuat   Kick = FQuat::FromAxisAngle(FVector3::RightVector, FMath::DegreesToRadians(-40.0f));
	FAnimationClip Clip;
	Clip.Name     = "Kick";
	Clip.Duration = 1.0f;
	Clip.Channels.push_back(MakeChannel(3, EAnimationPath::Rotation, { 0.0f, 1.0f }, { ToVector(FQuat::Identity), ToVector(Kick) }));
	Clip.Channels.push_back(MakeChannel(0, EAnimationPath::Translation, { 0.0f, 1.0f }, { FVector4(0, 0, 100, 0), FVector4(-20, 0, 100, 0) }));
	FRetargetFixture Fixture;
	InitFixture(Fixture, { Clip });

	E_EXPECT_NEAR(Fixture.Plan.HeightRatio, 0.5f, Tol);
	E_EXPECT_TRUE(Fixture.Plan.bActive[static_cast<size_t>(EHumanoidBone::LeftLowerArm)] != 0);
	E_EXPECT_TRUE(Fixture.Plan.bActive[static_cast<size_t>(EHumanoidBone::Root)] == 0); // 소스에 root 없음

	const FAnimationClip Retargeted = AnimRetargetMath::RetargetClip(Fixture.Source->Clips[0], Fixture.Plan);
	E_EXPECT_TRUE(Retargeted.Name == "Kick");
	E_EXPECT_NEAR(Retargeted.Duration, 1.0f, Tol);

	// 시각 0 (소스 = 바인드): A 포즈 팔이 소스 T 포즈 방향(+Y)으로 정렬, 다리·골반은 대상 바인드 위치
	const std::vector<FMatrix4x4> Start = EvaluateTarget(*Fixture.Target, Retargeted, 0.0f);
	E_EXPECT_EQUALS(Direction(Start, 10, 11), FVector3(0, 1, 0), Tol);
	E_EXPECT_EQUALS(Direction(Start, 11, 12), FVector3(0, 1, 0), Tol);
	E_EXPECT_EQUALS(Start[1].GetOrigin(), FVector3(0, 0, 50), Tol);
	E_EXPECT_EQUALS(Direction(Start, 4, 5), FVector3(0, 0, -1), Tol);

	// 시각 1: 대상 허벅지 방향 = 소스 허벅지 방향 (로컬 축이 달라도), 골반 이동 = -20 × 0.5
	const std::vector<FMatrix4x4> End = EvaluateTarget(*Fixture.Target, Retargeted, 1.0f);
	const FVector3                Expected = Kick.RotateVector(FVector3(0, 0, -1));
	E_EXPECT_EQUALS(Direction(End, 4, 5), Expected, Tol);
	E_EXPECT_EQUALS(Direction(End, 5, 6), Expected, Tol); // 무릎 아래도 따라간다 (소스 정강이 회전 없음)
	E_EXPECT_EQUALS(End[1].GetOrigin(), FVector3(-10, 0, 50), Tol);
	E_EXPECT_EQUALS(Direction(End, 7, 8), FVector3(0, 0, -1), Tol); // 오른다리 그대로
	// 손가락 등 매핑 없는 노드(대상 root)는 바인드 유지
	E_EXPECT_EQUALS(End[0].GetOrigin(), FVector3(), Tol);
}

E_TEST(AnimRetarget_SystemAppendsClipsWithAliasesAndSourceNotifies)
{
	FAnimationClip Clip;
	Clip.Name     = "Kick";
	Clip.Duration = 1.0f;
	Clip.Channels.push_back(MakeChannel(3, EAnimationPath::Rotation, { 0.0f, 1.0f },
	                                    { ToVector(FQuat::Identity), ToVector(FQuat::FromAxisAngle(FVector3::RightVector, -0.6f)) }));
	auto SourceMetadata = std::make_shared<FModelMetadata>();
	SourceMetadata->GetOrAddNotifies("Kick").push_back({ "Hit", EAnimNotifyKind::Notify, 0.25f, 0.0f });
	FAnimRetargetLibrary& Library = FAnimRetargetLibrary::Get();
	Library.AddSource("Tests/RetargetSource.glb", { MakeSkeletonSet(MakeSourceBones(), { Clip }), SourceMetadata });
	Library.AddSource("Tests/RetargetOther.glb", { MakeSkeletonSet(MakeSourceBones(), { Clip }), nullptr });

	// 대상 모델 배치 (자기 클립 "Idle" 하나)
	FAnimationClip Idle;
	Idle.Name     = "Idle";
	Idle.Duration = 1.0f;
	FScene                         Scene;
	const FEntity                  Root   = Scene.CreateEntity("Target");
	std::shared_ptr<FAnimationSet> Target = MakeSkeletonSet(MakeTargetBones(), { Idle });
	std::vector<FEntity>           Nodes;
	for (size_t Node = 0; Node < Target->NodeParents.size(); ++Node)
	{
		const FEntity Entity = Scene.CreateEntity(Target->NodeNames[Node]);
		const int32   Parent = Target->NodeParents[Node];
		Scene.SetParent(Entity, Parent >= 0 ? Nodes[static_cast<size_t>(Parent)] : Root);
		Scene.GetTransform(Entity).Position = Target->RestPose[Node].Translation;
		Scene.GetTransform(Entity).Rotation = Target->RestPose[Node].Rotation;
		Nodes.push_back(Entity);
	}
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Runtime.Set          = Target;
	Animation.Runtime.NodeEntities = Nodes;
	Animation.RetargetSources      = "Tests/RetargetSource.glb";
	Animation.Clip                 = "Kick";

	FAnimationSystem::Update(Scene, 0.0f);
	const std::vector<std::string> Names = FAnimationSystem::GetClipNames(Scene, Root);
	E_EXPECT_EQ(Names.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Names[0] == "Idle" && Names[1] == "Kick");
	E_EXPECT_TRUE(FAnimationSystem::GetCurrentClip(Scene, Root) == "Kick");
	E_EXPECT_TRUE(Animation.Runtime.BaseSet.get() == Target.get());

	// 노티파이는 소스 .emeta 기준
	FAnimationSystem::Update(Scene, 0.5f);
	const auto Hit = std::find_if(Animation.Runtime.PendingNotifies.begin(), Animation.Runtime.PendingNotifies.end(),
	                              [](const FAnimNotifyEvent& Event) { return Event.Name == "Hit" && Event.Clip == "Kick"; });
	E_EXPECT_TRUE(Hit != Animation.Runtime.PendingNotifies.end());
	// 왼 허벅지 엔티티가 움직였다 (리타기팅 포즈 기록)
	E_EXPECT_FALSE(Scene.GetTransform(Nodes[4]).Rotation.Equals(Target->RestPose[4].Rotation, 1.0e-3f));

	// "<모델>:<클립>" 이름: 소스 목록에 없는 모델도 Play가 요청해 덧붙인다. 같은 이름은 별칭으로 구분
	E_EXPECT_TRUE(FAnimationSystem::Play(Scene, Root, "Tests/RetargetOther.glb:Kick", 0.0f));
	E_EXPECT_EQ(Animation.Runtime.Set->Clips.size(), static_cast<size_t>(3));
	const std::vector<std::string> After = FAnimationSystem::GetClipNames(Scene, Root);
	E_EXPECT_TRUE(After.size() == 3 && After[2] == "Tests/RetargetOther.glb:Kick");
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(FAnimationSystem::GetCurrentClip(Scene, Root) == "Kick");

	// 소스를 비우면 모델 자신의 세트로 돌아간다 (요청한 소스는 유지되므로 그것만)
	Animation.RetargetSources.clear();
	Animation.Runtime.RequestedSources.clear();
	Animation.Clip = "Idle";
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(Animation.Runtime.Set.get() == Target.get());
	Library.Invalidate();
}

// ---------------------------------------------------------------- 루트 모션

namespace
{
	// 노드 0(루트) 하나. "Walk": X 0→100 (1초, 루프용), Z 0→10→0 / "Turn": X 0→100 + Yaw 0→90°
	std::shared_ptr<FAnimationSet> MakeRootMotionSet()
	{
		FAnimationClip Walk;
		Walk.Name     = "Walk";
		Walk.Duration = 1.0f;
		Walk.Channels.push_back(MakeChannel(0, EAnimationPath::Translation, { 0.0f, 0.5f, 1.0f },
		                                    { FVector4(0, 0, 0, 0), FVector4(50, 0, 10, 0), FVector4(100, 0, 0, 0) }));
		FAnimationClip Turn;
		Turn.Name     = "Turn";
		Turn.Duration = 1.0f;
		Turn.Channels.push_back(MakeChannel(0, EAnimationPath::Translation, { 0.0f, 1.0f }, { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) }));
		Turn.Channels.push_back(MakeChannel(0, EAnimationPath::Rotation, { 0.0f, 1.0f },
		                                    { ToVector(FQuat::Identity), ToVector(FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi)) }));
		FAnimationClip Dash;
		Dash.Name     = "Dash";
		Dash.Duration = 1.0f;
		Dash.Channels.push_back(MakeChannel(0, EAnimationPath::Translation, { 0.0f, 1.0f }, { FVector4(0, 0, 0, 0), FVector4(300, 0, 0, 0) }));
		std::vector<FNodePose> Rest(2);
		Rest[1].Translation = FVector3(0, 0, 50);
		return MakeAnimationSet({ Walk, Turn, Dash }, { -1, 0 }, Rest, { "root", "child" });
	}
} // namespace

E_TEST(RootMotion_ExtractAndRemoveFromPose)
{
	const std::shared_ptr<FAnimationSet> Set = MakeRootMotionSet();
	E_EXPECT_EQ(Set->ClipRootMotionNodes[0], 0);
	const FRootMotionTrack Track = RootMotionMath::MakeTrack(*Set, 0, -1, false);
	E_EXPECT_TRUE(Track.IsValid());
	const FRootMotionDelta Delta = RootMotionMath::ComputeSegment(Track, 0.25f, 0.75f);
	E_EXPECT_EQUALS(Delta.Translation, FVector3(50, 0, 0), Tol); // 수직은 빼고
	E_EXPECT_NEAR(Delta.Yaw, 0.0f, Tol);

	// 제자리화: XY는 시작 위치, Z(위아래 흔들림)는 남는다
	FNodePose Pose;
	Pose.Translation = FVector3(50, 0, 10);
	RootMotionMath::RemoveFromPose(Track, 0.5f, Pose);
	E_EXPECT_EQUALS(Pose.Translation, FVector3(0, 0, 10), Tol);
}

E_TEST(RootMotion_LoopBoundaryAccumulates)
{
	// 0.1초씩 25번 (2.5바퀴): 이동 합 = 250, 루프 경계마다 끊김 없음
	const std::shared_ptr<FAnimationSet> Set   = MakeRootMotionSet();
	const FRootMotionTrack               Track = RootMotionMath::MakeTrack(*Set, 0, -1, false);
	float                                Time  = 0.0f;
	FRootMotionDelta                     Total;
	for (int32 Step = 0; Step < 25; ++Step)
	{
		bool        bWrapped = false;
		const float Previous = Time;
		Time                 = AnimationMath::AdvanceTime(Time, 0.1f, 1.0f, true, bWrapped);
		const FRootMotionDelta Delta = RootMotionMath::ComputeDelta(Track, Previous, Time, 0.1f, bWrapped, 1.0f);
		E_EXPECT_NEAR(Delta.Translation.X, 10.0f, 1.0e-2f);
		Total = RootMotionMath::Compose(Total, Delta);
	}
	E_EXPECT_NEAR(Total.Translation.X, 250.0f, 0.05f);
	// 역재생도 경계에서 이어진다 (0.05 → -0.1 → 0.95: -5 + -10 ... = -10)
	const FRootMotionDelta Backward = RootMotionMath::ComputeDelta(Track, 0.05f, 0.95f, -0.1f, true, 1.0f);
	E_EXPECT_NEAR(Backward.Translation.X, -10.0f, 1.0e-2f);
	// 제자리 포즈는 끝과 시작이 같은 XY
	FNodePose EndPose;
	EndPose.Translation = FVector3(99.9f, 0, 0.2f);
	RootMotionMath::RemoveFromPose(Track, 0.999f, EndPose);
	E_EXPECT_NEAR(EndPose.Translation.X, 0.0f, Tol);
}

E_TEST(RootMotion_YawExtractionAndCompose)
{
	const std::shared_ptr<FAnimationSet> Set   = MakeRootMotionSet();
	const FRootMotionTrack               Track = RootMotionMath::MakeTrack(*Set, 1, -1, true);
	const FRootMotionDelta               Whole = RootMotionMath::ComputeSegment(Track, 0.0f, 1.0f);
	E_EXPECT_EQUALS(Whole.Translation, FVector3(100, 0, 0), Tol);
	E_EXPECT_NEAR(Whole.Yaw, FMath::HalfPi, Tol);
	// 두 번에 나눠도 합성 결과가 같다 (두 번째 이동은 첫 회전 기준)
	const FRootMotionDelta Halves = RootMotionMath::Compose(RootMotionMath::ComputeSegment(Track, 0.0f, 0.5f), RootMotionMath::ComputeSegment(Track, 0.5f, 1.0f));
	E_EXPECT_EQUALS(Halves.Translation, Whole.Translation, 1.0e-2f);
	E_EXPECT_NEAR(Halves.Yaw, Whole.Yaw, Tol);
	// 제자리화: 회전도 시작 방향으로
	FNodePose Pose;
	Pose.Translation = FVector3(100, 0, 0);
	Pose.Rotation    = FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi);
	RootMotionMath::RemoveFromPose(Track, 1.0f, Pose);
	E_EXPECT_EQUALS(Pose.Rotation, FQuat::Identity, Tol);
	E_EXPECT_NEAR(RootMotionMath::ExtractYaw(FQuat::FromAxisAngle(FVector3::UpVector, -2.0f)), -2.0f, Tol);
	E_EXPECT_NEAR(RootMotionMath::NormalizeAngle(FMath::Pi * 1.5f), -FMath::HalfPi, Tol);

	// 트랜스폼 적용: 위치는 현재 회전 기준, 그다음 회전
	FVector3 Position;
	FQuat    Rotation = FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi);
	RootMotionMath::ApplyToTransform(Whole, Position, Rotation, FVector3(2, 2, 2));
	E_EXPECT_EQUALS(Position, FVector3(0, 200, 0), Tol);
	E_EXPECT_EQUALS(Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::Pi), Tol);
}

E_TEST(RootMotion_BlendWeights)
{
	FRootMotionDelta A;
	A.Translation = FVector3(10, 0, 0);
	A.Yaw         = 0.2f;
	FRootMotionDelta B;
	B.Translation = FVector3(0, 20, 0);
	B.Yaw         = -0.2f;
	const FRootMotionDelta Mid = RootMotionMath::Lerp(A, B, 0.25f);
	E_EXPECT_EQUALS(Mid.Translation, FVector3(7.5f, 5.0f, 0), Tol);
	E_EXPECT_NEAR(Mid.Yaw, 0.1f, Tol);
	FRootMotionDelta Sum;
	RootMotionMath::AddWeighted(Sum, A, 0.5f);
	RootMotionMath::AddWeighted(Sum, B, 0.5f);
	E_EXPECT_EQUALS(Sum.Translation, FVector3(5, 10, 0), Tol);
	E_EXPECT_NEAR(Sum.Yaw, 0.0f, Tol);
}

E_TEST(RootMotion_SystemModesAndMontages)
{
	FScene        Scene;
	const FEntity Root  = Scene.CreateEntity("Model");
	const FEntity Node0 = Scene.CreateEntity("root");
	const FEntity Node1 = Scene.CreateEntity("child");
	Scene.SetParent(Node0, Root);
	Scene.SetParent(Node1, Node0);
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Clip                 = "Walk";
	Animation.RootMotionMode       = ERootMotionMode::MontagesOnly;
	Animation.Runtime.Set          = MakeRootMotionSet();
	Animation.Runtime.NodeEntities = { Node0, Node1 };
	Scene.UpdateTransforms();

	// 몽타주만: 클립 재생은 포즈에 이동이 남고 엔티티는 그대로
	FAnimationSystem::Update(Scene, 0.0f);
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_EQUALS(Scene.GetTransform(Root).Position, FVector3(), Tol);
	E_EXPECT_NEAR(Scene.GetTransform(Node0).Position.X, 25.0f, 1.0e-2f);

	// 몽타주(Dash 300cm/s)는 추출: 엔티티 +75, 포즈는 제자리
	FMontagePlayParams Params;
	Params.BlendIn  = 0.0f;
	Params.BlendOut = 0.0f;
	E_EXPECT_TRUE(FAnimationSystem::PlayMontage(Scene, Root, "Dash", Params));
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_NEAR(Scene.GetTransform(Root).Position.X, 75.0f, 0.05f);
	E_EXPECT_NEAR(Scene.GetTransform(Node0).Position.X, 0.0f, 0.05f);
	E_EXPECT_NEAR(FAnimationSystem::GetLastRootMotion(Scene, Root).Translation.X, 75.0f, 0.05f);

	// 캐릭터 이동 컴포넌트가 조상에 있으면 엔티티 대신 그쪽에 쌓인다 (다음 무브가 속도로 쓴다)
	const FEntity Character = Scene.CreateEntity("Character");
	Scene.SetParent(Root, Character);
	FCharacterMovementComponent& Movement = Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Character);
	Scene.UpdateTransforms();
	const FVector3 Before = Scene.GetTransform(Root).Position;
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_EQUALS(Scene.GetTransform(Root).Position, Before, Tol);
	E_EXPECT_NEAR(Movement.PendingRootMotion.X, 75.0f, 0.05f);
	E_EXPECT_NEAR(Movement.PendingRootMotionSeconds, 0.25f, Tol);

	FCharacterMove Move;
	Move.DeltaSeconds = 0.1f;
	Move.bJump        = true;
	CharacterMovementMath::ConsumeRootMotion(Movement, Move);
	E_EXPECT_TRUE(Move.bRootMotion);
	E_EXPECT_NEAR(Move.RootMotionVelocity.X, 300.0f, 0.5f);
	E_EXPECT_NEAR(Movement.PendingRootMotionSeconds, 0.0f, Tol);
	bool           bJumped  = true;
	const FVector3 Velocity = CharacterMovementMath::ComputeVelocity(Movement, FVector3(0, 0, -50), true, Move, -980.0f, bJumped);
	E_EXPECT_FALSE(bJumped); // 루트 모션 중에는 점프하지 않는다
	E_EXPECT_NEAR(Velocity.X, 300.0f, 0.5f);
	E_EXPECT_NEAR(Velocity.Z, -98.0f, 0.01f);
	const FVector2 Clamped = CharacterMovementMath::ClampRootMotionVelocity(FVector2(10000.0f, 0.0f));
	E_EXPECT_NEAR(Clamped.X, FCharacterMove::MaxRootMotionSpeed, Tol);
}
