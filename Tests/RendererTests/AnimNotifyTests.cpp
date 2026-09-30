#include "Core/Testing/TestFramework.h"
#include "Scene/AnimNotify.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"
#include "Scene/SceneSerializer.h"

#include <filesystem>
#include <memory>

namespace
{
	constexpr float Tol = 1.0e-4f;

	FAnimNotify MakeNotify(const char* Name, float Time)
	{
		FAnimNotify Notify;
		Notify.Name = Name;
		Notify.Time = Time;
		return Notify;
	}

	FAnimNotify MakeState(const char* Name, float Start, float Duration)
	{
		FAnimNotify Notify = MakeNotify(Name, Start);
		Notify.Kind        = EAnimNotifyKind::State;
		Notify.Duration    = Duration;
		return Notify;
	}

	// 한 번 진행한 결과를 "인덱스:종류" 문자열로 (N=노티파이 B=시작 T=진행 E=끝)
	std::string Run(const std::vector<FAnimNotify>& Notifies, float From, float To, float Delta, bool bLoop, bool bWrapped, std::vector<uint8>& Active,
	                bool bResync = false)
	{
		std::vector<FAnimNotifyHit> Hits;
		AnimNotifyMath::Collect(Notifies, From, To, Delta, 1.0f, bLoop, bWrapped, bResync, Active, Hits);
		std::string Result;
		for (const FAnimNotifyHit& Hit : Hits)
		{
			static constexpr char Letters[] = { 'N', 'B', 'T', 'E' };
			Result += std::to_string(Hit.Index) + Letters[static_cast<int32>(Hit.Type)] + ' ';
		}
		return Result;
	}

	FAnimationChannel MakeChannel(int32 Node)
	{
		FAnimationChannel Channel;
		Channel.Node   = Node;
		Channel.Path   = EAnimationPath::Translation;
		Channel.Times  = { 0.0f, 1.0f };
		Channel.Values = { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) };
		return Channel;
	}

	// 클립 "Move", "Idle" (각 1초, 노드 0 이동)
	std::shared_ptr<const FAnimationSet> MakeSet()
	{
		FAnimationClip Move;
		Move.Name     = "Move";
		Move.Duration = 1.0f;
		Move.Channels.push_back(MakeChannel(0));
		FAnimationClip Idle = Move;
		Idle.Name           = "Idle";
		return MakeAnimationSet({ Move, Idle }, { -1 }, std::vector<FNodePose>(1));
	}

	std::string Describe(const std::vector<FAnimNotifyEvent>& Events)
	{
		std::string Result;
		for (const FAnimNotifyEvent& Event : Events)
		{
			static constexpr const char* Types[] = { "", ".Begin", ".Tick", ".End" };
			Result += Event.Name + Types[static_cast<int32>(Event.Type)] + ' ';
		}
		return Result;
	}
} // namespace

E_TEST(AnimNotify_NamesAreIdentifiers)
{
	E_EXPECT_TRUE(AnimNotifyMath::IsValidName("Footstep_L"));
	E_EXPECT_FALSE(AnimNotifyMath::IsValidName("1Step"));
	E_EXPECT_FALSE(AnimNotifyMath::IsValidName("발소리"));
	E_EXPECT_FALSE(AnimNotifyMath::IsValidName(""));
	E_EXPECT_TRUE(AnimNotifyMath::MakeValidName("foot step") == "foot_step");
	E_EXPECT_TRUE(AnimNotifyMath::MakeValidName("1Step") == "_1Step");
	E_EXPECT_TRUE(AnimNotifyMath::MakeValidName("") == "Notify");
}

