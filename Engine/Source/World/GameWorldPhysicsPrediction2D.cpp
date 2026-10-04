#include "World/GameWorld.h"

#include "Core/Log.h"
#include "Core/Settings/ProjectSettings.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Physics/Physics2DSystem.h"
#include "Scene/Scene.h"
#include "World/PhysicsPredictionTuning.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

// 2D 물리 예측 (네트워크 클라이언트) — 3D 물리 예측(GameWorldPhysicsPrediction.cpp 머리 주석이 기준)과 같은 규칙·같은 상수
// (World/PhysicsPredictionTuning.h)·같은 시계(PredictionClock/PredictionTimeOffset — ack 무브 시계 ↔ 스냅샷 서버 시각)를 평면 상태로 옮긴 것이다.
//   대상 (TickPhysicsPrediction2D, 3D와 같은 자리 — 스크립트 뒤·캐릭터 이동 전): 루트 엔티티이고 NetId + 동적 RigidBody2D + 2D 바디가 있는
//     복제 엔티티(이동기 엔티티 제외) 중 로컬 예측 2D 캐릭터(IsPredicted — FCharacterMovement2DComponent) 중심에서 Network.PhysicsPredictionRadius
//     안이거나 캐릭터가 닿은 것(대리 캡슐 + 2cm 겹침 — FCharacterMovement2DSystem::GetCharacterContacts). 반경 안이라도 서버 속도가
//     MaxEnterSpeed 이상이면 닿을 때만. 대상이 되면 2D 키네마틱 오버라이드가 풀려(FGameWorld::BeginPlay — IsPhysics2DSimulatedLocally)
//     바디가 동적으로 다시 만들어지고, 화면 위치(보간 값) + 서버 속도(스냅샷 두 개의 차분)에서 시작한다.
//   수렴: 새 스냅샷마다 서버 상태(Ts) - 로컬 기록(Ts + 오프셋)의 위치·각·속도·각속도 오차, 기록이 없으면 서버 상태를 현재로 외삽해 비교.
//     매 프레임 1 - exp(-dt / τ)만큼 바디(FPhysics2DSystem::CorrectBody — 렌더 보간 직전 상태도 함께)와 기록에 더한다. 위치 차이가 SnapDistance보다
//     크면 바로 옮긴다 (각은 스냅하지 않는다).
//   해제: ReleaseDelaySeconds 동안 근처/접촉 없음 + 로컬·서버 속도 RestSpeed 미만 + 위치 오차 작음 → 키네마틱으로 돌아가 BlendOutSeconds 동안
//     화면을 스냅샷 보간 위치로 옮긴 뒤 보간에 넘긴다. 다시 가까워지면 재진입.
//   서버 권위: 서버 코드·메시지는 그대로 (스냅샷 차분 속도). 관찰자 화면은 바뀌지 않는다.
//   2D 캐릭터 재조정과의 관계 (전체 롤백 없음, GameWorldCharacter2D.cpp ReceiveCharacterAck2D): 2D 이동기는 동적 바디를 옆으로 밀지 않고
//     (밀기는 2D 스텝의 키네마틱 대리 바디) 위에 설 때만 막히므로, 다시 적용에서 예측 바디가 영향을 주는 것은 "그 위에 섰는가"다.
//     3D처럼 두 번 다시 적용해 지금 예측에 가까운 쪽을 쓴다: ① 바디를 지금 자리에 둔 채 ② 무브마다 그 무브를 처음 시뮬레이션할 때의
//     기록 위치로 잠시 옮겨(PoseBodiesForReplay2D — 끝나면 RestoreBodiesAfterReplay2D). 예측 바디는 재시뮬레이션하지 않는다.
//   옵션: 3D와 같은 Network.bPhysicsPrediction(+ 캐릭터 예측) && SetReplicationClient. 끄면 키네마틱 보간.
//   측정(--net-physics-stats)의 바디 항목은 3D 바디만 센다 (2D는 스냅 횟수와 예측 중 개수만 합친다).

namespace
{
	using namespace PhysicsPredictionTuning;

	// 최단 각 차이 (To - From, -π ~ π)
	float WrapAngle(float Radians)
	{
		float Wrapped = std::fmod(Radians, FMath::TwoPi);
		if (Wrapped > FMath::Pi)
		{
			Wrapped -= FMath::TwoPi;
		}
		else if (Wrapped < -FMath::Pi)
		{
			Wrapped += FMath::TwoPi;
		}
		return Wrapped;
	}

