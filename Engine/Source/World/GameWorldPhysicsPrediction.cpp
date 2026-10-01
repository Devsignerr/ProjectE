#include "World/GameWorld.h"

#include "Core/Log.h"
#include "Core/Settings/ProjectSettings.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationTypes.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

// 물리 예측 (네트워크 클라이언트, 언리얼 Networked Physics의 Predictive Interpolation과 같은 방식)
//
//   문제: 예측 캐릭터는 "현재"(서버보다 왕복 지연만큼 앞)에 있는데 복제 동적 바디는 키네마틱으로 스냅샷 보간(편도 지연 + 0.1초 과거)을 따른다.
//     클라이언트에선 과거 위치의 밀리지 않는 상자에 막히고 서버에선 밀리므로 ack마다 캐릭터 보정이 생기고, 상자는 왕복 + 0.1초 뒤에야 움직인다.
//   대상 선정 (TickPhysicsPrediction, 스크립트 뒤·캐릭터 이동 전): 루트 엔티티이고 NetId + 동적 RigidBody + 콜라이더(바디)가 있는 복제 엔티티 중
//     로컬 예측 캐릭터(IsPredicted) 중심에서 PhysicsPredictionRadius 안에 중심이 있거나 캐릭터가 닿은 것.
//     반경 안이라도 서버 속도가 MaxEnterSpeed 이상이면(서버가 쏜 공처럼 내가 영향을 주지 않는 빠른 물체) 닿을 때만 — 현재로 외삽하면 크게 틀린다.
//     대상이 되면 키네마틱 오버라이드를 풀어(바디를 동적으로 다시 만든다) 로컬에서 현재 시간 기준으로 시뮬레이션한다.
//     진입 상태 = 화면 위치(보간 값) + 서버 속도(스냅샷 두 개의 차분)이고, 현재 시간과의 차이는 아래 수렴이 곧바로 줄인다 (진입 순간 튀지 않게).
//   시각 맞추기: 서버는 무브 S를 적용한 프레임에 ack(S)를 보내고 같은 프레임 끝에 스냅샷을 보낸다. 그래서 스냅샷(서버 시각 Ts)의 바디 상태는
//     로컬이 무브 S를 시뮬레이션한 프레임의 물리 스텝 결과와 같은 시점이다. 오프셋 = (ack 무브의 로컬 시계) - Ts 를 감소하는 최댓값으로 유지한다
//     (ack 손실/재정렬은 오프셋을 작게만 만든다). 로컬 시각 = Ts + 오프셋.
//   수렴 (새 스냅샷마다 ProcessBodySnapshot, 매 프레임 ApplyBodyCorrection):
//     오차 = 서버 상태(Ts) - 로컬 기록(Ts + 오프셋) — 위치, 회전, 속도·각속도(서버·로컬 모두 같은 구간의 차분). 같은 시뮬레이션이면 가속 중에도 0이다.
//     기록이 그 시각까지 없으면(막 진입) 서버 상태를 현재로 외삽(위치 + 속도 × 경과)한 값과 현재 상태를 비교한다.
//     남은 오차는 매 프레임 1 - exp(-dt / τ)만큼 바디에 더하고(CorrectBody — 렌더 보간 직전 상태도 함께 옮겨 화면이 끊기지 않음),
//     같은 보정을 기록 전체에도 더한다 (속도 보정은 "처음부터 그 속도였다"로 기록 위치를 dv × (t - 지금)만큼 옮긴다) → 다음 스냅샷은 남은 오차만 본다.
//     위치 차이가 SnapDistance보다 크면 서버 상태를 현재로 외삽한 값으로 바로 옮긴다 (회전은 스냅하지 않는다 — 구르는 공은 회전이 쉽게 어긋난다).
//   해제: ReleaseDelaySeconds 동안 근처/접촉이 없고, 로컬·서버 속도가 RestSpeed 미만이고 위치 오차가 작으면 키네마틱으로 돌아가
//     화면 위치를 BlendOutSeconds에 걸쳐 스냅샷 보간 위치로 옮긴 뒤(멈춘 물체라 과거 = 현재) 보간에 넘긴다. 다시 가까워지면 블렌드 중에도 재진입.
//   서버 권위: 서버 코드/메시지는 그대로다 (스냅샷에는 속도가 없어 차분으로 구한다). 서버 결과가 최종이고 클라이언트는 그쪽으로 수렴할 뿐이다.
//     다른 클라이언트(관찰자) 화면은 바뀌지 않는다 — 자기 캐릭터 근처만 예측하므로 남의 캐릭터가 미는 물체는 계속 보간이다.
//   한계 (전체 롤백 없음): 캐릭터 ack 재조정은 캐릭터만 되감아 기록 무브를 다시 적용한다. 예측 바디는 되감지 않고 다시 적용하는 동안
//     캐릭터가 바디를 밀지 않는다(SetCharactersPushBodies(false) — 이미 그 무브로 밀었다). 그래서 다시 적용한 캐릭터는 바디의 "현재" 위치에
//     막힌다 — 서버와 로컬 시뮬레이션이 같으면 결과가 같아 보정이 없고, 다르면(서버만 아는 충돌, 다른 플레이어가 같은 물체를 밀 때) 캐릭터 보정은
//     화면 오프셋으로, 바디 차이는 위 수렴으로 따로 흡수한다. 로컬 결정론은 보장하지 않으므로 오차가 0이 되지는 않는다.
//     원격 캐릭터(과거 보간 위치)와 예측 바디의 충돌은 과거·현재가 섞이므로 서로 밀 때는 보정이 커질 수 있다.
//     서버는 받은 무브를 프레임마다 몰아서(0~여러 개) 적용한 뒤 물리 스텝을 한 번 하고, 클라이언트는 무브 하나 → 스텝이라 무브가 몰려 도착하면
//     (손실 재전송, 지연 변동, 느린 프레임) 밀기 결과가 조금 달라진다 — 측정에서 남는 1~5cm 보정의 대부분. 가벼운 공(2kg)은 캐릭터가 걸쳐 올라타는
//     계단 오르기가 갈리는 경우가 있어 보정이 20cm 안팎까지 생긴다 (서버 무브 적용 방식을 바꾸지 않는 한 남는다).
//   옵션: 프로젝트 설정 네트워크 bPhysicsPrediction(+ 캐릭터 예측) && 앱이 SetReplicationClient로 스냅샷 버퍼를 연결. 끄면 지금까지처럼 키네마틱 보간.
//   측정 (자동 검증): --net-physics-stats → 캐릭터 보정 횟수(5cm 초과)/최대, 화면 위치의 프레임당 튐(등속 외삽과의 차이, 정지 → 출발 프레임 제외),
//     닿은(닿을 위치에 온) 때 → 화면에서 바디가 움직이기 시작할 때까지 시간. 2초마다·종료 때 로그 한 줄 (Verify.ps1 -Multiplayer -ExtraArgs).

