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
//     다시 적용하는 동안 캐릭터는 동적 바디를 밀지 않는다 (그 무브로 이미 밀었다). 물리 예측 바디는 되감지 않는다 — GameWorldPhysicsPrediction.cpp 머리 주석
//     ack 무브의 로컬 시각(MoveTimes)은 물리 예측이 스냅샷 시각을 로컬 기록에 맞추는 데 쓴다.
//   예측 옵션 (UsesClientPrediction = 컴포넌트 bClientPrediction && 프로젝트 설정 네트워크 → 클라이언트 예측): 끄면 소유 클라이언트는
//     무브를 보내기만 하고 미리 움직이지 않으며, 자기 캐릭터도 스냅샷 보간으로 보여 준다 (IsPredicted = false). 서버 쪽은 같다.
//   메시지 (비신뢰):
//     CharacterMoves: uint32 NetId, uint8 개수, [uint32 순번, float dt, float 입력 X, float 입력 Y, float yaw, uint8 점프]...
//     CharacterAck:   uint32 NetId, uint32 순번, FVector3 위치, FVector3 속도, uint8 바닥
//   서버는 무브 dt를 FCharacterMove::MaxMoveDeltaSeconds로 자르고, 소유자가 아닌 연결이 보낸 무브는 버린다.

namespace
{
	constexpr uint8  MaxMovesPerPacket  = 8;   // 겹쳐 보내는 최근 무브 수 (손실 대비)
	constexpr size_t MaxPredictedMoves  = 240; // 확인 안 된 무브 기록 상한 (4초 — 서버 응답이 끊기면 오래된 것부터 버린다)
	constexpr size_t MaxQueuedMoves     = 120; // 서버가 한 캐릭터에 쌓아 두는 무브 상한
	constexpr float  CorrectionSmoothingSeconds = 0.1f;   // 보정 오프셋이 1/e로 줄어드는 시간
	constexpr float  SnapCorrectionDistance     = 150.0f; // cm, 이보다 큰 보정은 부드럽게 하지 않는다

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
	const FCharacterMovementComponent* Movement = Scene != nullptr ? Scene->GetRegistry().TryGet<FCharacterMovementComponent>(Entity) : nullptr;
	return Movement != nullptr && Movement->bClientPrediction && FProjectSettings::Get().Network.bClientPrediction;
}

bool FGameWorld::IsPredicted(FEntity Entity) const
{
	if (IsPhysicsPredicted(Entity))
	{
		return true; // 물리 예측 바디 (해제 블렌드 중에도 화면은 물리 예측이 맡는다)
	}
	return Mode == ENetMode::Client && Scene != nullptr && Scene->GetRegistry().IsValid(Entity) &&
	       Scene->GetRegistry().Has<FCharacterMovementComponent>(Entity) && GetOwner(Entity) >= 0 && IsLocallyControlled(Entity) &&
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
	// 다시 적용하는 동안 캐릭터는 동적 바디를 밀지 않는다 (그 무브로 이미 밀었다 — 물리 예측 바디는 되감지 않는다, GameWorldPhysicsPrediction.cpp)
	const FVector3 Before = Systems.Physics->GetCharacterState(Entity).Position;
	Systems.Physics->SetCharactersPushBodies(false);
	Systems.Physics->SetCharacterState(*Scene, Entity, State);
	for (const FCharacterMove& Move : Predicted.Moves)
	{
		Systems.Physics->SimulateCharacter(*Scene, Entity, Move);
	}
	Systems.Physics->SetCharactersPushBodies(true);
	const FVector3 After = Systems.Physics->GetCharacterState(Entity).Position;
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