E_TEST(AnimNotify_PointNotifiesAcrossLoopAndEnd)
{
	const std::vector<FAnimNotify> Notifies = { MakeNotify("Start", 0.0f), MakeNotify("Mid", 0.5f), MakeNotify("End", 1.0f) };
	std::vector<uint8>             Active;

	// 처음 진행: 0초 노티파이 포함, 경계 시각은 한 번만
	E_EXPECT_TRUE(Run(Notifies, 0.0f, 0.5f, 0.5f, true, false, Active) == "0N ");
	E_EXPECT_TRUE(Run(Notifies, 0.5f, 0.9f, 0.4f, true, false, Active) == "1N ");
	// 루프 경계: 끝(1.0) 포함 → 처음부터(0) 포함
	E_EXPECT_TRUE(Run(Notifies, 0.9f, 0.1f, 0.2f, true, true, Active) == "2N 0N ");
	// 멈춤(진행량 0)은 아무것도 없음
	E_EXPECT_TRUE(Run(Notifies, 0.1f, 0.1f, 0.0f, true, false, Active).empty());
	// 반복 없음: 끝에 닿을 때 끝 노티파이 한 번, 멈춰 있는 동안에는 다시 안 남
	E_EXPECT_TRUE(Run(Notifies, 0.8f, 1.0f, 0.3f, false, false, Active) == "2N ");
	E_EXPECT_TRUE(Run(Notifies, 1.0f, 1.0f, 0.3f, false, false, Active).empty());
	// 역재생: 거꾸로 지나간 순서대로
	E_EXPECT_TRUE(Run(Notifies, 0.9f, 0.4f, -0.5f, true, false, Active) == "1N ");
}

E_TEST(AnimNotify_StatesBeginTickEnd)
{
	const std::vector<FAnimNotify> Notifies = { MakeState("Swing", 0.2f, 0.3f), MakeState("Short", 0.6f, 0.05f), MakeState("Tail", 0.8f, 0.2f) };
	std::vector<uint8>             Active;

	E_EXPECT_TRUE(Run(Notifies, 0.0f, 0.3f, 0.3f, true, false, Active) == "0B 0T ");
	E_EXPECT_TRUE(Run(Notifies, 0.3f, 0.4f, 0.1f, true, false, Active) == "0T ");
	// 한 프레임에 Swing 끝 + Short가 들어갔다 나옴 (누락 없음)
	E_EXPECT_TRUE(Run(Notifies, 0.4f, 0.7f, 0.3f, true, false, Active) == "0E 1B 1E ");
	E_EXPECT_TRUE(Run(Notifies, 0.7f, 0.9f, 0.2f, true, false, Active) == "2B 2T ");
	// 루프 경계: Tail은 끝(1.0)에서 End, 새 주기에서 Swing은 아직 (0.1 < 0.2)
	E_EXPECT_TRUE(Run(Notifies, 0.9f, 0.1f, 0.2f, true, true, Active) == "2E ");
	// 클립 전환/스크럽: 진행 중 스테이트 모두 End
	E_EXPECT_TRUE(Run(Notifies, 0.1f, 0.25f, 0.15f, true, false, Active) == "0B 0T ");
	std::vector<FAnimNotifyHit> Hits;
	AnimNotifyMath::EndAll(Active, Hits);
	E_EXPECT_TRUE(Hits.size() == 1 && Hits[0].Index == 0 && Hits[0].Type == EAnimNotifyEventType::StateEnd);
	// 스크럽 후 다시 진행: 시작 시각이 스테이트 안이면 Begin (bResync)
	E_EXPECT_TRUE(Run(Notifies, 0.3f, 0.35f, 0.05f, true, false, Active, true) == "0B 0T ");
}