namespace
{
	constexpr float HistorySeconds            = 1.5f;   // 로컬 기록 보관 (왕복 지연 + 여유)
	constexpr float ReleaseDelaySeconds       = 1.0f;   // 근처/접촉이 이만큼 없으면 해제 후보
	constexpr float ReleaseRadiusScale        = 1.25f;  // 해제 판정 반경 (진입 반경 × — 경계에서 들락날락하지 않게)
	constexpr float RestSpeed                 = 5.0f;   // cm/s, 이보다 느리면 멈춘 것으로 본다
	constexpr float ReleaseErrorDistance      = 2.0f;   // cm, 서버와 이만큼 가까워야 해제
	constexpr float BlendOutSeconds           = 0.25f;  // 해제할 때 화면을 보간 위치로 옮기는 시간
	constexpr float PositionCorrectionSeconds = 0.15f;  // 위치 오차가 1/e로 줄어드는 시간
	constexpr float VelocityCorrectionSeconds = 0.15f;
	constexpr float RotationCorrectionSeconds = 0.15f;
	constexpr float SnapDistance              = 100.0f; // cm, 이보다 크면 바로 옮긴다
	constexpr float MaxEnterSpeed             = 300.0f; // cm/s, 이보다 빠른 물체는 닿을 때만 예측한다 (서버가 쏜 공 등 — 내가 영향을 주지 않는 빠른 물체)
	constexpr float MaxVelocitySampleGap      = 0.25f;  // 초, 이보다 먼 두 스냅샷으로는 속도를 구하지 않는다 (멈춰서 안 보내던 구간)
	constexpr float TimingDecayPerSecond      = 0.05f;  // 시각 오프셋(감소하는 최댓값)이 내려가는 속도
	constexpr float TimingResetSeconds        = 0.5f;   // 새 표본이 이보다 작으면 오프셋을 다시 잡는다 (지연이 크게 줄었을 때)
	constexpr float StatsJumpThreshold        = 2.0f;   // cm, 측정: 눈에 띄는 튐
	constexpr float StatsMoveThreshold        = 1.0f;   // cm, 측정: 접촉 후 이만큼 움직이면 반응 시작

