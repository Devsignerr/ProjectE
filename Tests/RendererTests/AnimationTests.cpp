#include "Core/Paths.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/AssetCache.h"
#include "Renderer/SkinnedMeshPalette.h"
#include "Scene/Animation.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <filesystem>

namespace
{
	constexpr float Tol = 1.0e-4f;

	FAnimationChannel MakeChannel(int32 Node, EAnimationPath Path, EAnimationInterpolation Interpolation, std::vector<float> Times,
	                              std::vector<FVector4> Values)
	{
		FAnimationChannel Channel;
		Channel.Node          = Node;
		Channel.Path          = Path;
		Channel.Interpolation = Interpolation;
		Channel.Times         = std::move(Times);
		Channel.Values        = std::move(Values);
		return Channel;
	}

	FVector4 ToVector(const FQuat& Q) { return FVector4(Q.X, Q.Y, Q.Z, Q.W); }
	FQuat    ToQuat(const FVector4& V) { return FQuat(V.X, V.Y, V.Z, V.W); }

	// 노드 2개(루트 0 → 자식 1). 클립 "Move": 노드 0 이동 X 0→100 (1초), 클립 "Turn": 노드 1 Z축 0→90° (1초)
	std::shared_ptr<const FAnimationSet> MakeTestSet()
	{
		FAnimationClip Move;
		Move.Name     = "Move";
		Move.Duration = 1.0f;
		Move.Channels.push_back(MakeChannel(0, EAnimationPath::Translation, EAnimationInterpolation::Linear, { 0.0f, 1.0f },
		                                    { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) }));

		FAnimationClip Turn;
		Turn.Name     = "Turn";
		Turn.Duration = 1.0f;
		Turn.Channels.push_back(MakeChannel(1, EAnimationPath::Rotation, EAnimationInterpolation::Linear, { 0.0f, 1.0f },
		                                    { ToVector(FQuat::Identity),
		                                      ToVector(FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(90.0f))) }));

		std::vector<FNodePose> Rest(2);
		Rest[1].Translation = FVector3(0, 0, 50);
		return MakeAnimationSet({ Move, Turn }, { -1, 0 }, Rest);
	}
} // namespace

E_TEST(Animation_SampleLinearAndClamp)
{
	const FAnimationChannel Channel = MakeChannel(0, EAnimationPath::Translation, EAnimationInterpolation::Linear, { 1.0f, 3.0f },
	                                              { FVector4(0, 0, 0, 0), FVector4(10, 20, 30, 0) });
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 2.0f), FVector4(5, 10, 15, 0), Tol);
	// 범위 밖은 끝 키로 고정
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 0.0f), FVector4(0, 0, 0, 0), Tol);
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 9.0f), FVector4(10, 20, 30, 0), Tol);
	// 키 정확히 위
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 3.0f), FVector4(10, 20, 30, 0), Tol);
}

E_TEST(Animation_SampleStep)
{
	const FAnimationChannel Channel = MakeChannel(0, EAnimationPath::Scale, EAnimationInterpolation::Step, { 0.0f, 1.0f, 2.0f },
	                                              { FVector4(1, 1, 1, 0), FVector4(2, 2, 2, 0), FVector4(3, 3, 3, 0) });
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 0.99f), FVector4(1, 1, 1, 0), Tol);
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 1.0f), FVector4(2, 2, 2, 0), Tol);
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 1.5f), FVector4(2, 2, 2, 0), Tol);
	E_EXPECT_EQUALS(AnimationMath::SampleChannel(Channel, 5.0f), FVector4(3, 3, 3, 0), Tol);
}

E_TEST(Animation_SampleRotationSlerp)
{
	const FQuat A = FQuat::Identity;
	const FQuat B = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(90.0f));
	// 반대 부호로 저장된 키도 최단 경로로 보간되어야 한다 (-B == B)
	const FQuat NegB(-B.X, -B.Y, -B.Z, -B.W);
	const FAnimationChannel Channel = MakeChannel(0, EAnimationPath::Rotation, EAnimationInterpolation::Linear, { 0.0f, 1.0f },
	                                              { ToVector(A), ToVector(NegB) });

	const FQuat Half     = ToQuat(AnimationMath::SampleChannel(Channel, 0.5f));
	const FQuat Expected = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(45.0f));
	E_EXPECT_EQUALS(Half, Expected, 1.0e-3f);
	E_EXPECT_NEAR(Half.Length(), 1.0f, 1.0e-4f);
	// 경계
	E_EXPECT_EQUALS(ToQuat(AnimationMath::SampleChannel(Channel, 0.0f)), A, Tol);
	E_EXPECT_EQUALS(ToQuat(AnimationMath::SampleChannel(Channel, 1.0f)), B, Tol);
	E_EXPECT_EQUALS(ToQuat(AnimationMath::SampleChannel(Channel, 2.0f)), B, Tol);
}

