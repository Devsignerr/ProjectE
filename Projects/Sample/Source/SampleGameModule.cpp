#include "SampleGameComponents.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/Components.h"
#include "Scene/GameModule.h"
#include "Scene/Scene.h"

#include <cmath>

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
	}

	void OnBeginPlay(FScene& Scene) override
	{
		// 부유 기준 높이는 플레이 시작 위치
		Scene.GetRegistry().View<FTransformComponent, FHoverComponent>().Each(
			[](FEntity, FTransformComponent&, FHoverComponent& Hover) { Hover.bInitialized = false; });
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
	}
};

E_IMPLEMENT_GAME_MODULE(FSampleGameModule)
