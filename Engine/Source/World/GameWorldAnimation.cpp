#include "World/GameWorld.h"

#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimIK.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <vector>

// 캐릭터 이동 → 애니메이션 그래프 파라미터 (FAnimGraphComponent::bUseCharacterMovement)
//   대상 캐릭터 = 그래프 엔티티 자신 또는 가장 가까운 조상의 FCharacterMovementComponent.
//   Speed = 수평 속도, VerticalSpeed = 수직 속도 (cm/s), Grounded = 캡슐이 바닥에 닿음.
//   속도 출처: 이 프로세스가 캐릭터를 시뮬레이션하면(서버/Standalone 전부, 예측하는 소유 클라이언트) 캐릭터 이동 상태 그대로,
//     아니면(클라이언트의 다른 플레이어, 예측을 끈 내 캐릭터) 복제 보간된 월드 위치의 변화를 짧게 평활한 값.
//     → 애니메이션 상태는 복제하지 않고, 모든 클라이언트가 같은 입력(복제된 움직임)으로 같은 파라미터를 만든다.
//   바닥: 어디서나 Jolt 캡슐 접촉 (다른 플레이어 캡슐도 FollowTransform이 복제 위치에 맞추고 접촉을 갱신한다)

namespace
{
	constexpr float RemoteVelocitySmoothingSeconds = 0.1f;    // 위치 변화 속도의 평활 시간 (스냅샷 간격 계단 제거)
	constexpr float TeleportSpeed                  = 5000.0f; // cm/s, 이보다 빠른 위치 변화는 순간이동(리스폰)으로 보고 0으로

	FEntity FindMovementOwner(const FScene& Scene, FEntity Entity)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		for (FEntity Current = Entity; Registry.IsValid(Current); Current = Scene.GetParent(Current))
		{
			if (Registry.Has<FCharacterMovementComponent>(Current))
			{
				return Current;
			}
		}
		return NullEntity;
	}
} // namespace

void FGameWorld::UpdateCharacterAnimParams(float DeltaSeconds)
{
	if (Scene == nullptr)
	{
		return;
	}
	std::vector<std::pair<FEntity, FAnimGraphComponent*>> Graphs;
	Scene->GetRegistry().View<FAnimGraphComponent>().Each([&](FEntity Entity, FAnimGraphComponent& Graph) {
		if (Graph.bUseCharacterMovement)
		{
			Graphs.emplace_back(Entity, &Graph);
		}
	});
	FPhysicsSystem* Physics = Systems.Physics;
	for (const auto& [Entity, Graph] : Graphs)
	{
		const FEntity Character = FindMovementOwner(*Scene, Entity);
		if (!Character.IsValid())
		{
			continue;
		}
		FAnimGraphRuntime& Runtime    = Graph->Runtime;
		const bool         bHasSim    = Physics != nullptr && Physics->HasCharacter(Character);
		const bool         bSimulated = bHasSim && (Mode != ENetMode::Client || IsPredicted(Character));
		const FVector3     Position   = Scene->GetTransform(Character).GetWorldPosition();

		FVector3 Velocity;
		if (bSimulated)
		{
			Velocity                         = Physics->GetCharacterState(Character).Velocity;
			Runtime.SmoothedMovementVelocity = Velocity;
		}
		else if (Runtime.bHasMovementSample && DeltaSeconds > FMath::SmallNumber)
		{
			FVector3 Raw = (Position - Runtime.PreviousMovementPosition) * (1.0f / DeltaSeconds);
			if (Raw.Length() > TeleportSpeed)
			{
				Raw = FVector3();
			}
			const float Alpha                = 1.0f - std::exp(-DeltaSeconds / RemoteVelocitySmoothingSeconds);
			Runtime.SmoothedMovementVelocity = Runtime.SmoothedMovementVelocity + (Raw - Runtime.SmoothedMovementVelocity) * Alpha;
			Velocity                         = Runtime.SmoothedMovementVelocity;
		}
		Runtime.PreviousMovementPosition = Position;
		Runtime.bHasMovementSample       = true;

		const bool bGrounded = bHasSim ? Physics->IsGrounded(Character) : true;
		Runtime.Parameters.Set("Speed", std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y));
		Runtime.Parameters.Set("VerticalSpeed", Velocity.Z);
		Runtime.Parameters.Set("Grounded", bGrounded ? 1.0f : 0.0f);
	}
}

// 발 IK 바닥 탐색 (규칙은 Scene/AnimIK.h): 기존 FPhysicsSystem::Raycast만 쓴다 (새 물리 질의 없음)
//   발마다 직전 애니메이션의 IK 전 발 위치(모델 공간) → 모델 루트 월드 행렬로 월드 → 위 TraceUp에서 아래로 TraceUp + TraceDown.
//   자기 몸(모델 루트의 조상/자손 — 강체 캡슐, 래그돌 바디)에 맞으면 맞은 점 1cm 아래에서 다시 (최대 3번)
void FGameWorld::UpdateFootIkProbes()
{
	FPhysicsSystem* Physics = Systems.Physics;
	if (Scene == nullptr || Physics == nullptr)
	{
		return;
	}
	FRegistry& Registry = Scene->GetRegistry();
	const auto IsSelf   = [&](FEntity ModelRoot, FEntity Hit) {
		for (FEntity Current = ModelRoot; Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			if (Current == Hit)
			{
				return true; // 조상 (캐릭터 루트 등)
			}
		}
		for (FEntity Current = Hit; Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			if (Current == ModelRoot)
			{
				return true; // 자손
			}
		}
		return false;
	};
	Registry.View<FFootIkComponent>().Each([&](FEntity Entity, FFootIkComponent& FootIk) {
		const FAnimationComponent* Animation = Registry.TryGet<FAnimationComponent>(Entity);
		if (!FootIk.bEnabled || Animation == nullptr || !Animation->Runtime.Set || Animation->Runtime.bPhysicsPose)
		{
			return;
		}
		const FMatrix4x4& RootWorld = Scene->GetTransform(Entity).WorldMatrix;
		const float       Distance  = FMath::Max(FootIk.TraceUp + FootIk.TraceDown, 1.0f);
		for (FFootIkFoot& Foot : FootIk.Runtime.Feet)
		{
			Foot.bHit = false;
			if (!Foot.bHasProbePosition)
			{
				continue;
			}
			FVector3 Origin = RootWorld.TransformPosition(Foot.ProbeModelPosition) + FVector3::UpVector * FootIk.TraceUp;
			float    Left   = Distance;
			for (int32 Try = 0; Try < 3 && Left > 0.0f; ++Try)
			{
				FPhysicsHit Hit;
				if (!Physics->Raycast(Origin, -FVector3::UpVector, Left, Hit))
				{
					break;
				}
				if (!IsSelf(Entity, Hit.Entity))
				{
					Foot.bHit      = true;
					Foot.HitPoint  = Hit.Position;
					Foot.HitNormal = Hit.Normal.GetNormalized().IsNearlyZero() ? FVector3::UpVector : Hit.Normal.GetNormalized();
					break;
				}
				Left -= Hit.Distance + 1.0f;
				Origin = Hit.Position - FVector3::UpVector * 1.0f;
			}
		}
		FootIk.Runtime.ProbeAge = 0.0f;
	});
}