E_TEST(AnimNotify_SystemEmitsAndEndsStatesOnClipChange)
{
	FScene        Scene;
	const FEntity Root = Scene.CreateEntity("Model");
	const FEntity Node = Scene.CreateEntity("Node");
	Scene.SetParent(Node, Root);

	auto Metadata = std::make_shared<FModelMetadata>();
	Metadata->GetOrAddNotifies("Move") = { MakeNotify("Footstep", 0.25f), MakeState("Swing", 0.4f, 0.4f) };

	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Clip                 = "Move";
	Animation.BlendTime            = 0.2f;
	Animation.Runtime.Set          = MakeSet();
	Animation.Runtime.NodeEntities = { Node };
	Animation.Runtime.Metadata     = Metadata;

	FAnimationSystem::Update(Scene, 0.3f);
	E_EXPECT_TRUE(Describe(Animation.Runtime.PendingNotifies) == "Footstep ");
	E_EXPECT_TRUE(Animation.Runtime.PendingNotifies[0].Entity == Root && Animation.Runtime.PendingNotifies[0].Clip == "Move");
	FAnimationSystem::Update(Scene, 0.2f); // 0.5
	E_EXPECT_TRUE(Describe(Animation.Runtime.PendingNotifies) == "Swing.Begin Swing.Tick ");
	E_EXPECT_NEAR(Animation.Runtime.PendingNotifies[1].DeltaSeconds, 0.2f, Tol);

	// 다른 클립으로 크로스페이드: 이전 클립 스테이트는 End, 사라지는 클립 노티파이는 없음
	E_EXPECT_TRUE(FAnimationSystem::Play(Scene, Root, "Idle"));
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(Describe(Animation.Runtime.PendingNotifies) == "Swing.End ");
	FAnimationSystem::Update(Scene, 0.5f);
	E_EXPECT_TRUE(Animation.Runtime.PendingNotifies.empty());

	// 스크럽(SetTime)은 점 노티파이를 건너뛴다
	E_EXPECT_TRUE(FAnimationSystem::Play(Scene, Root, "Move", 0.0f));
	FAnimationSystem::Update(Scene, 0.0f);
	FAnimationSystem::SetTime(Scene, Root, 0.5f);
	FAnimationSystem::Update(Scene, 0.05f); // 0.5 → 0.55: Swing 안에서 재개
	E_EXPECT_TRUE(Describe(Animation.Runtime.PendingNotifies) == "Swing.Begin Swing.Tick ");

	// 편집기가 공유 메타데이터를 바꾸면 바로 반영
	Metadata->GetOrAddNotifies("Move").push_back(MakeNotify("Late", 0.6f));
	FAnimationSystem::Update(Scene, 0.1f); // 0.55 → 0.65
	E_EXPECT_TRUE(Describe(Animation.Runtime.PendingNotifies) == "Late Swing.Tick ");
}

E_TEST(ModelMetadata_JsonRoundTripAndSidecar)
{
	FModelMetadata Metadata;
	Metadata.GetOrAddNotifies("Walk") = { MakeNotify("Footstep_L", 0.3f), MakeState("Trail", 0.1f, 0.5f) };
	FModelSocket Socket;
	Socket.Name     = "Hand_R";
	Socket.Bone     = "mixamorig:RightHand";
	Socket.Position = FVector3(1.0f, 2.0f, 3.0f);
	Socket.Rotation = FQuat::FromEuler(10.0f, 20.0f, 30.0f);
	Socket.Scale    = FVector3(2.0f);
	Metadata.Sockets.push_back(Socket);

	FModelMetadata Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Metadata.ToJsonString()));
	const std::vector<FAnimNotify>* Walk = Loaded.FindNotifies("Walk");
	E_EXPECT_TRUE(Walk != nullptr && Walk->size() == 2);
	if (Walk != nullptr && Walk->size() == 2)
	{
		// 시간순 정렬
		E_EXPECT_TRUE((*Walk)[0].Name == "Trail" && (*Walk)[0].Kind == EAnimNotifyKind::State);
		E_EXPECT_NEAR((*Walk)[0].Duration, 0.5f, Tol);
		E_EXPECT_TRUE((*Walk)[1].Name == "Footstep_L" && (*Walk)[1].Kind == EAnimNotifyKind::Notify);
	}
	const FModelSocket* LoadedSocket = Loaded.FindSocket("Hand_R");
	E_EXPECT_TRUE(LoadedSocket != nullptr);
	if (LoadedSocket != nullptr)
	{
		E_EXPECT_TRUE(LoadedSocket->Bone == "mixamorig:RightHand");
		E_EXPECT_EQUALS(LoadedSocket->Position, FVector3(1.0f, 2.0f, 3.0f), Tol);
		E_EXPECT_TRUE(LoadedSocket->Rotation.Equals(Socket.Rotation, 1.0e-4f));
		E_EXPECT_EQUALS(LoadedSocket->Scale, FVector3(2.0f), Tol);
	}
	E_EXPECT_FALSE(Loaded.FromJsonString("{ 깨진"));

	// 사이드카: 원본 + ".emeta", 비어 있으면 파일을 지운다
	const std::filesystem::path Source = FTestRegistry::GetTempDirectory() / L"ProjectE_메타_테스트.glb";
	E_EXPECT_TRUE(FModelMetadata::GetSidecarPath(Source).filename() == L"ProjectE_메타_테스트.glb.emeta");
	E_EXPECT_TRUE(Metadata.SaveForSource(Source));
	E_EXPECT_TRUE(std::filesystem::exists(FModelMetadata::GetSidecarPath(Source)));
	E_EXPECT_TRUE(FModelMetadata::LoadForSource(Source).FindSocket("Hand_R") != nullptr);
	E_EXPECT_TRUE(FModelMetadata{}.SaveForSource(Source));
	E_EXPECT_FALSE(std::filesystem::exists(FModelMetadata::GetSidecarPath(Source)));
	E_EXPECT_TRUE(FModelMetadata::LoadForSource(Source).IsEmpty());
}

