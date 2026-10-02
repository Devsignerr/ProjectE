#include "Core/Testing/TestFramework.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimMontage.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <filesystem>
#include <fstream>

// 몽타주 (Scene/AnimMontage.h, Phase 42-3): 블렌드 인/아웃 시각, 중단, 구간 반복, 슬롯 마스크, 끝 이벤트, 노티파이 규칙

namespace
{
	constexpr float Tol = 1.0e-4f;

	FAnimMontageInstance MakeMontage(float Duration, const FMontagePlayParams& Params)
	{
		FAnimMontageInstance Montage;
		Montage.Clip      = "Clip";
		Montage.ClipIndex = 0;
		Montage.Duration  = Duration;
		Montage.Params    = Params;
		AnimMontageMath::Start(Montage);
		return Montage;
	}
} // namespace

E_TEST(AnimMontage_BlendTiming)
{
	// 1초 클립, 들어오기/나가기 0.2초: smoothstep 들어오기 → 남은 0.2초부터 저절로 빠져 끝에서 0 (완료, 중단 아님)
	FMontagePlayParams Params;
	Params.BlendIn  = 0.2f;
	Params.BlendOut = 0.2f;
	FAnimMontageInstance Montage = MakeMontage(1.0f, Params);
	E_EXPECT_NEAR(Montage.Weight, 0.0f, Tol);
	AnimMontageMath::Advance(Montage, 0.1f);
	E_EXPECT_NEAR(Montage.Weight, 0.5f, Tol); // smoothstep(0.5)
	AnimMontageMath::Advance(Montage, 0.1f);
	E_EXPECT_NEAR(Montage.Weight, 1.0f, Tol);
	AnimMontageMath::Advance(Montage, 0.5f); // 0.7
	E_EXPECT_NEAR(Montage.Weight, 1.0f, Tol);
	E_EXPECT_TRUE(Montage.BlendOutElapsed < 0.0f);
	AnimMontageMath::Advance(Montage, 0.15f); // 0.85: 남은 0.15초 <= 0.2 → 남은 시간 동안 빠진다
	E_EXPECT_TRUE(Montage.BlendOutElapsed >= 0.0f);
	E_EXPECT_NEAR(Montage.BlendOutDuration, 0.15f, 1.0e-4f);
	E_EXPECT_NEAR(Montage.Weight, 1.0f, Tol);
	AnimMontageMath::Advance(Montage, 0.075f);
	E_EXPECT_NEAR(Montage.Weight, 0.5f, 1.0e-3f);
	E_EXPECT_FALSE(Montage.bFinished);
	const FMontageStep Step = AnimMontageMath::Advance(Montage, 0.075f);
	E_EXPECT_TRUE(Montage.bFinished);
	E_EXPECT_FALSE(Montage.bInterrupted);
	E_EXPECT_NEAR(Montage.Weight, 0.0f, Tol);
	E_EXPECT_NEAR(Step.NewTime, 1.0f, 1.0e-4f);
	// 끝난 뒤에는 진행하지 않는다
	E_EXPECT_NEAR(AnimMontageMath::Advance(Montage, 0.1f).Delta, 0.0f, Tol);

	// 나가기 0: 끝 시각에 닿는 프레임에 바로 완료
	Params.BlendIn  = 0.0f;
	Params.BlendOut = 0.0f;
	Montage         = MakeMontage(0.5f, Params);
	E_EXPECT_NEAR(Montage.Weight, 1.0f, Tol);
	AnimMontageMath::Advance(Montage, 0.4f);
	E_EXPECT_FALSE(Montage.bFinished);
	AnimMontageMath::Advance(Montage, 0.2f);
	E_EXPECT_TRUE(Montage.bFinished);
}