	float PlaneAngle(const FQuat& Rotation) { return Physics2DMath::AngleFromRotation(Rotation); }

	template <typename TSample>
	bool SampleHistory(const std::deque<TSample>& History, float Time, FVector2& OutPosition, float& OutAngle)
	{
		if (History.empty() || Time < History.front().Time || Time > History.back().Time)
		{
			return false;
		}
		const auto Upper = std::lower_bound(History.begin(), History.end(), Time, [](const TSample& Sample, float Value) { return Sample.Time < Value; });
		if (Upper == History.begin())
		{
			OutPosition = Upper->Position;
			OutAngle    = Upper->Angle;
			return true;
		}
		const TSample& From  = *(Upper - 1);
		const TSample& To    = *Upper;
		const float    Span  = To.Time - From.Time;
		const float    Alpha = Span > 0.0f ? (Time - From.Time) / Span : 1.0f;
		OutPosition          = From.Position + (To.Position - From.Position) * Alpha;
		OutAngle             = From.Angle + WrapAngle(To.Angle - From.Angle) * Alpha;
		return true;
	}

	// 마지막 스냅샷 두 개의 차분 (평면 속도, 각속도). 반환: 차분 구간 (0 = 없음)
	float EstimateServerMotion2D(const std::deque<FReplicationClient::FTransformSample>& Samples, FVector2& OutVelocity, float& OutAngular)
	{
		OutVelocity = FVector2();
		OutAngular  = 0.0f;
		if (Samples.size() < 2)
		{
			return 0.0f;
		}
		const FReplicationClient::FTransformSample& Newest = Samples.back();
		const FReplicationClient::FTransformSample& Prev   = Samples[Samples.size() - 2];
		const float                                 Span   = Newest.ServerTime - Prev.ServerTime;
		if (Span <= 0.0f || Span > MaxVelocitySampleGap)
		{
			return 0.0f;
		}
		OutVelocity = (Physics2DMath::ToPlane(Newest.Position) - Physics2DMath::ToPlane(Prev.Position)) * (1.0f / Span);
		OutAngular  = WrapAngle(PlaneAngle(Newest.Rotation) - PlaneAngle(Prev.Rotation)) / Span;
		return Span;
	}
} // namespace

bool FGameWorld::IsPhysics2DSimulatedLocally(FEntity Entity) const
{
	const auto Found = PredictedBodies2D.find(Entity);
	return Found != PredictedBodies2D.end() && !Found->second.bBlendingOut;
}

bool FGameWorld::IsPhysicsPrediction2DEnabled() const
{
	return IsPhysicsPredictionTimingEnabled() && Physics2D->IsActive();
}

