#include "World/GameWorld.h"

#include "Core/Log.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

// 캐릭터 이동 + 클라이언트 예측 (언리얼 CharacterMovement의 클라이언트 예측/서버 보정과 같은 구조)
//
//   조종 주체 (IsLocallyControlled): 소유 플레이어가 로컬(호스트/클라이언트)이면 그 프로세스, 서버 소유(owner < 0)면 서버/Standalone.
//   매 틱 (TickCharacters, 스크립트 직후 — 스크립트가 AddMovementInput/Jump로 입력을 넣는다):
//     - 조종 주체 + 서버/Standalone: 무브를 만들어 바로 시뮬레이션 (지연 없음)
//     - 조종 주체 + 클라이언트(예측): 무브에 순번을 붙여 바로 시뮬레이션하고 기록, 서버로 최근 무브 MaxMovesPerPacket개를 보낸다
//     - 서버의 원격 플레이어 캐릭터: 받은 무브를 순번 순서대로 시뮬레이션하고 소유자에게 ack(마지막 순번 + 상태)
//     - 클라이언트의 다른 캐릭터: 복제 스냅샷 보간 위치에 캡슐만 맞춘다 (FollowTransform)
//   재조정 (ReceiveCharacterAck): ack 상태로 되돌린 뒤 순번이 그보다 큰 기록 무브를 다시 적용한다.
//     같은 무브 → 같은 결과라 보통 차이가 없고, 서버만 아는 일(서버가 시뮬레이션하는 공/상자와 부딪힘, 다른 캐릭터, 순간이동)이 있었을 때만 위치가 바뀐다.
//     바뀐 만큼은 화면 오프셋(VisualOffset)으로 옮겨 CorrectionSmoothingSeconds에 걸쳐 0으로 줄인다 (시뮬레이션은 즉시 서버를 따른다).
//     SnapCorrectionDistance보다 크면(순간이동) 바로 옮긴다.
//     다시 적용하는 동안 캐릭터는 동적 바디를 밀지 않는다 (그 무브로 이미 밀었다). 물리 예측 바디가 있으면 바디를 기록 위치에 잠시 둔 다시 적용도
//     해서 지금 예측에 가까운 쪽을 쓴다 (바디 재시뮬레이션 없음) — GameWorldPhysicsPrediction.cpp 머리 주석
//     ack 무브의 로컬 시각(MoveTimes)은 물리 예측이 스냅샷 시각을 로컬 기록에 맞추는 데 쓴다.
//   예측 옵션 (UsesClientPrediction = 컴포넌트 bClientPrediction && 프로젝트 설정 네트워크 → 클라이언트 예측): 끄면 소유 클라이언트는
//     무브를 보내기만 하고 미리 움직이지 않으며, 자기 캐릭터도 스냅샷 보간으로 보여 준다 (IsPredicted = false). 서버 쪽은 같다.
//   넉백/발사 (entity:LaunchCharacter/AddKnockback — 규칙은 CharacterMovement.h, 2D GameWorldCharacter2D.cpp와 같은 규칙): 엔티티에 쌓였다가 다음 무브에 실린다.
//     조종하는 쪽(Standalone, 서버 소유를 서버가, 소유 클라이언트): 자기 다음 무브 — 클라이언트면 무브와 함께 서버로 간다 (서버는 성분을 MaxLaunchSpeed로 자름)
//     서버의 원격 플레이어 캐릭터: 서버가 준 넉백은 다음에 적용하는 받은 무브(큐 첫 무브)에 합친다 — 받은 무브가 없으면 올 때까지 기다린다.
//       결과(속도·경직 타이머)는 ack 상태로 소유 클라이언트에 가고 재조정이 같은 궤적을 맞춘다 (넉백을 실은 무브까지는 클라이언트가 넉백 없이 예측 — 보정)
//     클라이언트의 다른 캐릭터: 무시 (서버 결과를 복제로 받는다). 같은 넉백을 서버와 소유 클라이언트가 둘 다 부르면 두 번 실리므로 한쪽(보통 서버)에서만
//     시간 정지(게임 시간 배율 0 — Standalone 전용)인 틱은 무브가 없다: 쌓인 입력은 버리고 넉백은 다음 무브까지 남긴다
//   메시지 (비신뢰):
//     CharacterMoves: uint32 NetId, uint8 개수, [uint32 순번, float dt, float 입력 X, float 입력 Y, float yaw, uint8 플래그(1 점프, 2 루트 모션, 4 발사)
//                     (+ 루트 모션이면 float 속도 X, float 속도 Y — 서버가 MaxRootMotionSpeed로 자른다)
//                     (+ 발사면 FVector3 속도, uint8 덮어쓰기(1 XY, 2 Z), float 경직 초 — 서버가 MaxLaunchSpeed/MaxStunSeconds로 자른다)]...
//     CharacterAck:   uint32 NetId, uint32 순번, FVector3 위치, FVector3 속도, uint8 바닥, float 경직 타이머
//   서버는 무브 dt를 FCharacterMove::MaxMoveDeltaSeconds로 자르고, 소유자가 아닌 연결이 보낸 무브는 버린다.