	FQuat Shortest(const FQuat& Q)
	{
		return Q.W < 0.0f ? FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W) : Q;
	}

	// 회전 From → To를 Seconds 동안 했을 때의 각속도 (rad/s, 월드)
	FVector3 AngularVelocityBetween(const FQuat& From, const FQuat& To, float Seconds)
	{
		const FQuat Delta   = Shortest((To * From.Conjugate()).GetNormalized());
		const float SinHalf = std::sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y + Delta.Z * Delta.Z);
		if (SinHalf < 1.0e-6f || Seconds <= 0.0f)
		{
			return FVector3();
		}
		const float Angle = 2.0f * std::atan2(SinHalf, Delta.W);
		return FVector3(Delta.X, Delta.Y, Delta.Z) * (Angle / (SinHalf * Seconds));
	}

	float SmoothStep(float X)
	{
		X = std::clamp(X, 0.0f, 1.0f);
		return X * X * (3.0f - 2.0f * X);
	}

	// 시각순 기록에서 Time의 값 (범위 밖이면 false)
	template <typename TSample>
	bool SampleHistory(const std::deque<TSample>& History, float Time, FVector3& OutPosition, FQuat& OutRotation)
	{
		if (History.empty() || Time < History.front().Time || Time > History.back().Time)
		{
			return false;
		}
		const auto Upper = std::lower_bound(History.begin(), History.end(), Time, [](const TSample& Sample, float Value) { return Sample.Time < Value; });
		if (Upper == History.begin())
		{
			OutPosition = Upper->Position;
			OutRotation = Upper->Rotation;
			return true;
		}
		const TSample& From  = *(Upper - 1);
		const TSample& To    = *Upper;
		const float    Span  = To.Time - From.Time;
		const float    Alpha = Span > 0.0f ? (Time - From.Time) / Span : 1.0f;
		OutPosition          = FVector3::Lerp(From.Position, To.Position, Alpha);
		OutRotation          = FQuat::Slerp(From.Rotation, To.Rotation, Alpha);
		return true;
	}

	// 마지막 스냅샷 두 개의 차분 속도/각속도 (간격이 너무 멀면 0). 반환: 차분 구간 (0 = 없음)
	float EstimateServerMotion(const std::deque<FReplicationClient::FTransformSample>& Samples, FVector3& OutVelocity, FVector3& OutAngular)
	{
		OutVelocity = FVector3();
		OutAngular  = FVector3();
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
		OutVelocity = (Newest.Position - Prev.Position) * (1.0f / Span);
		OutAngular  = AngularVelocityBetween(Prev.Rotation, Newest.Rotation, Span);
		return Span;
	}
} // namespace

float FGameWorld::FMotionTrack::Push(const FVector3& Position, float DeltaSeconds)
{
	float Jump = 0.0f;
	// 멈춰 있다가 출발하는 프레임은 뺀다 (캐릭터 이동은 첫 프레임에 바로 걷는 속도 — 화면 튐이 아니다)
	if (Samples >= 2 && PreviousDelta > 0.0f && FVector3::DistanceSquared(Current, Previous) > 0.25f)
	{
		const FVector3 Expected = Current + (Current - Previous) * (DeltaSeconds / PreviousDelta);
		Jump                    = FVector3::Distance(Position, Expected);
	}
	Previous      = Current;
	Current       = Position;
	PreviousDelta = DeltaSeconds;
	++Samples;
	return Jump;
}

bool FGameWorld::IsPhysicsSimulatedLocally(FEntity Entity) const
{
	const auto Found = PredictedBodies.find(Entity);
	return Found != PredictedBodies.end() && !Found->second.bBlendingOut;
}

bool FGameWorld::IsPhysicsPredictionEnabled() const
{
	const FNetworkSettings& Settings = FProjectSettings::Get().Network;
	return Mode == ENetMode::Client && Scene != nullptr && Replication != nullptr && Systems.Physics != nullptr && Systems.Physics->IsActive() &&
	       Settings.bPhysicsPrediction && Settings.bClientPrediction;
}