E_TEST(AnimMontage_InterruptAndLoop)
{
	// 들어오는 중 중단: 그 순간 가중치(0.5)에서 0.2초 동안 빠진다
	FMontagePlayParams Params;
	Params.BlendIn  = 0.2f;
	Params.BlendOut = 0.4f;
	Params.bLoop    = true;
	FAnimMontageInstance Montage = MakeMontage(1.0f, Params);
	AnimMontageMath::Advance(Montage, 0.1f);
	AnimMontageMath::BeginBlendOut(Montage, 0.2f, true);
	E_EXPECT_TRUE(Montage.bInterrupted);
	AnimMontageMath::Advance(Montage, 0.1f);
	E_EXPECT_NEAR(Montage.Weight, 0.25f, 1.0e-3f);
	// 더 긴 나가기 요청은 무시 (이미 더 빨리 끝난다), 더 짧은 요청은 지금 가중치에서 다시
	AnimMontageMath::BeginBlendOut(Montage, 5.0f, true);
	E_EXPECT_NEAR(Montage.BlendOutDuration, 0.2f, Tol);
	AnimMontageMath::BeginBlendOut(Montage, 0.0f, true);
	E_EXPECT_TRUE(Montage.bFinished);

	// 구간 반복 [0.25, 0.75] × 2배속: 감기면 구간 시작으로. 반복은 저절로 끝나지 않는다
	Params.BlendIn   = 0.0f;
	Params.Speed     = 2.0f;
	Params.StartTime = 0.25f;
	Params.EndTime   = 0.75f;
	Montage          = MakeMontage(1.0f, Params);
	E_EXPECT_NEAR(Montage.Time, 0.25f, Tol);
	AnimMontageMath::Advance(Montage, 0.2f);
	E_EXPECT_NEAR(Montage.Time, 0.65f, Tol);
	const FMontageStep Wrap = AnimMontageMath::Advance(Montage, 0.1f);
	E_EXPECT_TRUE(Wrap.bWrapped);
	E_EXPECT_NEAR(Montage.Time, 0.35f, Tol);
	E_EXPECT_NEAR(Wrap.Delta, 0.2f, Tol);
	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		AnimMontageMath::Advance(Montage, 0.05f);
	}
	E_EXPECT_FALSE(Montage.bFinished);

	// 거꾸로 재생은 구간 끝에서 시작해 시작에서 끝난다
	Params.Speed = -1.0f;
	Params.bLoop = false;
	Montage      = MakeMontage(1.0f, Params);
	E_EXPECT_NEAR(Montage.Time, 0.75f, Tol);
	AnimMontageMath::Advance(Montage, 0.6f);
	E_EXPECT_NEAR(Montage.Time, 0.25f, Tol);
	E_EXPECT_TRUE(Montage.bFinished);
	// EndTime이 클립보다 길면 클립 끝, 시작이 끝보다 크면 끝
	Params.EndTime   = 5.0f;
	Params.StartTime = 3.0f;
	Montage          = MakeMontage(1.0f, Params);
	E_EXPECT_NEAR(AnimMontageMath::GetEndTime(Montage), 1.0f, Tol);
	E_EXPECT_NEAR(AnimMontageMath::GetStartTime(Montage), 1.0f, Tol);
}

