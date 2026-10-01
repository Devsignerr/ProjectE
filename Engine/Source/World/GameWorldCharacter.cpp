#include "World/GameWorld.h"

#include "Core/Log.h"
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
//     같은 무브 → 같은 결과라 보통 차이가 없고, 서버만 아는 일(다른 캐릭터와 부딪힘, 순간이동)이 있었을 때만 위치가 바뀐다.
//   메시지 (비신뢰):
//     CharacterMoves: uint32 NetId, uint8 개수, [uint32 순번, float dt, float 입력 X, float 입력 Y, float yaw, uint8 점프]...
//     CharacterAck:   uint32 NetId, uint32 순번, FVector3 위치, FVector3 속도, uint8 바닥
//   서버는 무브 dt를 FCharacterMove::MaxMoveDeltaSeconds로 자르고, 소유자가 아닌 연결이 보낸 무브는 버린다.

namespace
{
	constexpr uint8  MaxMovesPerPacket  = 8;   // 겹쳐 보내는 최근 무브 수 (손실 대비)
	constexpr size_t MaxPredictedMoves  = 240; // 확인 안 된 무브 기록 상한 (4초 — 서버 응답이 끊기면 오래된 것부터 버린다)
	constexpr size_t MaxQueuedMoves     = 120; // 서버가 한 캐릭터에 쌓아 두는 무브 상한

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

bool FGameWorld::IsPredicted(FEntity Entity) const
{
	return Mode == ENetMode::Client && Scene != nullptr && Scene->GetRegistry().IsValid(Entity) &&
	       Scene->GetRegistry().Has<FCharacterMovementComponent>(Entity) && GetOwner(Entity) >= 0 && IsLocallyControlled(Entity);
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
	for (const FEntity Entity : Characters)
	{
		const int32 Owner = GetOwner(Entity);
		if (IsLocallyControlled(Entity))
		{
			// 플레이어 캐릭터는 시점 방향(yaw)을 보고, 서버 소유(AI 등)는 이동 방향을 본다
			const float    Yaw  = LocalControlRotation.X;
			FCharacterMove Move = Physics->ConsumePendingMove(Entity, DeltaSeconds, Owner >= 0 ? &Yaw : nullptr);
			if (Mode == ENetMode::Client)
			{
				FPredictedCharacter& Predicted = PredictedCharacters[Entity];
				Move.Sequence                  = ++Predicted.NextSequence;
				Physics->SimulateCharacter(*Scene, Entity, Move); // 예측: 바로 움직인다
				Predicted.Moves.push_back(Move);
				while (Predicted.Moves.size() > MaxPredictedMoves)
				{
					Predicted.Moves.pop_front();
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
			Physics->FollowTransform(*Scene, Entity); // 다른 플레이어: 복제 보간 위치에 캡슐만 맞춘다
		}
		else if (Owner >= 0)
		{
			// 서버: 원격 플레이어 캐릭터는 받은 무브만큼 움직인다 (무브가 없으면 그 자리 — 클라이언트가 시간을 정한다)
			FServerCharacter& Server = ServerCharacters[Entity];
			if (Server.Queue.empty())
			{
				continue;
			}
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
		Writer.Write(static_cast<uint8>(Move.bJump ? 1 : 0));
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
		Move.bJump        = Reader.Read<uint8>() != 0;
		if (!Reader.IsOk() || !IsFiniteMove(Move))
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
	const FEntity Entity = FindByNetId(*Scene, NetId);
	const auto    Found  = PredictedCharacters.find(Entity);
	if (!Reader.IsOk() || !Entity.IsValid() || Found == PredictedCharacters.end() || Sequence <= Found->second.LastAckSequence)
	{
		return; // 늦게 온(순서가 바뀐) ack
	}
	FPredictedCharacter& Predicted = Found->second;
	Predicted.LastAckSequence      = Sequence;
	while (!Predicted.Moves.empty() && Predicted.Moves.front().Sequence <= Sequence)
	{
		Predicted.Moves.pop_front();
	}

	// 서버 상태에서 남은 무브를 다시 적용 → 지금 예측한 위치와 비교
	const FVector3 Before = Systems.Physics->GetCharacterState(Entity).Position;
	Systems.Physics->SetCharacterState(*Scene, Entity, State);
	for (const FCharacterMove& Move : Predicted.Moves)
	{
		Systems.Physics->SimulateCharacter(*Scene, Entity, Move);
	}
	const FVector3 After = Systems.Physics->GetCharacterState(Entity).Position;
	if (FVector3::DistanceSquared(Before, After) > 1.0f)
	{
		++CharacterCorrections;
		E_LOG(LogNet, Verbose, "캐릭터 재조정: {:.1f}cm (순번 {})", FVector3::Distance(Before, After), Sequence);
	}
}