void FGameWorld::CollectPredictionCharacters(std::vector<FEntity>& OutCharacters) const
{
	OutCharacters.clear();
	Scene->GetRegistry().View<FCharacterMovementComponent>().Each([&](FEntity Entity, FCharacterMovementComponent&) {
		if (IsPredicted(Entity))
		{
			OutCharacters.push_back(Entity);
		}
	});
}

void FGameWorld::UpdatePhysicsPredictionTiming()
{
	const float Latest = Replication->GetLatestSnapshotTime();
	if (Latest <= LastSnapshotTime)
	{
		return;
	}
	LastSnapshotTime = Latest;
	if (LastAckMoveTime < 0.0f)
	{
		return; // 아직 ack 없음
	}
	const float Sample = LastAckMoveTime - Latest;
	if (!bPredictionTimingValid || Sample > PredictionTimeOffset || PredictionTimeOffset - Sample > TimingResetSeconds)
	{
		PredictionTimeOffset   = Sample;
		bPredictionTimingValid = true;
	}
}

void FGameWorld::TickPhysicsPrediction(float DeltaSeconds)
{
	PredictionClock += DeltaSeconds;
	PredictionStats.LastDelta = DeltaSeconds;
	if (Mode != ENetMode::Client || Scene == nullptr || Systems.Physics == nullptr || !Systems.Physics->IsActive())
	{
		return;
	}
	FPhysicsSystem& Physics  = *Systems.Physics;
	FRegistry&      Registry = Scene->GetRegistry();
	const bool      bEnabled = IsPhysicsPredictionEnabled();
	if (bEnabled)
	{
		PredictionTimeOffset -= TimingDecayPerSecond * DeltaSeconds;
		UpdatePhysicsPredictionTiming();
	}

	// 사라진 엔티티
	for (auto It = PredictedBodies.begin(); It != PredictedBodies.end();)
	{
		It = Registry.IsValid(It->first) && Registry.Has<FNetIdComponent>(It->first) ? std::next(It) : PredictedBodies.erase(It);
	}

	// 대상 선정: 예측 캐릭터 근처 또는 접촉
	std::vector<FEntity> Entered;
	if (bEnabled && bPredictionTimingValid)
	{
		std::vector<FEntity> Characters;
		CollectPredictionCharacters(Characters);
		std::unordered_set<FEntity> Touching;
		std::vector<FEntity>        Contacts;
		std::vector<FVector3>       CharacterPositions;
		for (const FEntity Character : Characters)
		{
			Physics.GetCharacterContacts(Character, Contacts);
			Touching.insert(Contacts.begin(), Contacts.end());
			CharacterPositions.push_back(Scene->GetTransform(Character).Position);
		}
		const float Radius = std::max(FProjectSettings::Get().Network.PhysicsPredictionRadius, 0.0f);
		Registry.View<FNetIdComponent, FRigidBodyComponent>().Each([&](FEntity Entity, FNetIdComponent&, FRigidBodyComponent& RigidBody) {
			if (RigidBody.MotionType != static_cast<int32>(EPhysicsMotionType::Dynamic) || Registry.Has<FCharacterMovementComponent>(Entity) ||
			    Scene->GetParent(Entity).IsValid() || Scene->IsSocketAttached(Entity) || !Physics.HasBody(Entity))
			{
				return;
			}
			const auto     Found    = PredictedBodies.find(Entity);
			const float    Range    = Found != PredictedBodies.end() ? Radius * ReleaseRadiusScale : Radius;
			const FVector3 Position = Scene->GetTransform(Entity).Position;
			bool           bNear    = Touching.contains(Entity);
			for (const FVector3& CharacterPosition : CharacterPositions)
			{
				bNear = bNear || FVector3::DistanceSquared(Position, CharacterPosition) < Range * Range;
			}
			if (Found == PredictedBodies.end())
			{
				// 빠른 물체는 닿았을 때만 (반경 안을 스쳐 가는 서버 발사체를 현재로 외삽하면 크게 틀린다)
				if (bNear && !Touching.contains(Entity))
				{
					FVector3 Velocity, Angular;
					if (const std::deque<FReplicationClient::FTransformSample>* Samples = Replication->FindTransformSamples(Entity))
					{
						EstimateServerMotion(*Samples, Velocity, Angular);
					}
					bNear = Velocity.Length() < MaxEnterSpeed;
				}
				if (bNear)
				{
					Entered.push_back(Entity);
				}
				return;
			}
			FPredictedBody& Body = Found->second;
			Body.IdleSeconds     = bNear ? 0.0f : Body.IdleSeconds + DeltaSeconds;
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
			PredictedBodies[Entity] = FPredictedBody();
		}
		Physics.SyncBodies(*Scene);
		for (const FEntity Entity : Entered)
		{
			FPhysicsBodyMotion Motion;
			Motion.Position = Scene->GetTransform(Entity).Position;
			Motion.Rotation = Scene->GetTransform(Entity).Rotation;
			if (const std::deque<FReplicationClient::FTransformSample>* Samples = Replication->FindTransformSamples(Entity))
			{
				EstimateServerMotion(*Samples, Motion.LinearVelocity, Motion.AngularVelocity);
			}
			Physics.SetBodyMotion(Entity, Motion);
			E_LOG(LogNet, Verbose, "물리 예측 시작: NetId {}", NetReplication::GetNetId(*Scene, Entity));
		}
	}

	// 수렴 / 해제 / 해제 블렌드
	for (auto It = PredictedBodies.begin(); It != PredictedBodies.end();)
	{
		const FEntity   Entity = It->first;
		FPredictedBody& Body   = It->second;
		if (!Body.bBlendingOut)
		{
			if (!bEnabled)
			{
				BeginBodyBlendOut(Entity, Body); // 예측을 끄면(설정·캐릭터 예측 없음) 보간으로 돌아간다
			}
			else
			{
				ProcessBodySnapshot(Entity, Body);
				ApplyBodyCorrection(Entity, Body, DeltaSeconds);
				FPhysicsBodyMotion Motion;
				if (Body.IdleSeconds > ReleaseDelaySeconds && Physics.GetBodyMotion(Entity, Motion) && Motion.LinearVelocity.Length() < RestSpeed &&
				    Body.ServerVelocity.Length() < RestSpeed && Body.PositionError.Length() < ReleaseErrorDistance)
				{
					BeginBodyBlendOut(Entity, Body);
				}
			}
		}
		if (Body.bBlendingOut)
		{
			// 키네마틱(다음 물리 갱신에 다시 만들어진다)으로 화면 위치를 보간 값까지 옮긴다
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
				E_LOG(LogNet, Verbose, "물리 예측 해제: NetId {}", NetReplication::GetNetId(*Scene, Entity));
				It = PredictedBodies.erase(It); // 이제 스냅샷 보간이 쓴다
				continue;
			}
		}
		++It;
	}
}

