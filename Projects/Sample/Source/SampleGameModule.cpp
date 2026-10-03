#include "SampleGameComponents.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/AnimNotify.h"
#include "Scene/Components.h"
#include "Scene/GameModule.h"
#include "Scene/Scene.h"

#include <cmath>

E_DEFINE_LOG_CATEGORY(LogSampleGame, Log)

// Sample 게임 모듈: C++ 게임 시스템 예시. 매 프레임 대량 순회는 스크립트가 아닌 여기(C++ 시스템)에 둔다.
class FSampleGameModule final : public IGameModule
{
public:
	void OnLoad() override
	{
		FTypeRegistry& Registry = FTypeRegistry::Get();
		Registry.RegisterType<FSpinnerComponent>("SpinnerComponent", "회전 (게임)")
			.Property(&FSpinnerComponent::DegreesPerSecond, "DegreesPerSecond", "초당 각도").Range(-720.0f, 720.0f, 1.0f)
			.Property(&FSpinnerComponent::Axis, "Axis", "회전축")
			.AsComponent();
		Registry.RegisterType<FHoverComponent>("HoverComponent", "부유 (게임)")
			.Property(&FHoverComponent::Amplitude, "Amplitude", "진폭 (cm)").Range(0.0f, 1000.0f, 1.0f)
			.Property(&FHoverComponent::Frequency, "Frequency", "주파수 (Hz)").Range(0.0f, 10.0f, 0.01f)
			.AsComponent();
		Registry.RegisterType<FStressWalkerComponent>("StressWalkerComponent", "궤도 이동 (스트레스)")
			.Property(&FStressWalkerComponent::Center, "Center", "궤도 중심")
			.Property(&FStressWalkerComponent::Speed, "Speed", "속도 (cm/s)").Range(-2000.0f, 2000.0f, 1.0f)
			.Property(&FStressWalkerComponent::YawOffset, "YawOffset", "모델 앞 보정 (도)").Range(-180.0f, 180.0f, 1.0f)
			.AsComponent();
	}

	void OnBeginPlay(FScene& Scene) override
	{
		// 부유 기준 높이는 플레이 시작 위치
		Scene.GetRegistry().View<FTransformComponent, FHoverComponent>().Each(
			[](FEntity, FTransformComponent&, FHoverComponent& Hover) { Hover.bInitialized = false; });
		Scene.GetRegistry().View<FTransformComponent, FStressWalkerComponent>().Each(
			[](FEntity, FTransformComponent&, FStressWalkerComponent& Walker) { Walker.bInitialized = false; });
	}

	// 애니메이션 노티파이 (C++ 수신 예시). 매 프레임 오는 스테이트 Tick은 로그에서 뺀다
	void OnAnimNotify(FScene& Scene, const FAnimNotifyEvent& Event) override
	{
		(void)Scene;
		if (Event.Type == EAnimNotifyEventType::Notify)
		{
			E_LOG(LogSampleGame, Display, "[C++] 노티파이 {} (클립 {})", Event.Name, Event.Clip);
		}
	}

	void OnUpdate(FScene& Scene, float DeltaSeconds) override
	{
		FRegistry& Registry = Scene.GetRegistry();
		Registry.View<FTransformComponent, FSpinnerComponent>().Each(
			[DeltaSeconds](FEntity, FTransformComponent& Transform, FSpinnerComponent& Spinner) {
				const FVector3 Axis = Spinner.Axis.GetNormalized();
				if (Axis.IsNearlyZero())
				{
					return;
				}
				const FQuat Delta  = FQuat::FromAxisAngle(Axis, FMath::DegreesToRadians(Spinner.DegreesPerSecond * DeltaSeconds));
				Transform.Rotation = (Delta * Transform.Rotation).GetNormalized();
			});

		Registry.View<FTransformComponent, FHoverComponent>().Each(
			[DeltaSeconds](FEntity, FTransformComponent& Transform, FHoverComponent& Hover) {
				if (!Hover.bInitialized)
				{
					Hover.BaseHeight   = Transform.Position.Z;
					Hover.Elapsed      = 0.0f;
					Hover.bInitialized = true;
				}
				Hover.Elapsed += DeltaSeconds;
				Transform.Position.Z = Hover.BaseHeight + Hover.Amplitude * std::sin(FMath::TwoPi * Hover.Frequency * Hover.Elapsed);
			});

		// 궤도 이동: 시작 위치에서 반지름·각도를 정하고 속도/반지름만큼 돈다 (루트 엔티티 전제 — 로컬 = 월드)
		Registry.View<FTransformComponent, FStressWalkerComponent>().Each(
			[DeltaSeconds](FEntity, FTransformComponent& Transform, FStressWalkerComponent& Walker) {
				const float DX = Transform.Position.X - Walker.Center.X;
				const float DY = Transform.Position.Y - Walker.Center.Y;
				if (!Walker.bInitialized)
				{
					Walker.Radius       = std::sqrt(DX * DX + DY * DY);
					Walker.Angle        = std::atan2(DY, DX);
					Walker.bInitialized = true;
				}
				if (Walker.Radius < 1.0f)
				{
					return;
				}
				Walker.Angle += Walker.Speed / Walker.Radius * DeltaSeconds;
				Transform.Position.X = Walker.Center.X + Walker.Radius * std::cos(Walker.Angle);
				Transform.Position.Y = Walker.Center.Y + Walker.Radius * std::sin(Walker.Angle);
				// 진행 방향 = 접선 (반시계면 +90도, 시계면 -90도)
				const float HeadingDegrees = FMath::RadiansToDegrees(Walker.Angle) + (Walker.Speed >= 0.0f ? 90.0f : -90.0f);
				Transform.Rotation = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(HeadingDegrees + Walker.YawOffset));
			});
	}
};

E_IMPLEMENT_GAME_MODULE(FSampleGameModule)