namespace
{
	constexpr uint8  MaxMovesPerPacket  = 8;   // 겹쳐 보내는 최근 무브 수 (손실 대비)
	constexpr size_t MaxPredictedMoves  = 240; // 확인 안 된 무브 기록 상한 (4초 — 서버 응답이 끊기면 오래된 것부터 버린다)
	constexpr size_t MaxQueuedMoves     = 120; // 서버가 한 캐릭터에 쌓아 두는 무브 상한
	constexpr float  CorrectionSmoothingSeconds = 0.1f;   // 보정 오프셋이 1/e로 줄어드는 시간
	constexpr float  SnapCorrectionDistance     = 150.0f; // cm, 이보다 큰 보정은 부드럽게 하지 않는다
	constexpr float  MaxReplayPenetration       = 1.0f;   // cm, 예측 바디를 기록 위치에 둔 다시 적용 결과가 지금 바디와 이보다 깊이 겹치면 버린다

	bool IsFiniteMove(const FCharacterMove& Move)
	{
		return std::isfinite(Move.DeltaSeconds) && std::isfinite(Move.Input.X) && std::isfinite(Move.Input.Y) && std::isfinite(Move.Yaw);
	}

	FEntity FindByNetId(FScene& Scene, uint32 NetId)
	{
		FEntity Found;
		Scene.GetRegistry().View<FNetIdComponent>().Each([&](FEntity Entity, FNetIdComponent& Component) {
			if (Component.NetId == NetId)
			{
				Found = Entity;
			}
		});
		return Found;
	}
} // namespace

bool FGameWorld::UsesClientPrediction(FEntity Entity) const
{
	if (Scene == nullptr || !FProjectSettings::Get().Network.bClientPrediction)
	{
		return false;
	}
	if (const FCharacterMovementComponent* Movement = Scene->GetRegistry().TryGet<FCharacterMovementComponent>(Entity))
	{
		return Movement->bClientPrediction;
	}
	const FCharacterMovement2DComponent* Movement2D = Scene->GetRegistry().TryGet<FCharacterMovement2DComponent>(Entity); // World/GameWorldCharacter2D.cpp
	return Movement2D != nullptr && Movement2D->bClientPrediction;
}

bool FGameWorld::IsPredicted(FEntity Entity) const
{
	if (IsPhysicsPredicted(Entity))
	{
		return true; // 물리 예측 바디 (해제 블렌드 중에도 화면은 물리 예측이 맡는다)
	}
	return Mode == ENetMode::Client && Scene != nullptr && Scene->GetRegistry().IsValid(Entity) &&
	       (Scene->GetRegistry().Has<FCharacterMovementComponent>(Entity) || Scene->GetRegistry().Has<FCharacterMovement2DComponent>(Entity)) &&
	       GetOwner(Entity) >= 0 && IsLocallyControlled(Entity) &&
	       UsesClientPrediction(Entity);
}