void FGameWorld::BeginBodyBlendOut(FEntity Entity, FPredictedBody& Body)
{
	Body.bBlendingOut      = true;
	Body.BlendSeconds      = 0.0f;
	Body.BlendFromPosition = Scene->GetTransform(Entity).Position;
	Body.BlendFromRotation = Scene->GetTransform(Entity).Rotation;
	Body.History.clear();
	Body.PositionError = FVector3();
	Body.VelocityError = FVector3();
	Body.AngularError  = FVector3();
	Body.RotationError = FQuat::Identity;
}

void FGameWorld::ProcessBodySnapshot(FEntity Entity, FPredictedBody& Body)
{
	const std::deque<FReplicationClient::FTransformSample>* Samples = Replication->FindTransformSamples(Entity);
	if (Samples == nullptr || Samples->back().ServerTime <= Body.LastSampleTime)
	{
		return; // 새 스냅샷 없음 (멈춘 물체는 서버가 잠시 뒤 보내지 않는다 — 오차도 새로 생기지 않는다)
	}
	FPhysicsBodyMotion Local;
	if (!Systems.Physics->GetBodyMotion(Entity, Local))
	{
		return;
	}
	const FReplicationClient::FTransformSample& Newest = Samples->back();
	Body.LastSampleTime                                = Newest.ServerTime;
	FVector3    ServerAngular;
	const float Span      = EstimateServerMotion(*Samples, Body.ServerVelocity, ServerAngular);
	const float LocalTime = Newest.ServerTime + PredictionTimeOffset; // 이 스냅샷 상태에 해당하는 로컬 시각
	const float Ahead     = std::max(LastRecordTime - LocalTime, 0.0f); // 그 시각부터 지금 상태(마지막 기록)까지

	FVector3 HistoryPosition;
	FQuat    HistoryRotation;
	if (SampleHistory(Body.History, LocalTime, HistoryPosition, HistoryRotation))
	{
		// 같은 시점끼리 비교 (같은 시뮬레이션이면 미는 중에도 0)
		Body.PositionError = Newest.Position - HistoryPosition;
		Body.RotationError = Shortest((Newest.Rotation * HistoryRotation.Conjugate()).GetNormalized());
		FVector3 EarlierPosition;
		FQuat    EarlierRotation;
		if (Span > 0.0f && SampleHistory(Body.History, LocalTime - Span, EarlierPosition, EarlierRotation))
		{
			// 속도도 같은 구간의 차분끼리
			Body.VelocityError = Body.ServerVelocity - (HistoryPosition - EarlierPosition) * (1.0f / Span);
			Body.AngularError  = ServerAngular - AngularVelocityBetween(EarlierRotation, HistoryRotation, Span);
		}
		else
		{
			Body.VelocityError = Body.ServerVelocity - Local.LinearVelocity;
			Body.AngularError  = ServerAngular - Local.AngularVelocity;
		}
	}
	else
	{
		// 기록이 그 시각까지 없다 (막 진입): 서버 상태를 현재로 외삽해 지금 상태와 비교
		Body.PositionError = Newest.Position + Body.ServerVelocity * Ahead - Local.Position;
		Body.RotationError = Shortest((Newest.Rotation * Local.Rotation.Conjugate()).GetNormalized());
		Body.VelocityError = Body.ServerVelocity - Local.LinearVelocity;
		Body.AngularError  = ServerAngular - Local.AngularVelocity;
	}

	// 위치만 스냅한다 (회전 차이는 충돌에 덜 중요하고, 구르는 공은 회전이 쉽게 어긋난다 — 부드럽게만 맞춘다)
	if (Body.PositionError.Length() > SnapDistance)
	{
		E_LOG(LogNet, Verbose, "물리 예측 스냅: NetId {} {:.1f}cm", NetReplication::GetNetId(*Scene, Entity), Body.PositionError.Length());
		++PredictionStats.Snaps;
		FPhysicsBodyMotion Snapped;
		Snapped.Position        = Newest.Position + Body.ServerVelocity * Ahead;
		Snapped.Rotation        = Newest.Rotation;
		Snapped.LinearVelocity  = Body.ServerVelocity;
		Snapped.AngularVelocity = ServerAngular;
		Systems.Physics->SetBodyMotion(Entity, Snapped);
		Body.History.clear();
		Body.PositionError = FVector3();
		Body.VelocityError = FVector3();
		Body.AngularError  = FVector3();
		Body.RotationError = FQuat::Identity;
	}
}

