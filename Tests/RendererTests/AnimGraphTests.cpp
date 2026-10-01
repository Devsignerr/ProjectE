#include "Core/Testing/TestFramework.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <filesystem>
#include <fstream>

// 애니메이션 그래프 (Scene/AnimGraph.h): 블렌드 스페이스 가중치, 전이 판정, 포즈 섞기, 상태 머신 진행, 시스템 연동

namespace
{
	constexpr float Tol = 1.0e-4f;

	// 상태: Locomotion = Speed 블렌드 (Idle 0 / Walk 100 / Run 300), Fall = 클립 Fall, Land = 반복 없는 Land
	const char* const TestGraphJson = R"({
		"Version": 1,
		"Parameters": [
			{ "Name": "Speed", "Type": "Float", "Default": 0 },
			{ "Name": "Grounded", "Type": "Bool", "Default": true }
		],
		"EntryState": "Locomotion",
		"States": [
			{ "Name": "Locomotion", "BlendParameter": "Speed", "Samples": [
				{ "Clip": "Run", "Position": 300 }, { "Clip": "Idle", "Position": 0 }, { "Clip": "Walk", "Position": 100 } ] },
			{ "Name": "Fall", "Clip": "Fall" },
			{ "Name": "Land", "Clip": "Land", "Loop": false }
		],
		"Transitions": [
			{ "From": "Locomotion", "To": "Fall", "Duration": 0.2, "Conditions": [ { "Parameter": "Grounded", "Op": "==", "Value": false } ] },
			{ "From": "Fall", "To": "Land", "Duration": 0.0, "Conditions": [ { "Parameter": "Grounded", "Op": "==", "Value": true } ] },
			{ "From": "Land", "To": "Locomotion", "Duration": 0.1, "ExitTime": 1.0 },
			{ "From": "*", "To": "Fall", "Duration": 0.1, "Conditions": [ { "Parameter": "Speed", "Op": "<", "Value": -1 } ] },
			{ "From": "Nowhere", "To": "Fall" }
		]
	})";

	FAnimGraphAsset ParseTestGraph()
	{
		FAnimGraphAsset          Asset;
		std::string              Error;
		std::vector<std::string> Warnings;
		E_EXPECT_TRUE(FAnimGraphAsset::FromJsonString(TestGraphJson, Asset, &Error, &Warnings));
		E_EXPECT_EQ(Warnings.size(), static_cast<size_t>(1)); // "Nowhere" 전이
		return Asset;
	}

	// 클립 번호: Idle 0 (1초), Walk 1 (1초), Run 2 (2초), Fall 3 (1초), Land 4 (0.5초)
	FAnimGraphBinding MakeTestBinding(const FAnimGraphAsset& Asset)
	{
		const std::vector<std::pair<std::string, float>> Clips = { { "Idle", 1.0f }, { "Walk", 1.0f }, { "Run", 2.0f }, { "Fall", 1.0f }, { "Land", 0.5f } };
		FAnimGraphBinding Binding;
		Binding.SampleClips.resize(Asset.States.size());
		for (size_t State = 0; State < Asset.States.size(); ++State)
		{
			for (const FAnimBlendSample& Sample : Asset.States[State].Samples)
			{
				int32 Found = -1;
				for (size_t Clip = 0; Clip < Clips.size(); ++Clip)
				{
					if (Clips[Clip].first == Sample.Clip)
					{
						Found = static_cast<int32>(Clip);
					}
				}
				Binding.SampleClips[State].push_back(Found);
			}
		}
		for (const auto& Clip : Clips)
		{
			Binding.ClipDurations.push_back(Clip.second);
		}
		return Binding;
	}

	float WeightOf(const FAnimGraphInstance& Instance, int32 Clip)
	{
		float Weight = 0.0f;
		for (const FAnimClipContribution& Contribution : Instance.GetContributions())
		{
			if (Contribution.Clip == Clip)
			{
				Weight += Contribution.Weight;
			}
		}
		return Weight;
	}

	float TimeOf(const FAnimGraphInstance& Instance, int32 Clip)
	{
		for (const FAnimClipContribution& Contribution : Instance.GetContributions())
		{
			if (Contribution.Clip == Clip)
			{
				return Contribution.Time;
			}
		}
		return -1.0f;
	}
} // namespace

