#include "Core/Console/Console.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/AnimNotify.h"
#include "Scene/AnimUpdateRate.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <memory>
#include <string>

namespace
{
	constexpr float Tol = 1.0e-3f;

	FAnimNotify MakeNotify(const char* Name, float Time, float Duration = 0.0f)
	{
		FAnimNotify Notify;
		Notify.Name     = Name;
		Notify.Time     = Time;
		Notify.Kind     = Duration > 0.0f ? EAnimNotifyKind::State : EAnimNotifyKind::Notify;
		Notify.Duration = Duration;
		return Notify;
	}

	// 클립 "Walk" (1초, 노드 0 X 이동 0 → 100)
	std::shared_ptr<const FAnimationSet> MakeWalkSet()
	{
		FAnimationChannel Channel;
		Channel.Node   = 0;
		Channel.Path   = EAnimationPath::Translation;
		Channel.Times  = { 0.0f, 1.0f };
		Channel.Values = { FVector4(0, 0, 0, 0), FVector4(100, 0, 0, 0) };
		FAnimationClip Walk;
		Walk.Name     = "Walk";
		Walk.Duration = 1.0f;
		Walk.Channels.push_back(Channel);
		return MakeAnimationSet({ Walk }, { -1 }, std::vector<FNodePose>(1));
	}

	struct FTestModel
	{
		FEntity Root;
		FEntity Node;
	};

	FTestModel CreateModel(FScene& Scene, const std::shared_ptr<const FModelMetadata>& Metadata)
	{
		FTestModel Model;
		Model.Root = Scene.CreateEntity("Model");
		Model.Node = Scene.CreateEntity("Node");
		Scene.SetParent(Model.Node, Model.Root);
		FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Model.Root);
		Animation.Clip                 = "Walk";
		Animation.Runtime.Set          = MakeWalkSet();
		Animation.Runtime.NodeEntities = { Model.Node };
		Animation.Runtime.Metadata     = Metadata;
		return Model;
	}

	// 스테이트 Tick(평가마다 한 번)을 뺀 이벤트 이름
	std::string DescribeEvents(const std::vector<FAnimNotifyEvent>& Events)
	{
		std::string Result;
		for (const FAnimNotifyEvent& Event : Events)
		{
			static constexpr const char* Types[] = { "", ".Begin", ".Tick", ".End" };
			if (Event.Type != EAnimNotifyEventType::StateTick)
			{
				Result += Event.Name + Types[static_cast<int32>(Event.Type)] + ' ';
			}
		}
		return Result;
	}
} // namespace

E_TEST(AnimUpdateRate_IntervalAndStagger)
{
	AnimUpdateRateMath::FSettings Settings;
	Settings.FullRateScreenSize = 0.2f;
	Settings.HalfRateScreenSize = 0.08f;
	Settings.MaxInterval        = 4;
	E_EXPECT_EQ(AnimUpdateRateMath::SelectInterval(0.5f, Settings), 1u);
	E_EXPECT_EQ(AnimUpdateRateMath::SelectInterval(0.1f, Settings), 2u);
	E_EXPECT_EQ(AnimUpdateRateMath::SelectInterval(0.01f, Settings), 4u);
	Settings.MaxInterval = 1;
	E_EXPECT_EQ(AnimUpdateRateMath::SelectInterval(0.1f, Settings), 1u);

	// 화면 크기: 반경 100cm, 거리 = 100 / tan(30°)이면 화면 세로 절반과 같다 → 1
	const float Distance = 100.0f / std::tan(FMath::DegreesToRadians(30.0f));
	E_EXPECT_NEAR(AnimUpdateRateMath::ComputeScreenSize(100.0f, Distance, FMath::DegreesToRadians(60.0f), false, 0.0f), 1.0f, Tol);
	E_EXPECT_NEAR(AnimUpdateRateMath::ComputeScreenSize(100.0f, 0.0f, 0.0f, true, 1000.0f), 0.2f, Tol);

	// 간격 4: 4프레임마다 정확히 한 번, 엔티티 번호로 엇갈림, 처음은 항상 평가
	for (uint32 Phase = 0; Phase < 4; ++Phase)
	{
		uint32 Count = 0;
		for (uint32 Tick = 0; Tick < 16; ++Tick)
		{
			Count += AnimUpdateRateMath::ShouldEvaluate(Tick, Phase, 4, false) ? 1u : 0u;
		}
		E_EXPECT_EQ(Count, 4u);
		E_EXPECT_TRUE(AnimUpdateRateMath::ShouldEvaluate((4 - Phase) % 4, Phase, 4, false));
	}
	E_EXPECT_FALSE(AnimUpdateRateMath::ShouldEvaluate(1, 0, 4, false));
	E_EXPECT_TRUE(AnimUpdateRateMath::ShouldEvaluate(1, 0, 4, true));
	E_EXPECT_TRUE(AnimUpdateRateMath::ShouldEvaluate(1, 0, 1, false));
}