void FGameWorld::ApplyBodyCorrection(FEntity Entity, FPredictedBody& Body, float DeltaSeconds)
{
	if (DeltaSeconds <= 0.0f)
	{
		return;
	}
	const float    PositionAlpha = 1.0f - std::exp(-DeltaSeconds / PositionCorrectionSeconds);
	const float    VelocityAlpha = 1.0f - std::exp(-DeltaSeconds / VelocityCorrectionSeconds);
	const float    RotationAlpha = 1.0f - std::exp(-DeltaSeconds / RotationCorrectionSeconds);
	const FVector3 DeltaPosition = Body.PositionError * PositionAlpha;
	const FVector3 DeltaVelocity = Body.VelocityError * VelocityAlpha;
	const FVector3 DeltaAngular  = Body.AngularError * VelocityAlpha;
	const FQuat    DeltaRotation = FQuat::Slerp(FQuat::Identity, Body.RotationError, RotationAlpha);
	Body.PositionError           = Body.PositionError - DeltaPosition;
	Body.VelocityError           = Body.VelocityError - DeltaVelocity;
	Body.AngularError            = Body.AngularError - DeltaAngular;
	Body.RotationError           = Shortest((Body.RotationError * DeltaRotation.Conjugate()).GetNormalized());
	if (DeltaPosition.LengthSquared() < 1.0e-8f && DeltaVelocity.LengthSquared() < 1.0e-8f && DeltaAngular.LengthSquared() < 1.0e-10f &&
	    1.0f - std::abs(DeltaRotation.W) < 1.0e-9f)
	{
		return;
	}
	Systems.Physics->CorrectBody(Entity, DeltaPosition, DeltaRotation, DeltaVelocity, DeltaAngular);
	// 기록에도 같은 보정 (다음 스냅샷이 남은 오차만 보게). 속도 보정은 "처음부터 그 속도로 지금 상태에 왔다"로 과거를 옮긴다
	const float AngularSpeed = DeltaAngular.Length();
	for (FBodyHistorySample& Sample : Body.History)
	{
		const float Before = Sample.Time - LastRecordTime; // 0 이하
		Sample.Position    = Sample.Position + DeltaPosition + DeltaVelocity * Before;
		Sample.Rotation    = (DeltaRotation * Sample.Rotation).GetNormalized();
		if (AngularSpeed > 1.0e-6f)
		{
			Sample.Rotation = (FQuat::FromAxisAngle(DeltaAngular * (1.0f / AngularSpeed), AngularSpeed * Before) * Sample.Rotation).GetNormalized();
		}
	}
}