E_TEST(AnimGraph_BlendSpace1DWeights)
{
	const std::vector<float> Positions = { 0.0f, 150.0f, 450.0f };
	std::vector<float>       Weights;
	AnimGraphMath::ComputeBlendSpace1DWeights(Positions, -10.0f, Weights);
	E_EXPECT_NEAR(Weights[0], 1.0f, Tol);
	E_EXPECT_NEAR(Weights[1], 0.0f, Tol);
	AnimGraphMath::ComputeBlendSpace1DWeights(Positions, 75.0f, Weights);
	E_EXPECT_NEAR(Weights[0], 0.5f, Tol);
	E_EXPECT_NEAR(Weights[1], 0.5f, Tol);
	E_EXPECT_NEAR(Weights[2], 0.0f, Tol);
	AnimGraphMath::ComputeBlendSpace1DWeights(Positions, 375.0f, Weights);
	E_EXPECT_NEAR(Weights[1], 0.25f, Tol);
	E_EXPECT_NEAR(Weights[2], 0.75f, Tol);
	AnimGraphMath::ComputeBlendSpace1DWeights(Positions, 150.0f, Weights);
	E_EXPECT_NEAR(Weights[1], 1.0f, Tol);
	AnimGraphMath::ComputeBlendSpace1DWeights(Positions, 1000.0f, Weights);
	E_EXPECT_NEAR(Weights[2], 1.0f, Tol);
	AnimGraphMath::ComputeBlendSpace1DWeights({ 5.0f }, 1000.0f, Weights);
	E_EXPECT_NEAR(Weights[0], 1.0f, Tol);
}

E_TEST(AnimGraph_ParseAndTransitionRules)
{
	const FAnimGraphAsset Asset = ParseTestGraph();
	E_EXPECT_EQ(Asset.States.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Asset.Transitions.size(), static_cast<size_t>(4));
	// 샘플은 Position 순으로 정렬된다
	E_EXPECT_TRUE(Asset.States[0].Samples[0].Clip == "Idle");
	E_EXPECT_TRUE(Asset.States[0].Samples[2].Clip == "Run");
	E_EXPECT_FALSE(Asset.States[2].bLoop);

	FAnimParameterSet Parameters;
	// 선언 기본값: Grounded = true → 전이 없음
	E_EXPECT_EQ(AnimGraphMath::FindTransition(Asset, 0, 0.0f, Parameters), -1);
	Parameters.Set("Grounded", 0.0f);
	E_EXPECT_EQ(AnimGraphMath::FindTransition(Asset, 0, 0.0f, Parameters), 0);
	// 목록 순서 우선: Speed < -1이어도 Locomotion에서는 0번이 먼저
	Parameters.Set("Speed", -5.0f);
	E_EXPECT_EQ(AnimGraphMath::FindTransition(Asset, 0, 0.0f, Parameters), 0);
	// "*" 전이는 To 자신(Fall)에서는 고르지 않는다
	E_EXPECT_EQ(AnimGraphMath::FindTransition(Asset, 1, 0.0f, Parameters), -1);
	// Land → Locomotion은 ExitTime 1 (끝까지 재생)
	Parameters.Set("Speed", 0.0f);
	Parameters.Set("Grounded", 1.0f);
	E_EXPECT_EQ(AnimGraphMath::FindTransition(Asset, 2, 0.5f, Parameters), -1);
	E_EXPECT_EQ(AnimGraphMath::FindTransition(Asset, 2, 1.0f, Parameters), 2);

	E_EXPECT_TRUE(AnimGraphMath::EvaluateCondition(EAnimConditionOp::GreaterEqual, 3.0f, 3.0f));
	E_EXPECT_FALSE(AnimGraphMath::EvaluateCondition(EAnimConditionOp::Greater, 3.0f, 3.0f));
	E_EXPECT_TRUE(AnimGraphMath::EvaluateCondition(EAnimConditionOp::NotEqual, 1.0f, 0.0f));

	// 형식 오류
	FAnimGraphAsset Bad;
	E_EXPECT_FALSE(FAnimGraphAsset::FromJsonString("{}", Bad));
	E_EXPECT_FALSE(FAnimGraphAsset::FromJsonString(R"({"States":[{"Name":"A","Clip":"X"},{"Name":"A","Clip":"Y"}]})", Bad));
	E_EXPECT_FALSE(FAnimGraphAsset::FromJsonString(R"({"States":[{"Name":"A","Clip":"X"}],"EntryState":"B"})", Bad));
}