// 시스템: 슬롯 마스크(부분), 몸 전체 몽타주의 아래 노티파이 멈춤, 몽타주 노티파이, 끝 이벤트(완료/중단), 그래프 없는 클립 재생 위 몽타주
E_TEST(AnimMontage_SystemSlotsEventsNotifies)
{
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEAnimMontage";
	std::filesystem::create_directories(Directory);
	const std::filesystem::path GraphPath = Directory / L"Montage.eanimgraph";
	{
		std::ofstream File(GraphPath, std::ios::binary | std::ios::trunc);
		File << R"({ "States": [ { "Name": "Base", "Clip": "Move" } ], "Slots": [ { "Name": "Arm", "Mask": [ { "Bone": "ArmNode" } ] }, { "Name": "Whole", "Mask": [] } ] })";
	}
	FAnimGraphLibrary::Get().Invalidate();

	// 노드 0 Body, 1 ArmNode. Move: X 0→100 (1초, 노티파이 Step 0.5), Still: 0, Wave: X 0→-100 (노티파이 Hello 0.25)
	const auto MakeClip = [](const char* Name, float To) {
		FAnimationClip Clip;
		Clip.Name     = Name;
		Clip.Duration = 1.0f;
		for (int32 Node = 0; Node < 2; ++Node)
		{
			FAnimationChannel Channel;
			Channel.Node   = Node;
			Channel.Path   = EAnimationPath::Translation;
			Channel.Times  = { 0.0f, 1.0f };
			Channel.Values = { FVector4(0, 0, 0, 0), FVector4(To, 0, 0, 0) };
			Clip.Channels.push_back(Channel);
		}
		return Clip;
	};
	FScene        Scene;
	const FEntity Character = Scene.CreateEntity("Character");
	const FEntity Root      = Scene.CreateEntity("Model");
	const FEntity Body      = Scene.CreateEntity("Body");
	const FEntity Arm       = Scene.CreateEntity("ArmNode");
	Scene.SetParent(Root, Character);
	Scene.SetParent(Body, Root);
	Scene.SetParent(Arm, Body);
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Runtime.Set          = MakeAnimationSet({ MakeClip("Move", 100.0f), MakeClip("Still", 0.0f), MakeClip("Wave", -100.0f) }, { -1, 0 },
	                                                  std::vector<FNodePose>(2));
	Animation.Runtime.NodeEntities = { Body, Arm };
	auto Metadata                  = std::make_shared<FModelMetadata>();
	Metadata->GetOrAddNotifies("Move").push_back({ "Step", EAnimNotifyKind::Notify, 0.5f, 0.0f });
	Metadata->GetOrAddNotifies("Wave").push_back({ "Hello", EAnimNotifyKind::Notify, 0.25f, 0.0f });
	Animation.Runtime.Metadata = Metadata;
	Scene.GetRegistry().Emplace<FAnimGraphComponent>(Root).Graph = GraphPath.string();
	const auto HasNotify = [&](const char* Name) {
		for (const FAnimNotifyEvent& Event : Animation.Runtime.PendingNotifies)
		{
			if (Event.Name == Name)
			{
				return true;
			}
		}
		return false;
	};

	FAnimationSystem::Update(Scene, 0.0f);
	E_EXPECT_FALSE(FAnimationSystem::PlayMontage(Scene, Character, "Nope", {}));

	// 1) Arm 슬롯 Wave (블렌드 0): Arm만 Wave, Body는 그래프(Move). 조상에서 불러도 된다
	FMontagePlayParams Instant;
	Instant.BlendIn  = 0.0f;
	Instant.BlendOut = 0.0f;
	Instant.Slot     = "Arm";
	E_EXPECT_TRUE(FAnimationSystem::PlayMontage(Scene, Character, "Wave", Instant));
	E_EXPECT_TRUE(FAnimationSystem::IsMontagePlaying(Scene, Character, "Arm"));
	E_EXPECT_FALSE(FAnimationSystem::IsMontagePlaying(Scene, Character, "Whole"));
	FAnimationSystem::Update(Scene, 0.4f);
	E_EXPECT_NEAR(Scene.GetTransform(Body).Position.X, 40.0f, 1.0e-2f);
	E_EXPECT_NEAR(Scene.GetTransform(Arm).Position.X, -40.0f, 1.0e-2f);
	E_EXPECT_TRUE(HasNotify("Hello")); // 몽타주 노티파이
	FAnimationSystem::Update(Scene, 0.2f); // 그래프 0.6: 슬롯 몽타주는 아래 노티파이를 막지 않는다
	E_EXPECT_TRUE(HasNotify("Step"));
	FAnimationSystem::Update(Scene, 0.5f); // 몽타주 끝 (완료)
	E_EXPECT_EQ(Animation.Runtime.PendingMontageEvents.size(), static_cast<size_t>(1));
	if (!Animation.Runtime.PendingMontageEvents.empty())
	{
		const FAnimMontageEvent& Event = Animation.Runtime.PendingMontageEvents[0];
		E_EXPECT_TRUE(Event.Clip == "Wave" && Event.Slot == "Arm" && !Event.bInterrupted && Event.Entity == Root);
	}
	E_EXPECT_FALSE(FAnimationSystem::IsMontagePlaying(Scene, Character, ""));
	FAnimationSystem::Update(Scene, 0.0f);
	E_EXPECT_TRUE(Animation.Runtime.PendingMontageEvents.empty()); // 다음 갱신에서 비움

	// 2) 몸 전체(빈 마스크 슬롯) Still 반복: 둘 다 0, 그래프 Move의 Step 노티파이는 멈춘다. 같은 슬롯에 다시 재생하면 이전 것은 중단
	FMontagePlayParams Whole = Instant;
	Whole.Slot               = "Whole";
	Whole.bLoop              = true;
	E_EXPECT_TRUE(FAnimationSystem::PlayMontage(Scene, Root, "Still", Whole));
	bool bStepWhileCovered = false;
	for (int32 Frame = 0; Frame < 12; ++Frame)
	{
		FAnimationSystem::Update(Scene, 0.1f);
		bStepWhileCovered = bStepWhileCovered || HasNotify("Step");
	}
	E_EXPECT_FALSE(bStepWhileCovered);
	E_EXPECT_NEAR(Scene.GetTransform(Body).Position.X, 0.0f, 1.0e-2f);
	E_EXPECT_NEAR(Scene.GetTransform(Arm).Position.X, 0.0f, 1.0e-2f);
	Whole.BlendIn = 0.2f;
	E_EXPECT_TRUE(FAnimationSystem::PlayMontage(Scene, Root, "Wave", Whole)); // Still은 0.2초 동안 빠진다
	E_EXPECT_EQ(Animation.Runtime.Montages.size(), static_cast<size_t>(2));
	FAnimationSystem::Update(Scene, 0.1f);
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_EQ(Animation.Runtime.PendingMontageEvents.size(), static_cast<size_t>(1));
	if (!Animation.Runtime.PendingMontageEvents.empty())
	{
		E_EXPECT_TRUE(Animation.Runtime.PendingMontageEvents[0].Clip == "Still" && Animation.Runtime.PendingMontageEvents[0].bInterrupted);
	}
	E_EXPECT_TRUE(FAnimationSystem::StopMontage(Scene, Root, "", 0.0f));
	E_EXPECT_FALSE(FAnimationSystem::IsMontagePlaying(Scene, Root, ""));
	FAnimationSystem::Update(Scene, 0.1f);
	E_EXPECT_TRUE(Animation.Runtime.Montages.empty());

	// 3) 그래프 없이 클립 재생 위에도 몽타주 (몸 전체)
	Scene.GetRegistry().Get<FAnimGraphComponent>(Root).Graph.clear();
	Animation.Clip = "Still";
	FAnimationSystem::Update(Scene, 0.0f);
	E_EXPECT_TRUE(FAnimationSystem::PlayMontage(Scene, Root, "Move", Instant)); // Arm 슬롯이지만 그래프가 없으니 몸 전체
	FAnimationSystem::Update(Scene, 0.3f);
	E_EXPECT_NEAR(Scene.GetTransform(Body).Position.X, 30.0f, 1.0e-2f);
	// 정지 중(Playing false)이면 몽타주도 멈춘다
	Animation.bPlaying = false;
	FAnimationSystem::Update(Scene, 0.3f);
	E_EXPECT_NEAR(Scene.GetTransform(Body).Position.X, 30.0f, 1.0e-2f);

	std::error_code ErrorCode;
	std::filesystem::remove_all(Directory, ErrorCode);
}