void FGameWorld::RecordPhysicsPrediction()
{
	if (Mode != ENetMode::Client || Systems.Physics == nullptr)
	{
		return;
	}
	LastRecordTime = PredictionClock;
	for (auto& [Entity, Body] : PredictedBodies)
	{
		FPhysicsBodyMotion Motion;
		if (Body.bBlendingOut || !Systems.Physics->GetBodyMotion(Entity, Motion))
		{
			continue;
		}
		Body.History.push_back({ PredictionClock, Motion.Position, Motion.Rotation });
		while (!Body.History.empty() && Body.History.front().Time < PredictionClock - HistorySeconds)
		{
			Body.History.pop_front();
		}
	}
	if (PredictionStats.bEnabled)
	{
		TickPhysicsPredictionStats();
	}
}

// ---------------------------------------------------------------- 측정 (--net-physics-stats, 자동 검증)

void FGameWorld::TickPhysicsPredictionStats()
{
	FPhysicsPredictionStats& Stats = PredictionStats;
	const float              Delta = Stats.LastDelta;
	Stats.Elapsed += Delta;
	++Stats.Frames;

	std::vector<FEntity> Characters;
	CollectPredictionCharacters(Characters);
	std::unordered_set<FEntity> Touching;
	std::vector<FEntity>        Contacts;
	for (const FEntity Character : Characters)
	{
		const float Jump       = Stats.Characters[Character].Push(Scene->GetTransform(Character).Position, Delta);
		Stats.CharacterMaxJump = std::max(Stats.CharacterMaxJump, Jump);
		Stats.CharacterJumpFrames += Jump > StatsJumpThreshold ? 1u : 0u;
		Systems.Physics->GetCharacterContacts(Character, Contacts);
		Touching.insert(Contacts.begin(), Contacts.end());
	}
	FRegistry& Registry = Scene->GetRegistry();
	Registry.View<FNetIdComponent, FRigidBodyComponent>().Each([&](FEntity Entity, FNetIdComponent& NetId, FRigidBodyComponent& RigidBody) {
		if (RigidBody.MotionType != static_cast<int32>(EPhysicsMotionType::Dynamic) || Registry.Has<FCharacterMovementComponent>(Entity))
		{
			return;
		}
		FBodyStats&    Body     = Stats.Bodies[Entity];
		const FVector3 Position = Scene->GetTransform(Entity).Position;
		// 움직이기 시작한 시각: 접촉 전에는 멈춰 있으면 기준 위치를 다시 잡는다 (떨어져 자리 잡는 공 등)
		if (!Body.bInitialized)
		{
			Body.bInitialized    = true;
			Body.RestPosition    = Position;
			Body.LastPosition    = Position;
		}
		if (Body.ContactTime < 0.0f && FVector3::Distance(Position, Body.LastPosition) < 0.05f && FVector3::Distance(Position, Body.RestPosition) > StatsMoveThreshold)
		{
			Body.RestPosition = Position;
			Body.MoveTime     = -1.0f;
		}
		Body.LastPosition = Position;
		if (Body.MoveTime < 0.0f && FVector3::Distance(Position, Body.RestPosition) > StatsMoveThreshold)
		{
			Body.MoveTime = Stats.Elapsed;
		}
		if (Body.ContactTime < 0.0f)
		{
			// 접촉 = 캐릭터가 닿았거나, 수평 거리상 닿을 위치에 왔다 (키네마틱 보간 상자는 과거 위치라 물리 접촉이 늦을 수 있다)
			const FVector3 Scale  = Scene->GetTransform(Entity).Scale;
			float          Extent = 0.0f;
			if (const FBoxColliderComponent* Box = Registry.TryGet<FBoxColliderComponent>(Entity))
			{
				Extent = std::max(std::abs(Box->HalfExtents.X * Scale.X), std::abs(Box->HalfExtents.Y * Scale.Y));
			}
			else if (const FSphereColliderComponent* Sphere = Registry.TryGet<FSphereColliderComponent>(Entity))
			{
				Extent = std::abs(Sphere->Radius) * std::max({ std::abs(Scale.X), std::abs(Scale.Y), std::abs(Scale.Z) });
			}
			bool bReached = Touching.contains(Entity);
			for (const FEntity Character : Characters)
			{
				const FVector3 Offset = Scene->GetTransform(Character).Position - Position;
				const float    Reach  = Registry.Get<FCharacterMovementComponent>(Character).CapsuleRadius + Extent + 2.0f;
				bReached              = bReached || Offset.X * Offset.X + Offset.Y * Offset.Y < Reach * Reach;
			}
			if (!bReached)
			{
				return;
			}
			Body.ContactTime = Stats.Elapsed;
			Body.bReacted    = Body.MoveTime >= 0.0f && Body.MoveTime < Stats.Elapsed; // 닿을 때 이미 움직이던 물체(떨어지는 중, 다른 것에 밀림)는 반응 지연에서 뺀다
			E_LOG(LogNet, Display, "물리 예측 측정: NetId {} 접촉 ({:.2f}초)", NetId.NetId, Stats.Elapsed);
		}
		const float Jump  = Body.Track.Push(Position, Delta);
		Stats.BodyMaxJump = std::max(Stats.BodyMaxJump, Jump);
		Stats.BodyJumpFrames += Jump > StatsJumpThreshold ? 1u : 0u;
		if (!Body.bReacted && Body.MoveTime >= 0.0f)
		{
			// 음수 = 내 캐릭터가 닿기 전에 화면에서 이미 움직였다 (서버 쪽 캐릭터가 앞섰다)
			Body.bReacted = true;
			Stats.ReactionDelays.push_back(Body.MoveTime - Body.ContactTime);
			E_LOG(LogNet, Display, "물리 예측 측정: NetId {} 반응 {:.3f}초", NetId.NetId, Body.MoveTime - Body.ContactTime);
		}
	});
	if (Replication != nullptr && Replication->HasServerClock())
	{
		Stats.InterpolationMargin = Replication->GetLatestSnapshotTime() - (Replication->GetServerClock() - Replication->InterpolationDelay);
	}
	if (Stats.Elapsed >= Stats.NextLog)
	{
		Stats.NextLog += 2.0f;
		LogPhysicsPredictionStats("중간");
	}
}