E_TEST(AnimGraph_WeightedPoseNlerp)
{
	const FQuat Quarter = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(90.0f));
	std::vector<FNodePose> A(1), B(1), Sum;
	A[0].Translation = FVector3(0, 0, 0);
	B[0].Translation = FVector3(100, 0, 0);
	B[0].Rotation    = Quarter;
	B[0].Scale       = FVector3(3, 3, 3);
	AnimGraphMath::AddWeightedPose(Sum, A, 0.5f, true);
	AnimGraphMath::AddWeightedPose(Sum, B, 0.5f, false);
	AnimGraphMath::FinishWeightedPose(Sum);
	E_EXPECT_EQUALS(Sum[0].Translation, FVector3(50, 0, 0), Tol);
	E_EXPECT_EQUALS(Sum[0].Scale, FVector3(2, 2, 2), Tol);
	E_EXPECT_EQUALS(Sum[0].Rotation, FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(45.0f)), 1.0e-4f);

	// -q는 같은 회전: 반구를 맞춰 더하므로 결과가 같다
	std::vector<FNodePose> Negated = B;
	Negated[0].Rotation            = FQuat(-Quarter.X, -Quarter.Y, -Quarter.Z, -Quarter.W);
	std::vector<FNodePose> Sum2;
	AnimGraphMath::AddWeightedPose(Sum2, A, 0.5f, true);
	AnimGraphMath::AddWeightedPose(Sum2, Negated, 0.5f, false);
	AnimGraphMath::FinishWeightedPose(Sum2);
	E_EXPECT_EQUALS(Sum2[0].Rotation, Sum[0].Rotation, 1.0e-4f);

	// 가중치 1 하나면 그대로
	std::vector<FNodePose> Single;
	AnimGraphMath::AddWeightedPose(Single, B, 1.0f, true);
	AnimGraphMath::FinishWeightedPose(Single);
	E_EXPECT_EQUALS(Single[0].Rotation, Quarter, Tol);
}

E_TEST(AnimGraph_BlendSpaceSyncedPhase)
{
	const FAnimGraphAsset   Asset   = ParseTestGraph();
	const FAnimGraphBinding Binding = MakeTestBinding(Asset);
	FAnimParameterSet       Parameters;
	FAnimGraphInstance      Instance;

	Instance.Update(Asset, Binding, Parameters, 0.5f); // 시작 프레임은 진행하지 않는다
	E_EXPECT_EQ(Instance.GetCurrentState(), 0);
	E_EXPECT_NEAR(WeightOf(Instance, 0), 1.0f, Tol);
	E_EXPECT_NEAR(TimeOf(Instance, 0), 0.0f, Tol);

	// Speed 200 = Walk(1초) 0.5 + Run(2초) 0.5 → 한 바퀴 1.5초. 0.75초 뒤 Phase 0.5: Walk 0.5초, Run 1.0초 (같은 정규화 시간)
	Parameters.Set("Speed", 200.0f);
	Instance.Update(Asset, Binding, Parameters, 0.75f);
	E_EXPECT_NEAR(WeightOf(Instance, 1), 0.5f, Tol);
	E_EXPECT_NEAR(WeightOf(Instance, 2), 0.5f, Tol);
	E_EXPECT_NEAR(TimeOf(Instance, 1), 0.5f, 1.0e-3f);
	E_EXPECT_NEAR(TimeOf(Instance, 2), 1.0f, 1.0e-3f);
	E_EXPECT_NEAR(Instance.GetCurrentPhase(), 0.5f, 1.0e-3f);

	// 노티파이 기준 = 가중치 최대 (Speed 250 → Run 0.75)
	Parameters.Set("Speed", 250.0f);
	Instance.Update(Asset, Binding, Parameters, 0.1f);
	E_EXPECT_EQ(Instance.GetNotifySource().Clip, 2);
	E_EXPECT_TRUE(Instance.GetNotifySource().Delta > 0.0f);

	// 루프: 한 바퀴 넘게 진행하면 감긴다
	Parameters.Set("Speed", 0.0f); // Idle 1초
	Instance.Update(Asset, Binding, Parameters, 1.25f);
	E_EXPECT_TRUE(Instance.GetCurrentPhase() < 1.0f);
	E_EXPECT_TRUE(Instance.GetNotifySource().bWrapped);
}