E_TEST(Animation_AdvanceTimeLoopAndClamp)
{
	bool bWrapped = false;
	E_EXPECT_NEAR(AnimationMath::AdvanceTime(0.5f, 0.25f, 1.0f, true, bWrapped), 0.75f, Tol);
	E_EXPECT_FALSE(bWrapped);
	E_EXPECT_NEAR(AnimationMath::AdvanceTime(0.9f, 0.25f, 1.0f, true, bWrapped), 0.15f, Tol);
	E_EXPECT_TRUE(bWrapped);
	E_EXPECT_NEAR(AnimationMath::AdvanceTime(0.1f, -0.25f, 1.0f, true, bWrapped), 0.85f, Tol);
	E_EXPECT_TRUE(bWrapped);
	E_EXPECT_NEAR(AnimationMath::AdvanceTime(0.9f, 0.25f, 1.0f, false, bWrapped), 1.0f, Tol);
	E_EXPECT_FALSE(bWrapped);
	E_EXPECT_NEAR(AnimationMath::AdvanceTime(0.5f, 1.0f, 0.0f, true, bWrapped), 0.0f, Tol);
}

E_TEST(Animation_CrossfadeWeight)
{
	E_EXPECT_NEAR(AnimationMath::ComputeCrossfadeWeight(0.0f, 1.0f), 0.0f, Tol);
	E_EXPECT_NEAR(AnimationMath::ComputeCrossfadeWeight(0.5f, 1.0f), 0.5f, Tol);
	E_EXPECT_NEAR(AnimationMath::ComputeCrossfadeWeight(2.0f, 1.0f), 1.0f, Tol);
	E_EXPECT_NEAR(AnimationMath::ComputeCrossfadeWeight(0.0f, 0.0f), 1.0f, Tol);
	// 단조 증가
	E_EXPECT_TRUE(AnimationMath::ComputeCrossfadeWeight(0.2f, 1.0f) < AnimationMath::ComputeCrossfadeWeight(0.3f, 1.0f));

	FNodePose A;
	FNodePose B;
	B.Translation = FVector3(10, 0, 0);
	B.Rotation    = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(90.0f));
	B.Scale       = FVector3(3, 3, 3);
	const FNodePose Mid = AnimationMath::BlendPose(A, B, 0.5f);
	E_EXPECT_EQUALS(Mid.Translation, FVector3(5, 0, 0), Tol);
	E_EXPECT_EQUALS(Mid.Scale, FVector3(2, 2, 2), Tol);
	E_EXPECT_EQUALS(Mid.Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(45.0f)), 1.0e-3f);
}

E_TEST(Animation_ModelMatricesAnyParentOrder)
{
	// 부모(인덱스 1)가 자식(인덱스 0)보다 뒤에 있어도 올바르게 합성
	std::vector<FNodePose> Pose(2);
	Pose[1].Translation = FVector3(100, 0, 0);
	Pose[1].Rotation    = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(90.0f));
	Pose[0].Translation = FVector3(10, 0, 0);
	std::vector<FMatrix4x4> Matrices;
	AnimationMath::ComputeModelMatrices(Pose, { 1, -1 }, Matrices);
	// 부모가 +X를 +Y로 돌리므로 자식 (10,0,0)은 (100,10,0)
	E_EXPECT_EQUALS(Matrices[0].GetOrigin(), FVector3(100, 10, 0), 1.0e-3f);
	E_EXPECT_EQUALS(Matrices[1].GetOrigin(), FVector3(100, 0, 0), Tol);
}