void FGameWorld::TickCharacters(float DeltaSeconds)
{
	FPhysicsSystem* Physics = Systems.Physics;
	if (Physics == nullptr || !Physics->IsActive() || Scene == nullptr)
	{
		return;
	}
	Physics->SyncCharacters(*Scene);
	std::vector<FEntity> Characters;
	Scene->GetRegistry().View<FCharacterMovementComponent>().Each([&](FEntity Entity, FCharacterMovementComponent&) { Characters.push_back(Entity); });
	if (DeltaSeconds <= 0.0f)
	{
		// 시간 정지 (게임 시간 배율 0/히트스톱 — Standalone, GameWorld.cpp "시간 배율"): 무브 없음, 쌓인 입력은 버린다 (넉백은 다음 무브까지 남는다)
		for (const FEntity Entity : Characters)
		{
			Physics->ConsumePendingMove(Entity, 0.0f, nullptr);
		}
		return;
	}
	for (const FEntity Entity : Characters)
	{
		const int32 Owner = GetOwner(Entity);
		if (IsLocallyControlled(Entity))
		{
			// 플레이어 캐릭터는 시점 방향(yaw)을 보고, 서버 소유(AI 등)는 이동 방향을 본다
			const float    Yaw  = LocalControlRotation.X;
			FCharacterMove Move = Physics->ConsumePendingMove(Entity, DeltaSeconds, Owner >= 0 ? &Yaw : nullptr);
			// 루트 모션 (직전 애니메이션 갱신이 쌓아 둔 것 — 규칙은 CharacterMovement.h)
			if (FCharacterMovementComponent* Movement = Scene->GetRegistry().TryGet<FCharacterMovementComponent>(Entity))
			{
				CharacterMovementMath::ConsumeRootMotion(*Movement, Move);
			}
			Physics->MergePendingLaunch(Entity, Move); // 넉백/발사는 자기 무브에 (클라이언트면 무브와 함께 서버로)
			if (Mode == ENetMode::Client)
			{
				FPredictedCharacter& Predicted = PredictedCharacters[Entity];
				Move.Sequence                  = ++Predicted.NextSequence;
				if (!UsesClientPrediction(Entity))
				{
					// 예측 끔: 보내기만 하고 화면은 서버 결과(스냅샷 보간)를 따른다
					Predicted.Moves.push_back(Move);
					while (Predicted.Moves.size() > MaxPredictedMoves)
					{
						Predicted.Moves.pop_front();
					}
					SendCharacterMoves(Entity);
					Physics->FollowTransform(*Scene, Entity);
					continue;
				}
				// 보정 오프셋을 시간에 따라 줄인다 (이번 프레임 트랜스폼에 반영)
				Predicted.VisualOffset = Predicted.VisualOffset * std::exp(-DeltaSeconds / CorrectionSmoothingSeconds);
				if (Predicted.VisualOffset.LengthSquared() < 0.01f)
				{
					Predicted.VisualOffset = FVector3();
				}
				Physics->SetCharacterVisualOffset(*Scene, Entity, Predicted.VisualOffset);
				Physics->SimulateCharacter(*Scene, Entity, Move); // 예측: 바로 움직인다
				Predicted.Moves.push_back(Move);
				Predicted.MoveTimes.emplace_back(Move.Sequence, PredictionClock); // 이 무브의 결과는 이번 프레임 물리 스텝 뒤에 기록된다
				while (Predicted.Moves.size() > MaxPredictedMoves)
				{
					Predicted.Moves.pop_front();
				}
				while (Predicted.MoveTimes.size() > MaxPredictedMoves)
				{
					Predicted.MoveTimes.pop_front();
				}
				SendCharacterMoves(Entity);
			}
			else
			{
				Physics->SimulateCharacter(*Scene, Entity, Move);
			}
		}
		else if (Mode == ENetMode::Client)
		{
			FCharacterMove Ignored;
			if (Physics->MergePendingLaunch(Entity, Ignored)) // 남의 캐릭터 넉백은 서버가 준다 (복제로 받는다)
			{
				E_LOG(LogNet, Verbose, "캐릭터 넉백 무시: 이 클라이언트가 조종하지 않는 캐릭터 (서버에서 부를 것)");
			}
			Physics->FollowTransform(*Scene, Entity); // 다른 플레이어: 복제 보간 위치에 캡슐만 맞춘다
		}
		else if (Owner >= 0)
		{
			// 서버: 원격 플레이어 캐릭터는 받은 무브만큼 움직인다 (무브가 없으면 그 자리 — 클라이언트가 시간을 정한다)
			FServerCharacter& Server = ServerCharacters[Entity];
			if (Server.Queue.empty())
			{
				continue; // 서버가 준 넉백도 다음 무브까지 기다린다
			}
			Physics->MergePendingLaunch(Entity, Server.Queue.front()); // 서버가 준 넉백 → 이번에 적용하는 첫 무브 (결과는 ack 상태로)
			for (const FCharacterMove& Move : Server.Queue)
			{
				Physics->SimulateCharacter(*Scene, Entity, Move);
				Server.LastApplied = Move.Sequence;
			}
			Server.Queue.clear();
			SendCharacterAck(Entity, Server.LastApplied);
		}
	}
}

