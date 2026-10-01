#include "Core/Testing/TestFramework.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Sequence.h"
#include "Scene/SequencePlayer.h"

#include <filesystem>
#include <fstream>

// 컷신 시퀀스 (Scene/Sequence.h, Scene/SequencePlayer.h): 키 보간, 컷/구간/이벤트 판정, 형식 왕복, 바인딩, 재생·복원

namespace
{
	constexpr float Tol = 1.0e-3f;

	FSequenceValueKey Key(float Time, float Value, ESequenceInterp Interp)
	{
		FSequenceValueKey Result;
		Result.Time    = Time;
		Result.Value.X = Value;
		Result.Interp  = Interp;
		return Result;
	}

	std::filesystem::path WriteSequence(const wchar_t* Name, const FSequenceAsset& Asset)
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectESequenceTests";
		std::filesystem::create_directories(Directory);
		const std::filesystem::path Path = Directory / Name;
		Asset.SaveToFile(Path);
		FSequenceLibrary::Get().Invalidate();
		return Path;
	}
} // namespace

E_TEST(Sequence_InterpolationModes)
{
	// 두 키: 계단은 끝에서만 바뀌고, 직선은 비례, 곡선(양 끝 기울기 0)은 가운데 대칭 + 처음엔 느리게
	E_EXPECT_NEAR(SequenceMath::Interpolate(ESequenceInterp::Constant, 0.9f, 0, 0, 1, 10, false, 0, 0, false, 0, 0), 0.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::Interpolate(ESequenceInterp::Constant, 1.0f, 0, 0, 1, 10, false, 0, 0, false, 0, 0), 10.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::Interpolate(ESequenceInterp::Linear, 0.25f, 0, 0, 1, 10, false, 0, 0, false, 0, 0), 2.5f, Tol);
	E_EXPECT_NEAR(SequenceMath::Interpolate(ESequenceInterp::Smooth, 0.5f, 0, 0, 1, 10, false, 0, 0, false, 0, 0), 5.0f, Tol);
	E_EXPECT_TRUE(SequenceMath::Interpolate(ESequenceInterp::Smooth, 0.25f, 0, 0, 1, 10, false, 0, 0, false, 0, 0) < 2.5f);

	// 세 키 직선 위 (0, 10, 20): 가운데 키 기울기 = 이웃을 잇는 기울기 10/s → 곡선도 직선과 같아진다 (끝 구간 제외)
	const std::vector<FSequenceValueKey> Keys = { Key(0, 0, ESequenceInterp::Smooth), Key(1, 10, ESequenceInterp::Smooth), Key(2, 20, ESequenceInterp::Smooth),
	                                              Key(3, 30, ESequenceInterp::Smooth) };
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Keys, 1.5f, FVector4(), false).X, 15.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Keys, 1.0f, FVector4(), false).X, 10.0f, Tol);
	// 범위 밖은 끝 값, 키 없으면 기본값
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Keys, -1.0f, FVector4(), false).X, 0.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Keys, 9.0f, FVector4(), false).X, 30.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys({}, 1.0f, FVector4(7, 0, 0, 0), false).X, 7.0f, Tol);
	// bool/int 프로퍼티는 보간 방식과 무관하게 계단
	const std::vector<FSequenceValueKey> Bools = { Key(0, 0, ESequenceInterp::Linear), Key(1, 1, ESequenceInterp::Linear) };
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Bools, 0.99f, FVector4(), true).X, 0.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Bools, 1.0f, FVector4(), true).X, 1.0f, Tol);
	// 키 구간마다 앞 키의 보간: 0→1 계단, 1→2 직선
	const std::vector<FSequenceValueKey> Mixed = { Key(0, 0, ESequenceInterp::Constant), Key(1, 10, ESequenceInterp::Linear), Key(2, 20, ESequenceInterp::Linear) };
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Mixed, 0.5f, FVector4(), false).X, 0.0f, Tol);
	E_EXPECT_NEAR(SequenceMath::EvaluateValueKeys(Mixed, 1.5f, FVector4(), false).X, 15.0f, Tol);
}