E_TEST(Animation_RootMotionDeltaAcrossLoop)
{
	const FAnimationChannel Channel = MakeChannel(0, EAnimationPath::Translation, EAnimationInterpolation::Linear, { 0.0f, 1.0f },
	                                              { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) });
	E_EXPECT_EQUALS(AnimationMath::ComputeRootMotionDelta(Channel, 0.2f, 0.5f, false, 1.0f), FVector3(30, 0, 0), 1.0e-3f);
	// 0.9 → (루프) → 0.1: 끝까지 10 + 처음부터 10
	E_EXPECT_EQUALS(AnimationMath::ComputeRootMotionDelta(Channel, 0.9f, 0.1f, true, 1.0f), FVector3(20, 0, 0), 1.0e-3f);

	const std::shared_ptr<const FAnimationSet> Set = MakeTestSet();
	E_EXPECT_EQ(Set->RootMotionNode, 0);
	E_EXPECT_EQ(Set->FindClip("Turn"), 1);
	E_EXPECT_EQ(Set->FindClip("없음"), -1);
	E_EXPECT_EQ(Set->AnimatedNodes[0], static_cast<uint8>(1));
	E_EXPECT_EQ(Set->AnimatedNodes[1], static_cast<uint8>(1));
}

// 시스템: 재생 → 노드 엔티티 트랜스폼 갱신, Play로 크로스페이드, Stop/SetSpeed, 루트 모션
E_TEST(Animation_SystemPlaybackAndCrossfade)
{
	FScene        Scene;
	const FEntity Root  = Scene.CreateEntity("Model");
	const FEntity Node0 = Scene.CreateEntity("Node0");
	const FEntity Node1 = Scene.CreateEntity("Node1");
	Scene.SetParent(Node0, Root);
	Scene.SetParent(Node1, Node0);

	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Clip                 = "Move";
	Animation.BlendTime            = 0.5f;
	Animation.Runtime.Set          = MakeTestSet();
	Animation.Runtime.NodeEntities = { Node0, Node1 };

	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_EQUALS(Scene.GetTransform(Node0).Position, FVector3(25, 0, 0), 1.0e-3f);
	E_EXPECT_EQUALS(Scene.GetTransform(Node1).Position, FVector3(0, 0, 50), Tol); // 기본 포즈 유지
	E_EXPECT_TRUE(FAnimationSystem::GetCurrentClip(Scene, Root) == "Move");

	const std::vector<std::string> Names = FAnimationSystem::GetClipNames(Scene, Root);
	E_EXPECT_EQ(Names.size(), static_cast<size_t>(2));

	// 크로스페이드 0.5초: 절반(가중치 0.5)에서 Node0 이동은 Move(0.5초 → 50)와 기본(0)의 중간
	E_EXPECT_TRUE(FAnimationSystem::Play(Scene, Root, "Turn", 0.5f));
	E_EXPECT_FALSE(FAnimationSystem::Play(Scene, Root, "없는클립"));
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_TRUE(FAnimationSystem::GetCurrentClip(Scene, Root) == "Turn");
	E_EXPECT_EQUALS(Scene.GetTransform(Node0).Position, FVector3(25, 0, 0), 1.0e-2f);
	// Turn 0.25초(22.5°)와 기본(0°)의 중간 → 11.25°
	E_EXPECT_EQUALS(Scene.GetTransform(Node1).Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(11.25f)), 1.0e-3f);

	// 블렌드 종료 후 Turn만
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_EQUALS(Scene.GetTransform(Node0).Position, FVector3::ZeroVector, 1.0e-3f);
	E_EXPECT_EQUALS(Scene.GetTransform(Node1).Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(45.0f)), 1.0e-3f);

	// 정지: 시간이 흐르지 않는다
	FAnimationSystem::Stop(Scene, Root);
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_EQUALS(Scene.GetTransform(Node1).Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(45.0f)), 1.0e-3f);

	// 속도 2배
	FAnimationSystem::Resume(Scene, Root);
	FAnimationSystem::SetSpeed(Scene, Root, 2.0f);
	FAnimationSystem::Update(Scene, 0.125f); // 0.5 → 0.75
	E_EXPECT_EQUALS(Scene.GetTransform(Node1).Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(67.5f)), 1.0e-3f);

	// 루트 모션: Move의 노드 0 수평 이동이 루트 엔티티로 옮겨지고 본은 시작 위치에 고정
	FAnimationSystem::SetSpeed(Scene, Root, 1.0f);
	Animation.bRootMotion = true;
	E_EXPECT_TRUE(FAnimationSystem::Play(Scene, Root, "Move", 0.0f));
	FAnimationSystem::Update(Scene, 0.0f);  // 즉시 전환, 시간 0
	FAnimationSystem::Update(Scene, 0.25f); // +25
	FAnimationSystem::Update(Scene, 0.25f); // +25
	E_EXPECT_EQUALS(Scene.GetTransform(Root).Position, FVector3(50, 0, 0), 1.0e-2f);
	E_EXPECT_EQUALS(Scene.GetTransform(Node0).Position, FVector3::ZeroVector, 1.0e-3f);
}