E_TEST(Socket_AttachedEntityFollowsBoneAndSurvivesCloneAndSave)
{
	FScene        Scene;
	const FEntity Model = Scene.CreateEntity("Character");
	const FEntity Bone  = Scene.CreateEntity("Hand");
	Scene.SetParent(Bone, Model);
	Scene.GetRegistry().Emplace<FTransientComponent>(Bone);
	Scene.GetTransform(Model).Position = FVector3(100.0f, 0.0f, 0.0f);
	Scene.GetTransform(Bone).Position  = FVector3(0.0f, 50.0f, 0.0f);

	auto         Metadata = std::make_shared<FModelMetadata>();
	FModelSocket Socket;
	Socket.Name     = "Grip";
	Socket.Bone     = "Hand";
	Socket.Position = FVector3(0.0f, 0.0f, 10.0f);
	Metadata->Sockets.push_back(Socket);
	FModelSocket RootSocket;
	RootSocket.Name     = "Back";
	RootSocket.Position = FVector3(-20.0f, 0.0f, 0.0f);
	Metadata->Sockets.push_back(RootSocket);
	FModelComponent& ModelComponent      = Scene.GetRegistry().Emplace<FModelComponent>(Model);
	ModelComponent.AssetPath             = "Models/Character.glb";
	ModelComponent.Runtime.Metadata      = Metadata;
	ModelComponent.Runtime.NodeEntities  = { Bone };

	// 무기: 소켓 기준 로컬 (5, 0, 0). 계층 부모는 없음 — 소켓이 부모 역할
	const FEntity Sword = Scene.CreateEntity("Sword");
	Scene.GetTransform(Sword).Position = FVector3(5.0f, 0.0f, 0.0f);
	Scene.GetRegistry().Emplace<FSocketAttachmentComponent>(Sword) = { Model, "Grip" };
	// 부착의 부착: 칼에 달린 장식이 모델처럼 소켓을 가진다
	const FEntity    Gem         = Scene.CreateEntity("Gem");
	auto             SwordMeta   = std::make_shared<FModelMetadata>();
	FModelSocket     Tip;
	Tip.Name     = "Tip";
	Tip.Position = FVector3(30.0f, 0.0f, 0.0f);
	SwordMeta->Sockets.push_back(Tip);
	Scene.GetRegistry().Emplace<FModelComponent>(Sword).Runtime.Metadata = SwordMeta;
	Scene.GetRegistry().Emplace<FSocketAttachmentComponent>(Gem)         = { Sword, "Tip" };

	Scene.UpdateTransforms();
	E_EXPECT_EQUALS(Scene.GetTransform(Sword).GetWorldPosition(), FVector3(105.0f, 50.0f, 10.0f), Tol);
	E_EXPECT_EQUALS(Scene.GetTransform(Gem).GetWorldPosition(), FVector3(135.0f, 50.0f, 10.0f), Tol);

	// 뼈가 움직이면(애니메이션) 따라간다. 모델 루트 소켓도 동작
	Scene.GetTransform(Bone).Rotation = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(90.0f));
	Scene.UpdateTransforms();
	FMatrix4x4 GripWorld;
	E_EXPECT_TRUE(Scene.GetSocketWorldMatrix(Model, "Grip", GripWorld));
	E_EXPECT_TRUE(Scene.GetParentWorldMatrix(Sword).Equals(GripWorld, Tol));
	E_EXPECT_EQUALS(Scene.GetTransform(Sword).GetWorldPosition(), GripWorld.TransformPosition(FVector3(5.0f, 0.0f, 0.0f)), Tol);
	FMatrix4x4 BackWorld;
	E_EXPECT_TRUE(Scene.GetSocketWorldMatrix(Model, "Back", BackWorld));
	E_EXPECT_EQUALS(BackWorld.GetOrigin(), FVector3(80.0f, 0.0f, 0.0f), Tol);

	// 없는 소켓 / 자기 하위를 대상으로 하면 평소 계층을 따른다
	E_EXPECT_FALSE(Scene.GetSocketWorldMatrix(Model, "없음", GripWorld));
	const FEntity Child = Scene.CreateEntity("Child");
	Scene.SetParent(Child, Sword);
	Scene.GetRegistry().Emplace<FModelComponent>(Child);
	Scene.GetRegistry().Get<FSocketAttachmentComponent>(Sword).Target = Child;
	E_EXPECT_FALSE(Scene.IsSocketAttached(Sword));
	Scene.UpdateTransforms();
	E_EXPECT_EQUALS(Scene.GetTransform(Sword).GetWorldPosition(), FVector3(5.0f, 0.0f, 0.0f), Tol);
	Scene.GetRegistry().Get<FSocketAttachmentComponent>(Sword).Target = Model;
	Scene.DestroyEntity(Child);
	Scene.UpdateTransforms();

	// 플레이 모드 복제: 대상 참조와 모델 노드 참조가 복제본으로 이어진다
	FScene                   Clone;
	FSceneCloner::FEntityMap Map;
	FSceneCloner::Clone(Scene, Clone, &Map);
	Clone.UpdateTransforms();
	const FEntity ClonedSword = Map.at(Sword.ToId());
	const FEntity ClonedModel = Map.at(Model.ToId());
	E_EXPECT_TRUE(Clone.GetRegistry().Get<FSocketAttachmentComponent>(ClonedSword).Target == ClonedModel);
	E_EXPECT_TRUE(Clone.GetRegistry().Get<FModelComponent>(ClonedModel).Runtime.NodeEntities[0] == Map.at(Bone.ToId()));
	E_EXPECT_EQUALS(Clone.GetTransform(ClonedSword).GetWorldPosition(), Scene.GetTransform(Sword).GetWorldPosition(), Tol);

	// 저장/불러오기: 부착 컴포넌트(대상 + 소켓 이름)가 유지된다 (모델 노드는 불러올 때 다시 생성)
	FScene Restored;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Restored, FSceneSerializer::ToJsonString(Scene)));
	bool bFound = false;
	Restored.GetRegistry().View<FSocketAttachmentComponent>().Each([&](FEntity Entity, FSocketAttachmentComponent& Attachment) {
		const FNameComponent* Name       = Restored.GetRegistry().TryGet<FNameComponent>(Entity);
		const FNameComponent* TargetName = Restored.GetRegistry().TryGet<FNameComponent>(Attachment.Target);
		if (Name != nullptr && Name->Name == "Sword")
		{
			bFound = TargetName != nullptr && TargetName->Name == "Character" && Attachment.Socket == "Grip";
		}
	});
	E_EXPECT_TRUE(bFound);
}