E_TEST(Sequence_TransformKeysAndEuler)
{
	FSequenceTransformKey A;
	A.Time     = 0.0f;
	A.Interp   = ESequenceInterp::Linear;
	FSequenceTransformKey B = A;
	B.Time     = 2.0f;
	B.Position = FVector3(100.0f, 0.0f, 50.0f);
	B.Rotation = FVector3(0.0f, 90.0f, 0.0f);
	B.Scale    = FVector3(3.0f);
	FVector3 Position;
	FQuat    Rotation;
	FVector3 Scale;
	E_EXPECT_TRUE(SequenceMath::EvaluateTransformKeys({ A, B }, 1.0f, Position, Rotation, Scale));
	E_EXPECT_NEAR(Position.X, 50.0f, Tol);
	E_EXPECT_NEAR(Position.Z, 25.0f, Tol);
	E_EXPECT_NEAR(Scale.Y, 2.0f, Tol);
	const FQuat Expected = FQuat::FromEuler(0.0f, 45.0f, 0.0f);
	E_EXPECT_NEAR(FMath::Abs(FQuat::Dot(Rotation, Expected)), 1.0f, Tol);
	E_EXPECT_FALSE(SequenceMath::EvaluateTransformKeys({}, 1.0f, Position, Rotation, Scale));

	// 오일러 펼치기: Yaw 350도는 기준 -5도 근처에서 -10도 (먼 길로 돌지 않게)
	const FVector3 Near = SequenceMath::QuatToEulerNear(FQuat::FromEuler(0.0f, 350.0f, 0.0f), FVector3(0.0f, -5.0f, 0.0f));
	E_EXPECT_NEAR(Near.Y, -10.0f, 0.05f);
	const FVector3 Far = SequenceMath::QuatToEulerNear(FQuat::FromEuler(0.0f, 10.0f, 0.0f), FVector3(0.0f, 700.0f, 0.0f));
	E_EXPECT_NEAR(Far.Y, 730.0f, 0.05f);
	const FVector3 Plain = SequenceMath::QuatToEulerNear(FQuat::FromEuler(20.0f, 30.0f, -15.0f), FVector3());
	E_EXPECT_NEAR(Plain.X, 20.0f, 0.05f);
	E_EXPECT_NEAR(Plain.Y, 30.0f, 0.05f);
	E_EXPECT_NEAR(Plain.Z, -15.0f, 0.05f);
}