void FGameWorld::LogPhysicsPredictionStats(const char* Label) const
{
	const FPhysicsPredictionStats& Stats = PredictionStats;
	float                          Sum = 0.0f, Max = 0.0f;
	for (const float Delay : Stats.ReactionDelays)
	{
		Sum += Delay;
		Max = std::max(Max, Delay);
	}
	const float Average = Stats.ReactionDelays.empty() ? 0.0f : Sum / static_cast<float>(Stats.ReactionDelays.size());
	E_LOG(LogNet, Display,
	      "물리 예측 측정 ({}): {}프레임 {:.1f}초, 캐릭터 보정 {}회 (5cm 초과 {}회, 최대 {:.1f}cm), 캐릭터 최대 튐 {:.2f}cm (2cm 초과 {}프레임), "
	      "반응한 바디 {}개 지연 평균 {:.3f}초 / 최대 {:.3f}초, 바디 최대 튐 {:.2f}cm (2cm 초과 {}프레임), 바디 스냅 {}회, 예측 중 {}개, 예측 선행 {:.3f}초, 보간 여유 {:.3f}초",
	      Label, Stats.Frames, Stats.Elapsed, CharacterCorrections, Stats.BigCorrections, Stats.CorrectionMax, Stats.CharacterMaxJump, Stats.CharacterJumpFrames,
	      Stats.ReactionDelays.size(), Average, Max, Stats.BodyMaxJump, Stats.BodyJumpFrames, Stats.Snaps, PredictedBodies.size(),
	      bPredictionTimingValid ? PredictionClock - PredictionTimeOffset - LastSnapshotTime : 0.0f,
	      Stats.InterpolationMargin);
}