// 먼 카메라(간격 4)로 건너뛴 시간은 다음 평가가 한 번에 진행 — 노티파이 순서·평가 프레임 포즈가 매 프레임 갱신과 같다
E_TEST(AnimUpdateRate_SkippedTimeKeepsNotifiesAndPose)
{
	FConsoleVariable* Uro = FConsoleManager::Get().FindVariable("a.URO");
	E_EXPECT_TRUE(Uro != nullptr);
	if (Uro == nullptr)
	{
		return;
	}
	const bool bWasUro = Uro->GetBool();
	Uro->SetBool(true);

	auto Metadata = std::make_shared<FModelMetadata>();
	Metadata->GetOrAddNotifies("Walk") = { MakeNotify("Step_L", 0.1f), MakeNotify("Step_R", 0.8f), MakeNotify("Swing", 0.3f, 0.2f) };

	FScene     Far;
	FScene     Full;
	FTestModel FarModel  = CreateModel(Far, Metadata);
	FTestModel FullModel = CreateModel(Full, Metadata);
	const FEntity Camera = Far.CreateEntity("Camera");
	Far.GetRegistry().Emplace<FCameraComponent>(Camera);
	Far.GetTransform(Camera).Position = FVector3(100000.0f, 0.0f, 0.0f);
	Far.UpdateTransforms();
	Full.UpdateTransforms();

	std::string FarEvents;
	std::string FullEvents;
	uint32      Evaluated = 0;
	uint32      Skipped   = 0;
	const float Step      = 1.0f / 30.0f;
	for (int32 Frame = 0; Frame < 90; ++Frame)
	{
		const float FarBefore = Far.GetTransform(FarModel.Node).Position.X;
		FAnimationSystem::Update(Far, Step);
		FAnimationSystem::Update(Full, Step);
		const FAnimationRuntime& FarRuntime  = Far.GetRegistry().Get<FAnimationComponent>(FarModel.Root).Runtime;
		const FAnimationRuntime& FullRuntime = Full.GetRegistry().Get<FAnimationComponent>(FullModel.Root).Runtime;
		E_EXPECT_EQ(static_cast<uint32>(FarRuntime.UpdateRateInterval), 4u);
		E_EXPECT_EQ(static_cast<uint32>(FullRuntime.UpdateRateInterval), 1u);
		FarEvents += DescribeEvents(FarRuntime.PendingNotifies);
		FullEvents += DescribeEvents(FullRuntime.PendingNotifies);
		if (FarRuntime.UpdateRatePending == 0.0f)
		{
			++Evaluated;
			E_EXPECT_NEAR(Far.GetTransform(FarModel.Node).Position.X, Full.GetTransform(FullModel.Node).Position.X, Tol);
			E_EXPECT_NEAR(FarRuntime.CurrentTime, FullRuntime.CurrentTime, Tol);
		}
		else
		{
			++Skipped;
			E_EXPECT_TRUE(FarRuntime.PendingNotifies.empty());
			E_EXPECT_NEAR(Far.GetTransform(FarModel.Node).Position.X, FarBefore, 0.0f);
		}
	}
	E_EXPECT_TRUE(Evaluated >= 20 && Skipped >= 60);
	// 마지막 평가 이후 아직 진행하지 않은 시간에 걸린 이벤트만 다를 수 있다 → 평가 프레임에 맞춰 남은 시간을 진행시키고 비교
	for (int32 Frame = 0; Frame < 4 && Far.GetRegistry().Get<FAnimationComponent>(FarModel.Root).Runtime.UpdateRatePending != 0.0f; ++Frame)
	{
		FAnimationSystem::Update(Far, 0.0f);
		FarEvents += DescribeEvents(Far.GetRegistry().Get<FAnimationComponent>(FarModel.Root).Runtime.PendingNotifies);
	}
	E_EXPECT_TRUE(FarEvents == FullEvents);
	E_EXPECT_TRUE(FullEvents.find("Step_L Swing.Begin Swing.End Step_R Step_L") != std::string::npos);

	// 끄면 매 프레임
	Uro->SetBool(false);
	FAnimationSystem::Update(Far, Step);
	E_EXPECT_EQ(static_cast<uint32>(Far.GetRegistry().Get<FAnimationComponent>(FarModel.Root).Runtime.UpdateRateInterval), 1u);
	Uro->SetBool(bWasUro);
}

// 병렬 평가(a.ParallelEvaluate)와 순차 평가의 포즈·노티파이가 같다
E_TEST(AnimUpdateRate_ParallelEvaluateMatchesSequential)
{
	FConsoleVariable* Parallel = FConsoleManager::Get().FindVariable("a.ParallelEvaluate");
	E_EXPECT_TRUE(Parallel != nullptr);
	if (Parallel == nullptr)
	{
		return;
	}
	const bool bWasParallel = Parallel->GetBool();
	auto       Metadata     = std::make_shared<FModelMetadata>();
	Metadata->GetOrAddNotifies("Walk") = { MakeNotify("Step_L", 0.1f), MakeNotify("Swing", 0.3f, 0.2f) };

	FScene                  Scenes[2];
	std::vector<FTestModel> Models[2];
	for (int32 Index = 0; Index < 2; ++Index)
	{
		for (int32 Model = 0; Model < 64; ++Model)
		{
			Models[Index].push_back(CreateModel(Scenes[Index], Metadata));
			Scenes[Index].GetRegistry().Get<FAnimationComponent>(Models[Index].back().Root).Speed = 0.5f + 0.03f * static_cast<float>(Model);
		}
	}
	for (int32 Frame = 0; Frame < 20; ++Frame)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			Parallel->SetBool(Index == 0);
			FAnimationSystem::Update(Scenes[Index], 1.0f / 30.0f);
		}
		for (size_t Model = 0; Model < Models[0].size(); ++Model)
		{
			E_EXPECT_NEAR(Scenes[0].GetTransform(Models[0][Model].Node).Position.X, Scenes[1].GetTransform(Models[1][Model].Node).Position.X, 0.0f);
			const auto& A = Scenes[0].GetRegistry().Get<FAnimationComponent>(Models[0][Model].Root).Runtime.PendingNotifies;
			const auto& B = Scenes[1].GetRegistry().Get<FAnimationComponent>(Models[1][Model].Root).Runtime.PendingNotifies;
			E_EXPECT_TRUE(DescribeEvents(A) == DescribeEvents(B) && A.size() == B.size());
		}
	}
	Parallel->SetBool(bWasParallel);
}