E_TEST(AnimGraph_CrossfadeAndInterrupt)
{
	const FAnimGraphAsset   Asset   = ParseTestGraph();
	const FAnimGraphBinding Binding = MakeTestBinding(Asset);
	FAnimParameterSet       Parameters;
	FAnimGraphInstance      Instance;
	Instance.Update(Asset, Binding, Parameters, 0.0f);

	// 바닥을 떠남 → Fall로 0.2초 크로스페이드. 전이한 프레임은 이전 포즈 그대로
	Parameters.Set("Grounded", false);
	Instance.Update(Asset, Binding, Parameters, 0.1f);
	E_EXPECT_EQ(Instance.GetCurrentState(), 1);
	E_EXPECT_EQ(Instance.GetLayerCount(), static_cast<size_t>(2));
	E_EXPECT_NEAR(WeightOf(Instance, 0), 1.0f, Tol);
	E_EXPECT_NEAR(WeightOf(Instance, 3), 0.0f, Tol);

	// 절반(0.1초): smoothstep(0.5) = 0.5
	Instance.Update(Asset, Binding, Parameters, 0.1f);
	E_EXPECT_NEAR(WeightOf(Instance, 3), 0.5f, 1.0e-3f);
	E_EXPECT_NEAR(WeightOf(Instance, 0), 0.5f, 1.0e-3f);
	E_EXPECT_NEAR(TimeOf(Instance, 3), 0.1f, 1.0e-3f); // 새 상태는 전이 다음 프레임부터 진행

	// 페이드 중 착지 → Land(페이드 0): 즉시 Land만
	Parameters.Set("Grounded", true);
	Instance.Update(Asset, Binding, Parameters, 0.0f);
	E_EXPECT_EQ(Instance.GetCurrentState(), 2);
	E_EXPECT_EQ(Instance.GetLayerCount(), static_cast<size_t>(1));
	E_EXPECT_NEAR(WeightOf(Instance, 4), 1.0f, Tol);

	// Land(0.5초, 반복 없음)는 끝까지 재생된 뒤 ExitTime 1로 Locomotion에 0.1초 페이드
	Instance.Update(Asset, Binding, Parameters, 0.3f);
	E_EXPECT_EQ(Instance.GetCurrentState(), 2);
	Instance.Update(Asset, Binding, Parameters, 0.3f);
	E_EXPECT_EQ(Instance.GetCurrentState(), 0);
	E_EXPECT_NEAR(TimeOf(Instance, 4), 0.5f, Tol); // 끝 포즈에서 멈춘 채 섞인다
	Instance.Update(Asset, Binding, Parameters, 0.2f);
	E_EXPECT_EQ(Instance.GetLayerCount(), static_cast<size_t>(1));
	E_EXPECT_NEAR(WeightOf(Instance, 0), 1.0f, Tol);

	// 페이드 중 원래 상태로 돌아감: 가중치가 끊기지 않고 이어진다 (Locomotion 0.5 → 다시 Locomotion에서 출발)
	Parameters.Set("Grounded", false);
	Instance.Update(Asset, Binding, Parameters, 0.0f);
	Instance.Update(Asset, Binding, Parameters, 0.1f);
	const float LocomotionBefore = WeightOf(Instance, 0);
	E_EXPECT_NEAR(LocomotionBefore, 0.5f, 1.0e-3f);
	Parameters.Set("Grounded", true); // 페이드 중 Fall → Land(페이드 0): 섞이던 Locomotion도 함께 정리된다
	Instance.Update(Asset, Binding, Parameters, 0.0f);
	E_EXPECT_EQ(Instance.GetCurrentState(), 2);
	float Total = 0.0f;
	for (const FAnimClipContribution& Contribution : Instance.GetContributions())
	{
		Total += Contribution.Weight;
	}
	E_EXPECT_NEAR(Total, 1.0f, Tol);
}