E_TEST(Sequence_CutsSectionsEvents)
{
	const std::vector<FSequenceCameraCut> Cuts = { { 1.0f, "A" }, { 3.0f, "B" } };
	E_EXPECT_EQ(SequenceMath::FindCameraCut(Cuts, 0.5f), -1);
	E_EXPECT_EQ(SequenceMath::FindCameraCut(Cuts, 1.0f), 0);
	E_EXPECT_EQ(SequenceMath::FindCameraCut(Cuts, 2.9f), 0);
	E_EXPECT_EQ(SequenceMath::FindCameraCut(Cuts, 3.0f), 1);
	E_EXPECT_EQ(SequenceMath::FindCameraCut({}, 3.0f), -1);

	FSequenceAnimSection First;
	First.Start  = 1.0f;
	First.End    = 2.0f;
	First.Offset = 0.5f;
	First.Rate   = 2.0f;
	FSequenceAnimSection Second;
	Second.Start = 4.0f;
	Second.End   = 5.0f;
	float ClipTime = 0.0f;
	E_EXPECT_EQ(SequenceMath::FindAnimSection({ First, Second }, 0.5f, ClipTime), -1);
	E_EXPECT_EQ(SequenceMath::FindAnimSection({ First, Second }, 1.5f, ClipTime), 0);
	E_EXPECT_NEAR(ClipTime, 1.5f, Tol); // 0.5 + 0.5 × 2
	E_EXPECT_EQ(SequenceMath::FindAnimSection({ First, Second }, 3.0f, ClipTime), 0);
	E_EXPECT_NEAR(ClipTime, 2.5f, Tol); // 끝(2초)에서 고정
	E_EXPECT_EQ(SequenceMath::FindAnimSection({ First, Second }, 4.25f, ClipTime), 1);
	E_EXPECT_NEAR(ClipTime, 0.25f, Tol);

	const std::vector<FSequenceEventKey> Events = { { 0.0f, "Start" }, { 1.0f, "One" }, { 2.0f, "Two" } };
	std::vector<int32>                   Hits;
	SequenceMath::CollectEvents(Events, 0.0f, 0.0f, true, Hits); // 재생 시작 프레임: 0초 이벤트 포함
	E_EXPECT_EQ(Hits.size(), static_cast<size_t>(1));
	Hits.clear();
	SequenceMath::CollectEvents(Events, 0.0f, 1.0f, false, Hits); // (0, 1]
	E_EXPECT_EQ(Hits.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(!Hits.empty() && Hits[0] == 1);
	Hits.clear();
	SequenceMath::CollectEvents(Events, 1.0f, 1.5f, false, Hits);
	E_EXPECT_TRUE(Hits.empty());
}

E_TEST(Sequence_JsonRoundTrip)
{
	FSequenceAsset Asset;
	Asset.Duration  = 7.5f;
	Asset.FrameRate = 24.0f;
	FSequenceTrack Transform;
	Transform.Type   = ESequenceTrackType::Transform;
	Transform.Target = "Gate/Door";
	FSequenceTransformKey TransformKey;
	TransformKey.Time     = 1.0f;
	TransformKey.Position = FVector3(1, 2, 3);
	TransformKey.Rotation = FVector3(10, 20, 30);
	TransformKey.Scale    = FVector3(2, 2, 2);
	TransformKey.Interp   = ESequenceInterp::Constant;
	Transform.TransformKeys.push_back(TransformKey);
	TransformKey.Time = 0.5f; // 정렬 확인
	Transform.TransformKeys.push_back(TransformKey);
	Asset.Tracks.push_back(Transform);
	FSequenceTrack Property;
	Property.Type      = ESequenceTrackType::Property;
	Property.Name      = "조명";
	Property.Target    = "Lamp";
	Property.Component = "PointLightComponent";
	Property.Property  = "Color";
	Property.bMuted    = true;
	Property.ValueKeys.push_back(Key(2.0f, 0.5f, ESequenceInterp::Smooth));
	Asset.Tracks.push_back(Property);
	FSequenceTrack Cut;
	Cut.Type = ESequenceTrackType::CameraCut;
	Cut.Cuts = { { 0.0f, "Cam1" }, { 3.0f, "Cam2" } };
	Asset.Tracks.push_back(Cut);
	FSequenceTrack Animation;
	Animation.Type = ESequenceTrackType::Animation;
	Animation.Target = "Fox";
	FSequenceAnimSection Section;
	Section.Start = 1.0f;
	Section.End   = 4.0f;
	Section.Clip  = "Walk";
	Section.Rate  = 0.5f;
	Section.bLoop = false;
	Animation.Sections.push_back(Section);
	Asset.Tracks.push_back(Animation);
	Asset.Tracks.push_back(FSequenceAsset::MakeDefault().Tracks.front());
	Asset.Tracks.back().Events = { { 2.5f, "Boom" } };

	FSequenceAsset Loaded;
	E_EXPECT_TRUE(FSequenceAsset::FromJsonString(Asset.ToJsonString(), Loaded));
	E_EXPECT_NEAR(Loaded.Duration, 7.5f, Tol);
	E_EXPECT_NEAR(Loaded.FrameRate, 24.0f, Tol);
	E_EXPECT_EQ(Loaded.Tracks.size(), static_cast<size_t>(5));
	if (Loaded.Tracks.size() == 5)
	{
		const FSequenceTrack& T = Loaded.Tracks[0];
		E_EXPECT_TRUE(T.Type == ESequenceTrackType::Transform && T.Target == "Gate/Door" && T.TransformKeys.size() == 2);
		E_EXPECT_NEAR(T.TransformKeys[0].Time, 0.5f, Tol);
		E_EXPECT_NEAR(T.TransformKeys[1].Rotation.Z, 30.0f, Tol);
		E_EXPECT_TRUE(T.TransformKeys[1].Interp == ESequenceInterp::Constant);
		const FSequenceTrack& P = Loaded.Tracks[1];
		E_EXPECT_TRUE(P.Type == ESequenceTrackType::Property && P.Name == "조명" && P.Component == "PointLightComponent" && P.Property == "Color" && P.bMuted);
		E_EXPECT_TRUE(P.ValueKeys.size() == 1 && P.ValueKeys[0].Interp == ESequenceInterp::Smooth);
		E_EXPECT_TRUE(Loaded.Tracks[2].Cuts.size() == 2 && Loaded.Tracks[2].Cuts[1].Camera == "Cam2");
		const FSequenceTrack& A = Loaded.Tracks[3];
		E_EXPECT_TRUE(A.Sections.size() == 1 && A.Sections[0].Clip == "Walk" && !A.Sections[0].bLoop);
		E_EXPECT_NEAR(A.Sections[0].End, 4.0f, Tol);
		E_EXPECT_TRUE(Loaded.Tracks[4].Events.size() == 1 && Loaded.Tracks[4].Events[0].Name == "Boom");
	}
	E_EXPECT_TRUE(Loaded.ToJsonString() == FSequenceAsset(Loaded).ToJsonString());
	FSequenceAsset Bad;
	E_EXPECT_FALSE(FSequenceAsset::FromJsonString("[]", Bad));
}

E_TEST(Sequence_BindingPaths)
{
	FScene        Scene;
	const FEntity GateA = Scene.CreateEntity("Gate");
	const FEntity DoorA = Scene.CreateEntity("Door");
	Scene.SetParent(DoorA, GateA);
	const FEntity Player = Scene.CreateEntity("Cinematic");
	const FEntity GateB  = Scene.CreateEntity("Gate");
	const FEntity DoorB  = Scene.CreateEntity("Door");
	Scene.SetParent(GateB, Player);
	Scene.SetParent(DoorB, GateB);
	const FEntity Other = Scene.CreateEntity("Other");

	// 재생 엔티티 하위가 먼저, 없으면 씬 전체
	E_EXPECT_TRUE(FSequenceSystem::ResolveBinding(Scene, Player, "Door") == DoorB);
	E_EXPECT_TRUE(FSequenceSystem::ResolveBinding(Scene, NullEntity, "Door") == DoorA);
	E_EXPECT_TRUE(FSequenceSystem::ResolveBinding(Scene, Player, "Gate/Door") == DoorB);
	E_EXPECT_TRUE(FSequenceSystem::ResolveBinding(Scene, Player, "Other") == Other);
	E_EXPECT_TRUE(FSequenceSystem::ResolveBinding(Scene, Player, "") == Player);
	E_EXPECT_FALSE(FSequenceSystem::ResolveBinding(Scene, Player, "Gate/Other").IsValid());
	E_EXPECT_FALSE(FSequenceSystem::ResolveBinding(Scene, Player, "Nope").IsValid());

	// 경로 만들기: 하위의 Door는 이름만으로, 바깥 Door는 부모 이름을 붙여도 하위가 먼저라 ... 결국 찾을 수 있는 경로
	E_EXPECT_TRUE(FSequenceSystem::MakeBindingPath(Scene, Player, DoorB) == "Door");
	E_EXPECT_TRUE(FSequenceSystem::MakeBindingPath(Scene, Player, Player).empty());
	E_EXPECT_TRUE(FSequenceSystem::MakeBindingPath(Scene, Player, Other) == "Other");
	E_EXPECT_TRUE(FSequenceSystem::MakeBindingPath(Scene, NullEntity, DoorB) == "Cinematic/Gate/Door");
}

// 재생기: 트랜스폼/프로퍼티/카메라 컷/이벤트 적용, 끝나면 카메라는 원래대로 (값은 유지), RestoreState면 전부 원래대로
E_TEST(Sequence_PlayerAppliesAndRestores)
{
	FSequenceAsset Asset;
	Asset.Duration = 1.0f;
	FSequenceTrack Door;
	Door.Type   = ESequenceTrackType::Transform;
	Door.Target = "Door";
	FSequenceTransformKey K0;
	K0.Interp = ESequenceInterp::Linear;
	FSequenceTransformKey K1 = K0;
	K1.Time                  = 1.0f;
	K1.Position              = FVector3(100.0f, 0.0f, 0.0f);
	Door.TransformKeys       = { K0, K1 };
	Asset.Tracks.push_back(Door);
	FSequenceTrack Fov;
	Fov.Type      = ESequenceTrackType::Property;
	Fov.Target    = "Cam";
	Fov.Component = "CameraComponent";
	Fov.Property  = "FovYDegrees";
	Fov.ValueKeys = { Key(0.0f, 60.0f, ESequenceInterp::Linear), Key(1.0f, 30.0f, ESequenceInterp::Linear) };
	Asset.Tracks.push_back(Fov);
	FSequenceTrack Cut;
	Cut.Type = ESequenceTrackType::CameraCut;
	Cut.Cuts = { { 0.25f, "Cam" } };
	Asset.Tracks.push_back(Cut);
	FSequenceTrack Events;
	Events.Type   = ESequenceTrackType::Event;
	Events.Events = { { 0.0f, "Begin" }, { 0.5f, "Mid" } };
	Asset.Tracks.push_back(Events);
	const std::filesystem::path Path = WriteSequence(L"Player.esequence", Asset);

	FScene        Scene;
	const FEntity DoorEntity = Scene.CreateEntity("Door");
	Scene.GetTransform(DoorEntity).Position = FVector3(5.0f, 6.0f, 7.0f);
	const FEntity     CamEntity = Scene.CreateEntity("Cam");
	FCameraComponent& Camera    = Scene.GetRegistry().Emplace<FCameraComponent>(CamEntity);
	Camera.bPrimary             = false;
	Camera.FovYDegrees          = 70.0f;
	const FEntity             Player    = Scene.CreateEntity("Cinematic");
	FSequencePlayerComponent& Component = Scene.GetRegistry().Emplace<FSequencePlayerComponent>(Player);
	Component.Sequence                  = Path.string();

	FSequenceSystem::Update(Scene, 0.016f); // 자동 재생 시작 프레임: 0초 적용 + 0초 이벤트
	const FSequencePlayerRuntime& Runtime = Scene.GetRegistry().Get<FSequencePlayerComponent>(Player).Runtime;
	E_EXPECT_TRUE(Runtime.bPlaying);
	E_EXPECT_TRUE(Runtime.Events.size() == 1 && Runtime.Events[0] == "Begin");
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 0.0f, Tol);
	E_EXPECT_FALSE(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).bPrimary); // 첫 컷 전

	FSequenceSystem::Update(Scene, 0.5f);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 50.0f, Tol);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).FovYDegrees, 45.0f, Tol);
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).bPrimary);
	E_EXPECT_EQ(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).Priority, FSequenceSystem::CameraCutPriority);
	E_EXPECT_TRUE(Runtime.Events.size() == 1 && Runtime.Events[0] == "Mid");

	FSequenceSystem::Update(Scene, 0.6f); // 끝: 마지막 값 유지, 카메라만 원래대로
	E_EXPECT_FALSE(Runtime.bPlaying);
	E_EXPECT_TRUE(Runtime.bFinishedThisUpdate);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 100.0f, Tol);
	E_EXPECT_FALSE(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).bPrimary);
	E_EXPECT_EQ(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).Priority, 0);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).FovYDegrees, 30.0f, Tol);
	FSequenceSystem::Update(Scene, 0.1f);
	E_EXPECT_FALSE(Runtime.bFinishedThisUpdate);

	// 다시 재생 + RestoreState: 끝나면 처음 건드리기 전 값 (문 5,6,7 / 시야각 70)
	Scene.GetRegistry().Get<FSequencePlayerComponent>(Player).bRestoreState = true;
	E_EXPECT_TRUE(FSequenceSystem::Play(Scene, Player, "", 0.0f));
	FSequenceSystem::Update(Scene, 0.0f);
	FSequenceSystem::Update(Scene, 0.75f);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 75.0f, Tol);
	FSequenceSystem::Stop(Scene, Player);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 5.0f, Tol);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.Z, 7.0f, Tol);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FCameraComponent>(CamEntity).FovYDegrees, 70.0f, Tol);
	E_EXPECT_FALSE(FSequenceSystem::IsPlaying(Scene, Player));

	// 반복: 끝을 넘으면 감아서 계속 (이벤트는 끝 → 처음 순서로)
	FSequencePlayerComponent& Looping = Scene.GetRegistry().Get<FSequencePlayerComponent>(Player);
	Looping.bLoop                     = true;
	FSequenceSystem::Play(Scene, Player, "", 0.9f);
	FSequenceSystem::Update(Scene, 0.0f);
	FSequenceSystem::Update(Scene, 0.2f);
	E_EXPECT_TRUE(FSequenceSystem::IsPlaying(Scene, Player));
	E_EXPECT_NEAR(FSequenceSystem::GetTime(Scene, Player), 0.1f, Tol);
	E_EXPECT_TRUE(Runtime.Events.size() == 1 && Runtime.Events[0] == "Begin");
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 10.0f, Tol);

	// 편집기용 맞바꾸기: 두 번이면 원상태
	FSequenceEvalState State;
	FSequenceSystem::Evaluate(Scene, Player, Asset, 0.5f, State);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 50.0f, Tol);
	State.SwapWithScene(Scene);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 10.0f, Tol);
	State.SwapWithScene(Scene);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 50.0f, Tol);
	FSequenceSystem::Restore(Scene, State);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.X, 10.0f, Tol);
	E_EXPECT_FALSE(State.HasSaved());
	std::error_code ErrorCode;
	std::filesystem::remove(Path, ErrorCode);
}