void FGameWorld::TickPhysicsPrediction2D(float DeltaSeconds)
{
	if (Mode != ENetMode::Client || Scene == nullptr || !Physics2D->IsActive())
	{
		return;
	}
	FPhysics2DSystem& Physics  = *Physics2D;
	FRegistry&        Registry = Scene->GetRegistry();
	const bool        bEnabled = IsPhysicsPrediction2DEnabled();

	// 사라진 엔티티
	for (auto It = PredictedBodies2D.begin(); It != PredictedBodies2D.end();)
	{
		It = Registry.IsValid(It->first) && Registry.Has<FNetIdComponent>(It->first) ? std::next(It) : PredictedBodies2D.erase(It);
	}

	// 대상 선정: 예측 2D 캐릭터 근처 또는 접촉
	std::vector<FEntity> Entered;
	if (bEnabled && bPredictionTimingValid)
	{
		std::unordered_set<FEntity> Touching;
		std::vector<FEntity>        Contacts;
		std::vector<FVector2>       CharacterPositions;
		Registry.View<FCharacterMovement2DComponent>().Each([&](FEntity Character, FCharacterMovement2DComponent&) {
			if (!IsPredicted(Character) || !Characters2D->HasCharacter(Character))
			{
				return;
			}
			Characters2D->GetCharacterContacts(Character, Contacts);
			Touching.insert(Contacts.begin(), Contacts.end());
			CharacterPositions.push_back(Characters2D->GetState(Character).Position);
		});
		const float Radius = std::max(FProjectSettings::Get().Network.PhysicsPredictionRadius, 0.0f);
		Registry.View<FNetIdComponent, FRigidBody2DComponent>().Each([&](FEntity Entity, FNetIdComponent&, FRigidBody2DComponent& RigidBody) {
			if (RigidBody.BodyType != EBodyType2D::Dynamic || !RigidBody.bEnabled || Registry.Has<FCharacterMovement2DComponent>(Entity) ||
			    Scene->GetParent(Entity).IsValid() || Scene->IsSocketAttached(Entity) || !Physics.HasBody(Entity))
			{
				return;
			}
			const auto     Found    = PredictedBodies2D.find(Entity);
			const float    Range    = Found != PredictedBodies2D.end() ? Radius * ReleaseRadiusScale : Radius;
			const FVector2 Position = Physics2DMath::ToPlane(Scene->GetTransform(Entity).Position);
			bool           bNear    = Touching.contains(Entity);
			for (const FVector2& CharacterPosition : CharacterPositions)
			{
				bNear = bNear || (Position - CharacterPosition).LengthSquared() < Range * Range;
			}
			if (Found == PredictedBodies2D.end())
			{
				// 빠른 물체는 닿았을 때만 (반경 안을 스쳐 가는 서버 발사체를 현재로 외삽하면 크게 틀린다)
				if (bNear && !Touching.contains(Entity))
				{
					FVector2 Velocity;
					float    Angular = 0.0f;
					if (const std::deque<FReplicationClient::FTransformSample>* Samples = Replication->FindTransformSamples(Entity))
					{
						EstimateServerMotion2D(*Samples, Velocity, Angular);
					}
					bNear = Velocity.Length() < MaxEnterSpeed;
				}
				if (bNear)
				{
					Entered.push_back(Entity);
				}
				return;
			}
			FPredictedBody2D& Body = Found->second;
			Body.IdleSeconds       = bNear ? 0.0f : Body.IdleSeconds + DeltaSeconds;
			if (Body.bBlendingOut && bNear)
			{
				Entered.push_back(Entity); // 해제 중에 다시 가까워졌다
			}
		});
	}

	// 진입: 동적으로 다시 만들고 화면 위치 + 서버 속도에서 시작 (현재와의 차이는 수렴이 곧바로 줄인다)
	if (!Entered.empty())
	{
		for (const FEntity Entity : Entered)
		{
			PredictedBodies2D[Entity] = FPredictedBody2D();
		}
		Physics.SyncBodies(*Scene);
		for (const FEntity Entity : Entered)
		{
			const FTransformComponent& Transform = Scene->GetTransform(Entity);
			FPhysics2DBodyMotion       Motion;
			Motion.Position = Physics2DMath::ToPlane(Transform.Position);
			Motion.Angle    = PlaneAngle(Transform.Rotation);
			if (const std::deque<FReplicationClient::FTransformSample>* Samples = Replication->FindTransformSamples(Entity))
			{
				EstimateServerMotion2D(*Samples, Motion.LinearVelocity, Motion.AngularVelocity);
			}
			Physics.SetBodyMotion(Entity, Motion);
			E_LOG(LogNet, Verbose, "2D 물리 예측 시작: NetId {}", NetReplication::GetNetId(*Scene, Entity));
		}
	}

	// 수렴 / 해제 / 해제 블렌드
	for (auto It = PredictedBodies2D.begin(); It != PredictedBodies2D.end();)
	{
		const FEntity     Entity = It->first;
		FPredictedBody2D& Body   = It->second;
		if (!Body.bBlendingOut)
		{
			if (!bEnabled)
			{
				BeginBodyBlendOut2D(Entity, Body); // 예측을 끄면 보간으로 돌아간다
			}
			else
			{
				ProcessBodySnapshot2D(Entity, Body);
				ApplyBodyCorrection2D(Entity, Body, DeltaSeconds);
				FPhysics2DBodyMotion Motion;
				if (Body.IdleSeconds > ReleaseDelaySeconds && Physics.GetBodyMotion(Entity, Motion) && Motion.LinearVelocity.Length() < RestSpeed &&
				    Body.ServerVelocity.Length() < RestSpeed && Body.PositionError.Length() < ReleaseErrorDistance)
				{
					BeginBodyBlendOut2D(Entity, Body);
				}
			}
		}
		if (Body.bBlendingOut)
		{
			// 키네마틱(다음 2D 물리 갱신에 다시 만들어진다)으로 화면 위치를 보간 값까지 옮긴다
			Body.BlendSeconds += DeltaSeconds;
			FVector3 Target         = Body.BlendFromPosition;
			FQuat    TargetRotation = Body.BlendFromRotation;
			Replication->SampleTransform(Entity, Target, TargetRotation);
			const float          Alpha     = SmoothStep(Body.BlendSeconds / BlendOutSeconds);
			FTransformComponent& Transform = Scene->GetTransform(Entity);
			Transform.Position             = FVector3::Lerp(Body.BlendFromPosition, Target, Alpha);
			Transform.Rotation             = FQuat::Slerp(Body.BlendFromRotation, TargetRotation, Alpha);
			if (Body.BlendSeconds >= BlendOutSeconds)
			{
				E_LOG(LogNet, Verbose, "2D 물리 예측 해제: NetId {}", NetReplication::GetNetId(*Scene, Entity));
				It = PredictedBodies2D.erase(It); // 이제 스냅샷 보간이 쓴다
				continue;
			}
		}
		++It;
	}
}