E_TEST(AnimGraph_InterruptResumesFadingState)
{
	// A ↔ B를 파라미터 하나로 오가는 그래프: 페이드 절반에서 A로 돌아가면 A 가중치가 0.5에서 이어진다
	FAnimGraphAsset Asset;
	E_EXPECT_TRUE(FAnimGraphAsset::FromJsonString(R"({
		"Parameters": [ { "Name": "B", "Type": "Bool" } ],
		"States": [ { "Name": "A", "Clip": "Idle" }, { "Name": "B", "Clip": "Walk" } ],
		"Transitions": [
			{ "From": "A", "To": "B", "Duration": 0.2, "Conditions": [ { "Parameter": "B", "Op": "==", "Value": true } ] },
			{ "From": "B", "To": "A", "Duration": 0.2, "Conditions": [ { "Parameter": "B", "Op": "==", "Value": false } ] }
		]
	})", Asset));
	const FAnimGraphBinding Binding = MakeTestBinding(Asset);
	FAnimParameterSet       Parameters;
	FAnimGraphInstance      Instance;
	Instance.Update(Asset, Binding, Parameters, 0.0f);
	Parameters.Set("B", true);
	Instance.Update(Asset, Binding, Parameters, 0.0f);
	Instance.Update(Asset, Binding, Parameters, 0.1f);
	E_EXPECT_NEAR(WeightOf(Instance, 0), 0.5f, 1.0e-3f);
	const float IdleTime = TimeOf(Instance, 0);

	Parameters.Set("B", false);
	Instance.Update(Asset, Binding, Parameters, 0.0f);
	E_EXPECT_EQ(Instance.GetCurrentState(), 0);
	E_EXPECT_EQ(Instance.GetLayerCount(), static_cast<size_t>(2)); // 새 레이어를 만들지 않고 A를 이어 감
	E_EXPECT_NEAR(WeightOf(Instance, 0), 0.5f, 1.0e-3f);
	E_EXPECT_NEAR(TimeOf(Instance, 0), IdleTime, 1.0e-4f);
	Instance.Update(Asset, Binding, Parameters, 0.1f); // 0.5 + 0.5 × 0.5
	E_EXPECT_NEAR(WeightOf(Instance, 0), 0.75f, 1.0e-3f);
	Instance.Update(Asset, Binding, Parameters, 0.1f);
	E_EXPECT_NEAR(WeightOf(Instance, 0), 1.0f, Tol);
	E_EXPECT_EQ(Instance.GetLayerCount(), static_cast<size_t>(1));
}