void FGameWorld::SendCharacterMoves(FEntity Entity)
{
	if (Systems.Net == nullptr || Systems.Net->GetClientState() != FNetDriver::EClientState::Joined)
	{
		return;
	}
	const uint32 NetId = NetReplication::GetNetId(*Scene, Entity);
	const auto   Found = PredictedCharacters.find(Entity);
	if (NetId == InvalidNetId || Found == PredictedCharacters.end() || Found->second.Moves.empty())
	{
		return;
	}
	const std::deque<FCharacterMove>& Moves = Found->second.Moves;
	const size_t                      Count = std::min<size_t>(Moves.size(), MaxMovesPerPacket);
	FBinaryWriter Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::CharacterMoves));
	Writer.Write(NetId);
	Writer.Write(static_cast<uint8>(Count));
	for (size_t Index = Moves.size() - Count; Index < Moves.size(); ++Index)
	{
		const FCharacterMove& Move = Moves[Index];
		Writer.Write(Move.Sequence);
		Writer.Write(Move.DeltaSeconds);
		Writer.Write(Move.Input.X);
		Writer.Write(Move.Input.Y);
		Writer.Write(Move.Yaw);
		Writer.Write(static_cast<uint8>((Move.bJump ? 1 : 0) | (Move.bRootMotion ? 2 : 0) | (Move.bLaunch ? 4 : 0)));
		if (Move.bRootMotion)
		{
			Writer.Write(Move.RootMotionVelocity.X);
			Writer.Write(Move.RootMotionVelocity.Y);
		}
		if (Move.bLaunch)
		{
			Writer.Write(Move.LaunchVelocity);
			Writer.Write(static_cast<uint8>((Move.bLaunchOverrideXY ? 1 : 0) | (Move.bLaunchOverrideZ ? 2 : 0)));
			Writer.Write(Move.StunSeconds);
		}
	}
	Systems.Net->SendToServer(Writer.GetBuffer(), ENetReliability::Unreliable);
}

void FGameWorld::ReceiveCharacterMoves(FNetConnectionId Connection, const std::vector<uint8>& Message)
{
	if (Mode == ENetMode::Client || Systems.Net == nullptr || Scene == nullptr)
	{
		return;
	}
	const std::vector<FNetDriver::FRemotePlayer>& Players = Systems.Net->GetPlayers();
	const auto Sender = std::find_if(Players.begin(), Players.end(), [Connection](const FNetDriver::FRemotePlayer& Player) { return Player.Connection == Connection; });
	if (Sender == Players.end())
	{
		return;
	}
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32  NetId  = Reader.Read<uint32>();
	const uint8   Count  = Reader.Read<uint8>();
	const FEntity Entity = FindByNetId(*Scene, NetId);
	if (!Reader.IsOk() || !Entity.IsValid() || !Scene->GetRegistry().Has<FCharacterMovementComponent>(Entity) ||
	    GetOwner(Entity) != static_cast<int32>(Sender->PlayerId))
	{
		return; // 없는 캐릭터 / 남의 캐릭터
	}
	FServerCharacter& Server = ServerCharacters[Entity];
	for (uint8 Index = 0; Index < Count; ++Index)
	{
		FCharacterMove Move;
		Move.Sequence     = Reader.Read<uint32>();
		Move.DeltaSeconds = Reader.Read<float>();
		Move.Input.X      = Reader.Read<float>();
		Move.Input.Y      = Reader.Read<float>();
		Move.Yaw          = Reader.Read<float>();
		const uint8 Flags = Reader.Read<uint8>();
		Move.bJump        = (Flags & 1) != 0;
		Move.bRootMotion  = (Flags & 2) != 0;
		if (Move.bRootMotion)
		{
			Move.RootMotionVelocity.X = Reader.Read<float>();
			Move.RootMotionVelocity.Y = Reader.Read<float>();
			Move.RootMotionVelocity   = CharacterMovementMath::ClampRootMotionVelocity(Move.RootMotionVelocity);
		}
		Move.bLaunch = (Flags & 4) != 0;
		if (Move.bLaunch)
		{
			Move.LaunchVelocity      = Reader.Read<FVector3>();
			const uint8 Overrides    = Reader.Read<uint8>();
			Move.bLaunchOverrideXY   = (Overrides & 1) != 0;
			Move.bLaunchOverrideZ    = (Overrides & 2) != 0;
			Move.StunSeconds         = Reader.Read<float>();
		}
		if (!Reader.IsOk() || !IsFiniteMove(Move) || !CharacterMovementMath::SanitizeLaunch(Move))
		{
			return;
		}
		if (Move.Sequence <= Server.LastQueued)
		{
			continue; // 이미 받은 무브 (겹쳐 보낸 것)
		}
		Move.DeltaSeconds = std::clamp(Move.DeltaSeconds, 0.0f, FCharacterMove::MaxMoveDeltaSeconds);
		Move.Input        = CharacterMovementMath::ClampInput(Move.Input);
		Server.LastQueued = Move.Sequence;
		Server.Queue.push_back(Move);
	}
	if (Server.Queue.size() > MaxQueuedMoves)
	{
		Server.Queue.erase(Server.Queue.begin(), Server.Queue.end() - static_cast<std::ptrdiff_t>(MaxQueuedMoves));
	}
}