void FGameWorld::BeginBodyBlendOut2D(FEntity Entity, FPredictedBody2D& Body)
{
	Body.bBlendingOut      = true;
	Body.BlendSeconds      = 0.0f;
	Body.BlendFromPosition = Scene->GetTransform(Entity).Position;
	Body.BlendFromRotation = Scene->GetTransform(Entity).Rotation;
	Body.History.clear();
	Body.PositionError     = FVector2();
	Body.VelocityError     = FVector2();
	Body.AngleError        = 0.0f;
	Body.AngularSpeedError = 0.0f;
}

void FGameWorld::ProcessBodySnapshot2D(FEntity Entity, FPredictedBody2D& Body)
{
	const std::deque<FReplicationClient::FTransformSample>* Samples = Replication->FindTransformSamples(Entity);
	if (Samples == nullptr || Samples->empty() || Samples->back().ServerTime <= Body.LastSampleTime)
	{
		return; // 새 스냅샷 없음
	}
	FPhysics2DBodyMotion Local;
	if (!Physics2D->GetBodyMotion(Entity, Local))
	{
		return;
	}
	const FReplicationClient::FTransformSample& Newest = Samples->back();
	Body.LastSampleTime                                = Newest.ServerTime;
	const FVector2 ServerPosition = Physics2DMath::ToPlane(Newest.Position);
	const float    ServerAngle    = PlaneAngle(Newest.Rotation);
	float          ServerAngular  = 0.0f;
	const float    Span           = EstimateServerMotion2D(*Samples, Body.ServerVelocity, ServerAngular);
	const float    LocalTime      = Newest.ServerTime + PredictionTimeOffset; // 이 스냅샷 상태에 해당하는 로컬 시각
	const float    Ahead          = std::max(LastRecordTime - LocalTime, 0.0f);

	FVector2 HistoryPosition;
	float    HistoryAngle = 0.0f;
	if (SampleHistory(Body.History, LocalTime, HistoryPosition, HistoryAngle))
	{
		// 같은 시점끼리 비교 (같은 시뮬레이션이면 미는 중에도 0)
		Body.PositionError = ServerPosition - HistoryPosition;
		Body.AngleError    = WrapAngle(ServerAngle - HistoryAngle);
		FVector2 EarlierPosition;
		float    EarlierAngle = 0.0f;
		if (Span > 0.0f && SampleHistory(Body.History, LocalTime - Span, EarlierPosition, EarlierAngle))
		{
			Body.VelocityError     = Body.ServerVelocity - (HistoryPosition - EarlierPosition) * (1.0f / Span);
			Body.AngularSpeedError = ServerAngular - WrapAngle(HistoryAngle - EarlierAngle) / Span;
		}
		else
		{
			Body.VelocityError     = Body.ServerVelocity - Local.LinearVelocity;
			Body.AngularSpeedError = ServerAngular - Local.AngularVelocity;
		}
	}
	else
	{
		// 기록이 그 시각까지 없다 (막 진입): 서버 상태를 현재로 외삽해 지금 상태와 비교
		Body.PositionError     = ServerPosition + Body.ServerVelocity * Ahead - Local.Position;
		Body.AngleError        = WrapAngle(ServerAngle - Local.Angle);
		Body.VelocityError     = Body.ServerVelocity - Local.LinearVelocity;
		Body.AngularSpeedError = ServerAngular - Local.AngularVelocity;
	}

	// 위치만 스냅한다 (각은 부드럽게만)
	if (Body.PositionError.Length() > SnapDistance)
	{
		E_LOG(LogNet, Verbose, "2D 물리 예측 스냅: NetId {} {:.1f}cm", NetReplication::GetNetId(*Scene, Entity), Body.PositionError.Length());
		++PredictionStats.Snaps;
		FPhysics2DBodyMotion Snapped;
		Snapped.Position        = ServerPosition + Body.ServerVelocity * Ahead;
		Snapped.Angle           = ServerAngle;
		Snapped.LinearVelocity  = Body.ServerVelocity;
		Snapped.AngularVelocity = ServerAngular;
		Physics2D->SetBodyMotion(Entity, Snapped);
		Body.History.clear();
		Body.PositionError     = FVector2();
		Body.VelocityError     = FVector2();
		Body.AngleError        = 0.0f;
		Body.AngularSpeedError = 0.0f;
	}
}