// 시스템 연동: 그래프 파일 → 노드 트랜스폼, 파라미터 API, 노티파이는 가중치 최대 클립만
E_TEST(AnimGraph_SystemDrivesPoseAndNotifies)
{
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEAnimGraphTests";
	std::filesystem::create_directories(Directory);
	const std::filesystem::path GraphPath = Directory / L"Test.eanimgraph";
	{
		std::ofstream File(GraphPath, std::ios::binary | std::ios::trunc);
		File << R"({
			"Parameters": [ { "Name": "Speed" }, { "Name": "Up", "Type": "Bool" } ],
			"States": [
				{ "Name": "Ground", "BlendParameter": "Speed", "Samples": [ { "Clip": "Still", "Position": 0 }, { "Clip": "Move", "Position": 100 } ] },
				{ "Name": "Air", "Clip": "Missing" }
			],
			"Transitions": [ { "From": "Ground", "To": "Air", "Duration": 0, "Conditions": [ { "Parameter": "Up", "Op": "==", "Value": true } ] } ]
		})";
	}
	FAnimGraphLibrary::Get().Invalidate();

	// 노드 1개, Move: X 0→100 (1초), Still: X 고정 0 (1초)
	FAnimationClip Move;
	Move.Name     = "Move";
	Move.Duration = 1.0f;
	FAnimationChannel Channel;
	Channel.Node   = 0;
	Channel.Path   = EAnimationPath::Translation;
	Channel.Times  = { 0.0f, 1.0f };
	Channel.Values = { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) };
	Move.Channels.push_back(Channel);
	FAnimationClip Still = Move;
	Still.Name           = "Still";
	Still.Channels[0].Values = { FVector4(0, 0, 0, 0), FVector4(0, 0, 0, 0) };

	FScene        Scene;
	const FEntity Character = Scene.CreateEntity("Character");
	const FEntity Root      = Scene.CreateEntity("Model");
	const FEntity Node      = Scene.CreateEntity("Node");
	Scene.SetParent(Root, Character);
	Scene.SetParent(Node, Root);
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Runtime.Set          = MakeAnimationSet({ Move, Still }, { -1 }, std::vector<FNodePose>(1));
	Animation.Runtime.NodeEntities = { Node };
	auto Metadata                  = std::make_shared<FModelMetadata>();
	Metadata->GetOrAddNotifies("Move").push_back({ "Step", EAnimNotifyKind::Notify, 0.5f, 0.0f });
	Metadata->GetOrAddNotifies("Still").push_back({ "Rest", EAnimNotifyKind::Notify, 0.5f, 0.0f });
	Animation.Runtime.Metadata = Metadata;
	Scene.GetRegistry().Emplace<FAnimGraphComponent>(Root).Graph = GraphPath.string();

	// 조상(캐릭터 루트)에서 파라미터 설정 → 자손 그래프로
	E_EXPECT_TRUE(FAnimationSystem::SetAnimParam(Scene, Character, "Speed", 75.0f));
	E_EXPECT_FALSE(FAnimationSystem::SetAnimParam(Scene, Node, "Speed", 1.0f));
	FAnimationSystem::Update(Scene, 0.0f);
	E_EXPECT_TRUE(FAnimationSystem::GetAnimState(Scene, Character) == "Ground");
	E_EXPECT_TRUE(FAnimationSystem::IsAnimParamBool(Scene, Character, "Up"));
	E_EXPECT_NEAR(*FAnimationSystem::GetAnimParam(Scene, Character, "Up"), 0.0f, Tol);

	// 0.6초: 같은 Phase 0.6 → Move 60 × 0.75 + Still 0 × 0.25 = 45. 노티파이는 Move(가중치 0.75)의 Step만
	FAnimationSystem::Update(Scene, 0.6f);
	E_EXPECT_NEAR(Scene.GetTransform(Node).Position.X, 45.0f, 1.0e-2f);
	const std::vector<FAnimNotifyEvent>& Events = Animation.Runtime.PendingNotifies;
	E_EXPECT_EQ(Events.size(), static_cast<size_t>(1));
	if (!Events.empty())
	{
		E_EXPECT_TRUE(Events[0].Name == "Step");
	}
	E_EXPECT_TRUE(FAnimationSystem::GetCurrentClip(Scene, Root) == "Move");

	// 없는 클립만 있는 상태: 기여가 없어 포즈를 그대로 둔다 (경고만)
	E_EXPECT_TRUE(FAnimationSystem::SetAnimParam(Scene, Root, "Up", true));
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(FAnimationSystem::GetAnimState(Scene, Root) == "Air");
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_NEAR(Scene.GetTransform(Node).Position.X, 45.0f, 1.0e-2f);

	// 그래프 경로를 비우면 Clip 재생으로 돌아간다
	Scene.GetRegistry().Get<FAnimGraphComponent>(Root).Graph.clear();
	Animation.Clip = "Move";
	FAnimationSystem::Update(Scene, 0.25f);
	E_EXPECT_TRUE(FAnimationSystem::GetCurrentClip(Scene, Root) == "Move");
	std::error_code ErrorCode;
	std::filesystem::remove_all(Directory, ErrorCode);
}