E_TEST(Animation_SkinPalette)
{
	const std::vector<FMatrix4x4> InverseBind = { FMatrix4x4::MakeTranslation(FVector3(0, 0, -100)) };
	const std::vector<FMatrix4x4> JointWorld  = { FMatrix4x4::MakeTranslation(FVector3(10, 0, 100)) };
	std::vector<FMatrix4x4>       Palette;
	FSkinnedMeshPalette::ComputePalette(InverseBind, JointWorld, Palette);
	E_EXPECT_EQ(Palette.size(), static_cast<size_t>(1));
	// 바인드 공간 정점 (0,0,100) → 조인트 로컬 (0,0,0) → 월드 (10,0,100)
	E_EXPECT_EQUALS(Palette[0].TransformPosition(FVector3(0, 0, 100)), FVector3(10, 0, 100), Tol);
}

E_TEST(Animation_CookedModelRoundTrip)
{
	FModelData Source;
	Source.Name = "Skinned";
	FModelMesh& Mesh = Source.Meshes.emplace_back();
	Mesh.Data.Vertices.resize(3);
	Mesh.Data.Indices = { 0, 1, 2 };
	Mesh.SkinVertices.resize(3);
	Mesh.SkinVertices[2].Joints[1] = 1;
	Mesh.SkinVertices[2].Weights   = FVector4(0.25f, 0.75f, 0, 0);
	FModelNode& Node = Source.Nodes.emplace_back();
	Node.Name  = "Node";
	Node.Skin  = 0;
	Node.Meshes = { 0 };
	Source.Nodes.emplace_back().Name = "Joint";
	Source.RootNodes = { 0, 1 };
	FModelSkin& Skin = Source.Skins.emplace_back();
	Skin.Joints              = { 1, 1 };
	Skin.InverseBindMatrices = { FMatrix4x4::Identity, FMatrix4x4::MakeTranslation(FVector3(1, 2, 3)) };
	FAnimationClip& Clip = Source.Animations.emplace_back();
	Clip.Name     = "Clip";
	Clip.Duration = 2.0f;
	Clip.Channels.push_back(MakeChannel(1, EAnimationPath::Rotation, EAnimationInterpolation::Step, { 0.0f, 2.0f },
	                                    { FVector4(0, 0, 0, 1), FVector4(0, 0, 1, 0) }));

	FBinaryWriter Writer;
	FAssetCache::WriteModel(Writer, Source);
	FBinaryReader Reader(Writer.GetBuffer().data(), Writer.GetBuffer().size());
	FModelData    Loaded;
	E_EXPECT_TRUE(FAssetCache::ReadModel(Reader, Loaded));
	E_EXPECT_EQ(Loaded.Meshes.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Loaded.Skins.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Loaded.Animations.size(), static_cast<size_t>(1));
	if (Loaded.Meshes.size() != 1 || Loaded.Skins.size() != 1 || Loaded.Animations.size() != 1)
	{
		return;
	}
	E_EXPECT_EQ(Loaded.Nodes[0].Skin, 0);
	E_EXPECT_EQ(Loaded.Meshes[0].SkinVertices.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Loaded.Meshes[0].SkinVertices[2].Joints[1], static_cast<uint16>(1));
	E_EXPECT_EQUALS(Loaded.Meshes[0].SkinVertices[2].Weights, FVector4(0.25f, 0.75f, 0, 0), 0.0f);
	E_EXPECT_EQUALS(Loaded.Skins[0].InverseBindMatrices[1], FMatrix4x4::MakeTranslation(FVector3(1, 2, 3)), 0.0f);
	E_EXPECT_TRUE(Loaded.Animations[0].Name == "Clip");
	E_EXPECT_NEAR(Loaded.Animations[0].Duration, 2.0f, 0.0f);
	E_EXPECT_TRUE(Loaded.Animations[0].Channels[0].Interpolation == EAnimationInterpolation::Step);
	E_EXPECT_EQUALS(Loaded.Animations[0].Channels[0].Values[1], FVector4(0, 0, 1, 0), 0.0f);

	// 조인트 인덱스가 노드 범위를 벗어나면 거부
	Source.Skins[0].Joints = { 1, 7 };
	FBinaryWriter BadWriter;
	FAssetCache::WriteModel(BadWriter, Source);
	FBinaryReader BadReader(BadWriter.GetBuffer().data(), BadWriter.GetBuffer().size());
	FModelData    Rejected;
	E_EXPECT_FALSE(FAssetCache::ReadModel(BadReader, Rejected));
}