// 애니메이션 트랙: 구간 시각을 지정해 그 포즈 (반복 구간은 감기), 그래프 없는 모델 루트만
E_TEST(Sequence_AnimationTrackSetsClipTime)
{
	FAnimationClip Move;
	Move.Name     = "Move";
	Move.Duration = 1.0f;
	FAnimationChannel Channel;
	Channel.Node   = 0;
	Channel.Path   = EAnimationPath::Translation;
	Channel.Times  = { 0.0f, 1.0f };
	Channel.Values = { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) };
	Move.Channels.push_back(Channel);
	FAnimationClip Idle = Move;
	Idle.Name           = "Idle";

	FScene               Scene;
	const FEntity        Root      = Scene.CreateEntity("Fox");
	const FEntity        Node      = Scene.CreateEntity("Bone");
	Scene.SetParent(Node, Root);
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Clip                 = "Idle";
	Animation.Runtime.Set          = MakeAnimationSet({ Idle, Move }, { -1 }, std::vector<FNodePose>(1));
	Animation.Runtime.NodeEntities = { Node };

	FSequenceAsset Asset;
	Asset.Duration = 4.0f;
	FSequenceTrack Track;
	Track.Type   = ESequenceTrackType::Animation;
	Track.Target = "Fox";
	FSequenceAnimSection Section;
	Section.Start = 1.0f;
	Section.End   = 3.0f;
	Section.Clip  = "Move";
	Track.Sections.push_back(Section);
	Asset.Tracks.push_back(Track);

	FSequenceEvalState State;
	FSequenceSystem::Evaluate(Scene, NullEntity, Asset, 0.5f, State); // 첫 구간 전: 그대로
	E_EXPECT_TRUE(Animation.Clip == "Idle" && Animation.bPlaying);
	FSequenceSystem::Evaluate(Scene, NullEntity, Asset, 1.25f, State);
	FAnimationSystem::Update(Scene, 0.0f);
	E_EXPECT_TRUE(Animation.Clip == "Move" && !Animation.bPlaying);
	E_EXPECT_NEAR(Scene.GetTransform(Node).Position.X, 25.0f, 0.01f);
	FSequenceSystem::Evaluate(Scene, NullEntity, Asset, 2.5f, State); // 1.5초 → 반복 0.5초
	FAnimationSystem::Update(Scene, 0.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Node).Position.X, 50.0f, 0.01f);
	FSequenceSystem::Restore(Scene, State);
	E_EXPECT_TRUE(Animation.Clip == "Idle" && Animation.bPlaying);
}