// 형식 왕복 (Phase 35-1): v1 → 구조체 → v2 JSON → 구조체가 같다. 편집기 정보(위치/미리보기 모델) 유지, bool 조건은 true/false
E_TEST(AnimGraph_JsonRoundTripV2)
{
	FAnimGraphAsset Asset           = ParseTestGraph();
	Asset.States[0].EditorPosition  = FVector2(10.0f, -20.0f);
	Asset.AnyStateEditorPosition    = FVector2(-300.0f, 5.0f);
	Asset.PreviewModel              = "Fox.glb";
	Asset.States[2].Speed           = 0.5f;
	Asset.States[1].Samples[0].Rate = 2.0f;

	const std::string Text = Asset.ToJsonString();
	E_EXPECT_TRUE(Text.find("\"Version\": 2") != std::string::npos);
	E_EXPECT_TRUE(Text.find("\"Value\": false") != std::string::npos); // Grounded == false

	FAnimGraphAsset          Loaded;
	std::string              Error;
	std::vector<std::string> Warnings;
	E_EXPECT_TRUE(FAnimGraphAsset::FromJsonString(Text, Loaded, &Error, &Warnings));
	E_EXPECT_TRUE(Warnings.empty());
	E_EXPECT_EQ(Loaded.States.size(), Asset.States.size());
	E_EXPECT_EQ(Loaded.Transitions.size(), Asset.Transitions.size());
	E_EXPECT_EQ(Loaded.Parameters.size(), Asset.Parameters.size());
	E_EXPECT_EQ(Loaded.EntryState, Asset.EntryState);
	E_EXPECT_TRUE(Loaded.PreviewModel == "Fox.glb");
	E_EXPECT_TRUE(Loaded.AnyStateEditorPosition.has_value() && Loaded.AnyStateEditorPosition->X == -300.0f);
	E_EXPECT_TRUE(Loaded.States[0].EditorPosition.has_value() && Loaded.States[0].EditorPosition->Y == -20.0f);
	E_EXPECT_FALSE(Loaded.States[1].EditorPosition.has_value());
	for (size_t Index = 0; Index < Asset.States.size() && Index < Loaded.States.size(); ++Index)
	{
		const FAnimGraphState& A = Asset.States[Index];
		const FAnimGraphState& B = Loaded.States[Index];
		E_EXPECT_TRUE(A.Name == B.Name && A.BlendParameter == B.BlendParameter && A.bLoop == B.bLoop);
		E_EXPECT_NEAR(A.Speed, B.Speed, Tol);
		E_EXPECT_EQ(A.Samples.size(), B.Samples.size());
		for (size_t Sample = 0; Sample < A.Samples.size() && Sample < B.Samples.size(); ++Sample)
		{
			E_EXPECT_TRUE(A.Samples[Sample].Clip == B.Samples[Sample].Clip);
			E_EXPECT_NEAR(A.Samples[Sample].Position, B.Samples[Sample].Position, Tol);
			E_EXPECT_NEAR(A.Samples[Sample].Rate, B.Samples[Sample].Rate, Tol);
		}
	}
	for (size_t Index = 0; Index < Asset.Transitions.size() && Index < Loaded.Transitions.size(); ++Index)
	{
		const FAnimGraphTransition& A = Asset.Transitions[Index];
		const FAnimGraphTransition& B = Loaded.Transitions[Index];
		E_EXPECT_EQ(A.From, B.From);
		E_EXPECT_EQ(A.To, B.To);
		E_EXPECT_NEAR(A.Duration, B.Duration, Tol);
		E_EXPECT_NEAR(A.ExitTime, B.ExitTime, Tol);
		E_EXPECT_EQ(A.Conditions.size(), B.Conditions.size());
		for (size_t Condition = 0; Condition < A.Conditions.size() && Condition < B.Conditions.size(); ++Condition)
		{
			E_EXPECT_TRUE(A.Conditions[Condition].Parameter == B.Conditions[Condition].Parameter);
			E_EXPECT_TRUE(A.Conditions[Condition].Op == B.Conditions[Condition].Op);
			E_EXPECT_NEAR(A.Conditions[Condition].Value, B.Conditions[Condition].Value, Tol);
		}
	}
	// 다시 쓰면 같은 텍스트 (안정적인 출력 — 실행 취소 스냅샷 비교에 쓰인다)
	E_EXPECT_TRUE(Loaded.ToJsonString() == Text);

	// 기본 에셋도 왕복된다 (빈 클립 상태 허용)
	FAnimGraphAsset Default;
	E_EXPECT_TRUE(FAnimGraphAsset::FromJsonString(FAnimGraphAsset::MakeDefault().ToJsonString(), Default));
	E_EXPECT_EQ(Default.States.size(), static_cast<size_t>(1));
}

// 상태 삭제: 그 상태를 쓰는 전이 제거 + 번호 당김 + 시작 상태 보정
E_TEST(AnimGraph_RemoveStateFixesIndices)
{
	FAnimGraphAsset Asset = ParseTestGraph(); // Locomotion 0, Fall 1, Land 2
	Asset.EntryState      = 2;
	Asset.RemoveState(1); // Fall이 From/To인 전이(0, 1, 3)는 사라지고 Land → Locomotion만 남는다
	E_EXPECT_EQ(Asset.States.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Asset.Transitions.size(), static_cast<size_t>(1));
	if (!Asset.Transitions.empty())
	{
		E_EXPECT_EQ(Asset.Transitions[0].From, 1);
		E_EXPECT_EQ(Asset.Transitions[0].To, 0);
	}
	E_EXPECT_EQ(Asset.EntryState, 1);
	Asset.RemoveState(1);
	E_EXPECT_EQ(Asset.EntryState, 0);
	E_EXPECT_TRUE(Asset.Transitions.empty());
}