// 실제 샘플(Fox): 스킨/클립 구성, 바인드 포즈에서 스킨 결과가 원래 정점과 일치 (역바인드 변환·단위 검증)
E_TEST(Animation_FoxBindPoseConsistency)
{
	if (!FPaths::HasProject())
	{
		return;
	}
	const std::filesystem::path Source = FPaths::GetProjectContentDirectory() / L"Fox.glb";
	if (!std::filesystem::exists(Source))
	{
		return;
	}
	FModelData Model;
	E_EXPECT_TRUE(FGltfLoader::Load(Source, Model));
	E_EXPECT_EQ(Model.Skins.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Model.Animations.size(), static_cast<size_t>(3));
	if (Model.Skins.size() != 1 || Model.Meshes.empty() || Model.Meshes[0].SkinVertices.empty())
	{
		E_EXPECT_TRUE(false);
		return;
	}
	E_EXPECT_EQ(Model.Skins[0].Joints.size(), static_cast<size_t>(24));

	std::vector<FNodePose> Rest(Model.Nodes.size());
	std::vector<int32>     Parents(Model.Nodes.size());
	for (size_t Index = 0; Index < Model.Nodes.size(); ++Index)
	{
		Rest[Index]    = { Model.Nodes[Index].Translation, Model.Nodes[Index].Rotation, Model.Nodes[Index].Scale };
		Parents[Index] = Model.Nodes[Index].Parent;
	}
	std::vector<FMatrix4x4> ModelMatrices;
	AnimationMath::ComputeModelMatrices(Rest, Parents, ModelMatrices);

	std::vector<FMatrix4x4> JointMatrices;
	for (int32 Joint : Model.Skins[0].Joints)
	{
		JointMatrices.push_back(ModelMatrices[Joint]);
	}
	std::vector<FMatrix4x4> Palette;
	FSkinnedMeshPalette::ComputePalette(Model.Skins[0].InverseBindMatrices, JointMatrices, Palette);

	// Fox 원본은 약 155 단위(glTF 미터로 해석 → ×100 후 약 1.5만 cm). 바인드 포즈 스킨 결과는 원래 정점과 같아야 한다
	const FModelMesh& Mesh     = Model.Meshes[0];
	float             MaxError = 0.0f;
	for (size_t Index = 0; Index < Mesh.Data.Vertices.size(); Index += 7)
	{
		const FVector3&    Position = Mesh.Data.Vertices[Index].Position;
		const FSkinVertex& Skin     = Mesh.SkinVertices[Index];
		const float        Weights[4] = { Skin.Weights.X, Skin.Weights.Y, Skin.Weights.Z, Skin.Weights.W };
		FVector3           Skinned = FVector3::ZeroVector;
		for (int32 Slot = 0; Slot < 4; ++Slot)
		{
			Skinned += Palette[Skin.Joints[Slot]].TransformPosition(Position) * Weights[Slot];
		}
		MaxError = FMath::Max(MaxError, (Skinned - Position).Length());
	}
	E_EXPECT_TRUE(MaxError < 1.0f); // 1cm 미만 (모델 전체 약 1.5만 cm)
}