void FGameWorld::ApplyBodyCorrection2D(FEntity Entity, FPredictedBody2D& Body, float DeltaSeconds)
{
	if (DeltaSeconds <= 0.0f)
	{
		return;
	}
	const float    PositionAlpha = 1.0f - std::exp(-DeltaSeconds / PositionCorrectionSeconds);
	const float    VelocityAlpha = 1.0f - std::exp(-DeltaSeconds / VelocityCorrectionSeconds);
	const float    RotationAlpha = 1.0f - std::exp(-DeltaSeconds / RotationCorrectionSeconds);
	const FVector2 DeltaPosition = Body.PositionError * PositionAlpha;
	const FVector2 DeltaVelocity = Body.VelocityError * VelocityAlpha;
	const float    DeltaAngle    = Body.AngleError * RotationAlpha;
	const float    DeltaAngular  = Body.AngularSpeedError * VelocityAlpha;
	Body.PositionError           = Body.PositionError - DeltaPosition;
	Body.VelocityError           = Body.VelocityError - DeltaVelocity;
	Body.AngleError -= DeltaAngle;
	Body.AngularSpeedError -= DeltaAngular;
	if (DeltaPosition.LengthSquared() < 1.0e-8f && DeltaVelocity.LengthSquared() < 1.0e-8f && std::abs(DeltaAngle) < 1.0e-7f &&
	    std::abs(DeltaAngular) < 1.0e-6f)
	{
		return;
	}
	Physics2D->CorrectBody(Entity, DeltaPosition, DeltaAngle, DeltaVelocity, DeltaAngular);
	// 기록에도 같은 보정 (속도 보정은 "처음부터 그 속도로 지금 상태에 왔다"로 과거를 옮긴다 — 3D와 같다)
	for (FBodyHistorySample2D& Sample : Body.History)
	{
		const float Before = Sample.Time - LastRecordTime; // 0 이하
		Sample.Position    = Sample.Position + DeltaPosition + DeltaVelocity * Before;
		Sample.Angle       = Sample.Angle + DeltaAngle + DeltaAngular * Before;
	}
}

void FGameWorld::PoseBodiesForReplay2D(float MoveTime)
{
	for (const auto& [Entity, Body] : PredictedBodies2D)
	{
		if (Body.bBlendingOut || Body.History.empty())
		{
			continue;
		}
		// 그 무브가 시뮬레이션될 때 바디가 있던 곳 = 그 프레임 전 기록 (기록은 프레임 끝 2D 물리 스텝 결과). 기록보다 오래된 무브는 가장 오래된 기록
		const auto After = std::lower_bound(Body.History.begin(), Body.History.end(), MoveTime,
		                                    [](const FBodyHistorySample2D& Sample, float Value) { return Sample.Time < Value; });
		const FBodyHistorySample2D& Sample = After == Body.History.begin() ? Body.History.front() : *(After - 1);
		Physics2D->PoseBody(Entity, Sample.Position, Sample.Angle);
	}
}

void FGameWorld::RestoreBodiesAfterReplay2D()
{
	for (const auto& [Entity, Body] : PredictedBodies2D)
	{
		if (!Body.bBlendingOut)
		{
			Physics2D->RestoreBodyPose(Entity);
		}
	}
}

void FGameWorld::RecordPhysicsPrediction2D()
{
	if (!Physics2D->IsActive())
	{
		return;
	}
	for (auto& [Entity, Body] : PredictedBodies2D)
	{
		FPhysics2DBodyMotion Motion;
		if (Body.bBlendingOut || !Physics2D->GetBodyMotion(Entity, Motion))
		{
			continue;
		}
		Body.History.push_back({ PredictionClock, Motion.Position, Motion.Angle });
		while (!Body.History.empty() && Body.History.front().Time < PredictionClock - HistorySeconds)
		{
			Body.History.pop_front();
		}
	}
}