// 핫 리로드: 파일을 바꾸고 Invalidate → 다음 갱신에서 새 그래프. 파라미터 값 유지, 같은 이름 상태에서 이어 간다
E_TEST(AnimGraph_HotReloadKeepsParametersAndState)
{
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEAnimGraphReload";
	std::filesystem::create_directories(Directory);
	const std::filesystem::path GraphPath  = Directory / L"Reload.eanimgraph";
	const auto                  WriteGraph = [&](const char* Text) {
		std::ofstream File(GraphPath, std::ios::binary | std::ios::trunc);
		File << Text;
	};
	WriteGraph(R"({
		"Parameters": [ { "Name": "Go", "Type": "Bool" } ],
		"States": [ { "Name": "A", "Clip": "Still" }, { "Name": "B", "Clip": "Move" } ],
		"Transitions": [ { "From": "A", "To": "B", "Duration": 0, "Conditions": [ { "Parameter": "Go", "Op": "==", "Value": true } ] } ]
	})");
	FAnimGraphLibrary::Get().Invalidate();

	FAnimationClip Move;
	Move.Name            = "Move";
	Move.Duration        = 1.0f;
	FAnimationClip Still = Move;
	Still.Name           = "Still";

	FScene               Scene;
	const FEntity        Root      = Scene.CreateEntity("Model");
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Runtime.Set          = MakeAnimationSet({ Move, Still }, { -1 }, std::vector<FNodePose>(1));
	Scene.GetRegistry().Emplace<FAnimGraphComponent>(Root).Graph = GraphPath.string();

	FAnimationSystem::SetAnimParam(Scene, Root, "Go", true);
	FAnimationSystem::Update(Scene, 0.0f);
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(FAnimationSystem::GetAnimState(Scene, Root) == "B");
	const std::shared_ptr<const FAnimGraphAsset> Before = Scene.GetRegistry().Get<FAnimGraphComponent>(Root).Runtime.Asset;

	// 다른 파일 무효화로 세대만 바뀌면 같은 에셋 객체 그대로 (다시 시작하지 않음)
	FAnimGraphLibrary::Get().Invalidate((Directory / L"Other.eanimgraph").string());
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FAnimGraphComponent>(Root).Runtime.Asset == Before);

	// 새 그래프: 상태 C 추가, 시작 상태 A. B가 남아 있으므로 B에서 이어 간다. 파라미터 Go는 그대로 true
	WriteGraph(R"({
		"Parameters": [ { "Name": "Go", "Type": "Bool" }, { "Name": "Extra" } ],
		"EntryState": "A",
		"States": [ { "Name": "A", "Clip": "Still" }, { "Name": "C", "Clip": "Still" }, { "Name": "B", "Clip": "Move" } ],
		"Transitions": [ { "From": "B", "To": "C", "Duration": 0, "Conditions": [ { "Parameter": "Extra", "Op": ">", "Value": 1 } ] } ]
	})");
	FAnimGraphLibrary::Get().Invalidate(GraphPath.string());
	FAnimationSystem::Update(Scene, 0.1f);
	const FAnimGraphRuntime& Runtime = Scene.GetRegistry().Get<FAnimGraphComponent>(Root).Runtime;
	E_EXPECT_TRUE(Runtime.Asset != Before);
	E_EXPECT_TRUE(Runtime.Asset != nullptr && Runtime.Asset->States.size() == 3);
	E_EXPECT_TRUE(FAnimationSystem::GetAnimState(Scene, Root) == "B");
	E_EXPECT_NEAR(FAnimationSystem::GetAnimParam(Scene, Root, "Go").value_or(-1.0f), 1.0f, Tol);
	FAnimationSystem::SetAnimParam(Scene, Root, "Extra", 5.0f);
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(FAnimationSystem::GetAnimState(Scene, Root) == "C");

	// 이어 갈 상태가 없어지면 시작 상태부터. 선언에서 빠진 파라미터 값도 지우지 않는다
	WriteGraph(R"({ "States": [ { "Name": "Z", "Clip": "Still" } ] })");
	FAnimGraphLibrary::Get().Invalidate(GraphPath.string());
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(FAnimationSystem::GetAnimState(Scene, Root) == "Z");
	E_EXPECT_NEAR(FAnimationSystem::GetAnimParam(Scene, Root, "Extra").value_or(-1.0f), 5.0f, Tol);

	std::error_code ErrorCode;
	std::filesystem::remove_all(Directory, ErrorCode);
}