void FGameWorld::SendCharacterAck(FEntity Entity, uint32 Sequence)
{
	if (Systems.Net == nullptr || Systems.Physics == nullptr)
	{
		return;
	}
	const int32                                   Owner   = GetOwner(Entity);
	const std::vector<FNetDriver::FRemotePlayer>& Players = Systems.Net->GetPlayers();
	const auto Target = std::find_if(Players.begin(), Players.end(), [Owner](const FNetDriver::FRemotePlayer& Player) { return static_cast<int32>(Player.PlayerId) == Owner; });
	const uint32 NetId = NetReplication::GetNetId(*Scene, Entity);
	if (Target == Players.end() || NetId == InvalidNetId)
	{
		return;
	}
	const FCharacterState State = Systems.Physics->GetCharacterState(Entity);
	FBinaryWriter         Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::CharacterAck));
	Writer.Write(NetId);
	Writer.Write(Sequence);
	Writer.Write(State.Position);
	Writer.Write(State.Velocity);
	Writer.Write(static_cast<uint8>(State.bGrounded ? 1 : 0));
	Writer.Write(State.StunTimer);
	Systems.Net->Send(Target->Connection, Writer.GetBuffer(), ENetReliability::Unreliable);
}

void FGameWorld::ReceiveCharacterAck(const std::vector<uint8>& Message)
{
	if (Mode != ENetMode::Client || Scene == nullptr || Systems.Physics == nullptr)
	{
		return;
	}
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32    NetId    = Reader.Read<uint32>();
	const uint32    Sequence = Reader.Read<uint32>();
	FCharacterState State;
	State.Position  = Reader.Read<FVector3>();
	State.Velocity  = Reader.Read<FVector3>();
	State.bGrounded = Reader.Read<uint8>() != 0;
	State.StunTimer = Reader.Read<float>();
	const FEntity Entity = FindByNetId(*Scene, NetId);
	const auto    Found  = PredictedCharacters.find(Entity);
	if (!Reader.IsOk() || !std::isfinite(State.StunTimer) || !Entity.IsValid() || Found == PredictedCharacters.end() ||
	    Sequence <= Found->second.LastAckSequence)
	{
		return; // 늦게 온(순서가 바뀐) ack
	}
	FPredictedCharacter& Predicted = Found->second;
	Predicted.LastAckSequence      = Sequence;
	while (!Predicted.Moves.empty() && Predicted.Moves.front().Sequence <= Sequence)
	{
		Predicted.Moves.pop_front();
	}
	if (!UsesClientPrediction(Entity))
	{
		return; // 예측 끔: 위치는 스냅샷 보간이 맡는다
	}
	// 물리 예측: 이 ack 무브를 시뮬레이션한 시각 (같은 서버 프레임의 스냅샷 상태 ↔ 로컬 기록을 맞춘다)
	while (!Predicted.MoveTimes.empty() && Predicted.MoveTimes.front().first < Sequence)
	{
		Predicted.MoveTimes.pop_front();
	}
	if (!Predicted.MoveTimes.empty() && Predicted.MoveTimes.front().first == Sequence)
	{
		LastAckMoveTime = Predicted.MoveTimes.front().second;
	}

	// 서버 상태에서 남은 무브를 다시 적용 → 지금 예측한 위치와 비교.
	// 다시 적용하는 동안 캐릭터는 동적 바디를 밀지 않는다 (그 무브로 이미 밀었다 — 물리 예측 바디는 재시뮬레이션하지 않는다, GameWorldPhysicsPrediction.cpp)
	const FVector3 Before = Systems.Physics->GetCharacterState(Entity).Position;
	Systems.Physics->SetCharactersPushBodies(false);
	const auto Replay = [&](bool bPoseBodies) {
		Systems.Physics->SetCharacterState(*Scene, Entity, State);
		size_t TimeIndex = 0;
		for (const FCharacterMove& Move : Predicted.Moves)
		{
			while (bPoseBodies && TimeIndex < Predicted.MoveTimes.size() && Predicted.MoveTimes[TimeIndex].first < Move.Sequence)
			{
				++TimeIndex;
			}
			if (bPoseBodies && TimeIndex < Predicted.MoveTimes.size() && Predicted.MoveTimes[TimeIndex].first == Move.Sequence)
			{
				PoseBodiesForReplay(Predicted.MoveTimes[TimeIndex].second);
			}
			Systems.Physics->SimulateCharacter(*Scene, Entity, Move);
		}
		if (bPoseBodies)
		{
			RestoreBodiesAfterReplay();
		}
		return Systems.Physics->GetCharacterState(Entity);
	};
	// 물리 예측 바디가 있으면 두 가지로 다시 적용해 지금 예측에 가까운 쪽을 쓴다 (둘 다 서버 상태에서 시작 — 서버만 아는 일은 둘 다 반영된다):
	//   1) 바디를 지금 자리에 둔 채 — 밀던 바디의 "현재" 면에 막혀 밀기 중에 작은 보정(1~8cm)이 ack마다 생긴다
	//   2) 무브마다 바디를 그 무브를 처음 시뮬레이션할 때의 기록 위치로 잠시 옮겨 (충돌 상대만 그때 자리에) — 상자 밀기는 원래 결과와 거의 같지만,
	//      서버와 갈린 경우(가벼운 공에 올라탐 등) 지금 바디와 겹친 위치로 끝나 공을 튕겨 낼 수 있다
	//   겹친 결과(바디를 되돌린 뒤 캡슐이 동적 바디에 MaxReplayPenetration 넘게 묻힘)는 쓰지 않는다
	FCharacterState Replayed = Replay(false);
	if (!PredictedBodies.empty())
	{
		const FCharacterState Posed = Replay(true);
		Systems.Physics->SetCharacterState(*Scene, Entity, Posed); // 바디가 지금 자리에 돌아온 상태로 접촉 다시 계산
		const bool bPosedClear = Systems.Physics->GetCharacterDynamicPenetration(Entity) <= MaxReplayPenetration;
		if (!bPosedClear || FVector3::DistanceSquared(Posed.Position, Before) >= FVector3::DistanceSquared(Replayed.Position, Before))
		{
			Systems.Physics->SetCharacterState(*Scene, Entity, Replayed);
		}
		else
		{
			Replayed = Posed;
		}
	}
	Systems.Physics->SetCharactersPushBodies(true);
	const FVector3 After = Replayed.Position;
	if (FVector3::DistanceSquared(Before, After) > 1.0f)
	{
		++CharacterCorrections;
		PredictionStats.CorrectionMax = std::max(PredictionStats.CorrectionMax, FVector3::Distance(Before, After));
		PredictionStats.BigCorrections += FVector3::DistanceSquared(Before, After) > 25.0f ? 1u : 0u;
		E_LOG(LogNet, Verbose, "캐릭터 재조정: {:.1f}cm (순번 {})", FVector3::Distance(Before, After), Sequence);
		// 화면은 이전 위치에서 시작해 새 위치로 천천히 (큰 차이는 순간이동으로 보고 바로)
		Predicted.VisualOffset = Predicted.VisualOffset + (Before - After);
		if (Predicted.VisualOffset.Length() > SnapCorrectionDistance)
		{
			Predicted.VisualOffset = FVector3();
		}
		Systems.Physics->SetCharacterVisualOffset(*Scene, Entity, Predicted.VisualOffset);
	}
}
