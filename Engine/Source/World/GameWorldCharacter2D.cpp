#include "World/GameWorld.h"

#include "Core/Log.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <cmath>

// 2D 캐릭터 이동 + 클라이언트 예측 (FCharacterMovement2DComponent — 이동 규칙은 Physics/CharacterMovement2D.h).
// 3D 캐릭터(World/GameWorldCharacter.cpp 머리 주석)와 같은 구조의 병렬 경로다 (무브·상태 형식이 달라 메시지를 나눴다):
//   조종 주체(IsLocallyControlled) + 서버/Standalone: 무브를 만들어 바로 시뮬레이션
//   조종 주체 + 클라이언트(예측): 순번을 붙여 바로 시뮬레이션·기록하고 최근 무브 MaxMovesPerPacket개를 겹쳐 서버로 보낸다
//   서버의 원격 플레이어 캐릭터: 받은 무브를 순번 순서대로 시뮬레이션하고 소유자에게 ack(마지막 순번 + 상태 전체)
//   클라이언트의 다른 캐릭터: 복제 스냅샷 보간 위치에 대리 바디만 맞춘다 (FollowTransform)
//   재조정(ReceiveCharacterAck2D): ack 상태로 되돌린 뒤 순번이 더 큰 기록 무브를 다시 적용 (이벤트는 내지 않음 — SetRecordEvents(false)).
//     위치가 1cm 넘게 바뀌면 차이를 화면 오프셋으로 옮겨 CorrectionSmoothingSeconds에 걸쳐 줄이고, SnapCorrectionDistance보다 크면 바로 옮긴다.
//     2D는 물리 예측(Phase 28식 동적 바디 로컬 시뮬레이션)이 없다 — 서버만 아는 일(움직이는 발판의 지금 위치와 다른 과거 위치, 서버 순간이동,
//     서버가 시뮬레이션한 동적 바디 위에 섬)은 이 재조정으로 맞춘다
//   예측 옵션(UsesClientPrediction = 컴포넌트 bClientPrediction && 프로젝트 설정): 끄면 보내기만 하고 스냅샷 보간으로 보여 준다
//   메시지 (비신뢰):
//     CharacterMoves2D: uint32 NetId, uint8 개수, [uint32 순번, float dt, float 입력 X, float 입력 Y,
//                       uint8 플래그(1 점프 누름, 2 점프 누르고 있음, 4 대시, 8 내려가기) (+ 대시면 float 방향 X, float 방향 Y)]...
//     CharacterAck2D:   uint32 NetId, uint32 순번, FCharacterState2D (WriteState 순서)
//   서버는 dt를 MaxMoveDeltaSeconds로 자르고 입력을 길이 1로 자르며, 소유자가 아닌 연결의 무브는 버린다.
// 이벤트(점프/착지/대시 시작)는 시뮬레이션한 쪽에서만 난다: 서버/Standalone(서버 스크립트·게임 모듈), 예측하는 소유 클라이언트(그 클라이언트의
//   ClientOnly/Both 스크립트). 다른 클라이언트에서 보이는 캐릭터는 내지 않는다.

namespace
{
	constexpr uint8  MaxMovesPerPacket          = 8;
	constexpr size_t MaxPredictedMoves          = 240;
	constexpr size_t MaxQueuedMoves             = 120;
	constexpr float  CorrectionSmoothingSeconds = 0.1f;
	constexpr float  SnapCorrectionDistance     = 150.0f;

	enum EMoveFlags : uint8
	{
		MoveFlag_JumpPressed = 1,
		MoveFlag_JumpHeld    = 2,
		MoveFlag_Dash        = 4,
		MoveFlag_DropDown    = 8,
	};

	bool IsFinite(const FVector2& V) { return std::isfinite(V.X) && std::isfinite(V.Y); }

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

	void WriteState(FBinaryWriter& Writer, const FCharacterState2D& State)
	{
		Writer.Write(State.Position);
		Writer.Write(State.Velocity);
		Writer.Write(static_cast<uint8>((State.bGrounded ? 1 : 0) | (State.bJumpCutAvailable ? 2 : 0)));
		Writer.Write(State.JumpsUsed);
		Writer.Write(State.AirDashesUsed);
		Writer.Write(State.CoyoteTimer);
		Writer.Write(State.JumpBufferTimer);
		Writer.Write(State.DashTimer);
		Writer.Write(State.DashCooldownTimer);
		Writer.Write(State.DropTimer);
		Writer.Write(State.DashDirection);
	}

	FCharacterState2D ReadState(FBinaryReader& Reader)
	{
		FCharacterState2D State;
		State.Position          = Reader.Read<FVector2>();
		State.Velocity          = Reader.Read<FVector2>();
		const uint8 Flags       = Reader.Read<uint8>();
		State.bGrounded         = (Flags & 1) != 0;
		State.bJumpCutAvailable = (Flags & 2) != 0;
		State.JumpsUsed         = Reader.Read<uint8>();
		State.AirDashesUsed     = Reader.Read<uint8>();
		State.CoyoteTimer       = Reader.Read<float>();
		State.JumpBufferTimer   = Reader.Read<float>();
		State.DashTimer         = Reader.Read<float>();
		State.DashCooldownTimer = Reader.Read<float>();
		State.DropTimer         = Reader.Read<float>();
		State.DashDirection     = Reader.Read<FVector2>();
		return State;
	}

	bool IsFiniteState(const FCharacterState2D& State)
	{
		return IsFinite(State.Position) && IsFinite(State.Velocity) && IsFinite(State.DashDirection) && std::isfinite(State.CoyoteTimer) &&
		       std::isfinite(State.JumpBufferTimer) && std::isfinite(State.DashTimer) && std::isfinite(State.DashCooldownTimer) &&
		       std::isfinite(State.DropTimer);
	}
} // namespace

void FGameWorld::TickCharacters2D(float DeltaSeconds)
{
	if (Scene == nullptr || !Characters2D->IsActive())
	{
		return;
	}
	Characters2D->Sync(*Scene);
	std::vector<FEntity> Characters;
	Scene->GetRegistry().View<FCharacterMovement2DComponent>().Each([&](FEntity Entity, FCharacterMovement2DComponent&) {
		if (Characters2D->HasCharacter(Entity))
		{
			Characters.push_back(Entity);
		}
	});
	for (const FEntity Entity : Characters)
	{
		const int32      Owner = GetOwner(Entity);
		FCharacterMove2D Move  = Characters2D->ConsumePendingMove(Entity, DeltaSeconds); // 조종하지 않는 쪽 입력은 버린다
		if (IsLocallyControlled(Entity))
		{
			if (Mode != ENetMode::Client)
			{
				Characters2D->SimulateCharacter(*Scene, Entity, Move);
				continue;
			}
			FPredictedCharacter2D& Predicted = PredictedCharacters2D[Entity];
			Move.Sequence                    = ++Predicted.NextSequence;
			if (UsesClientPrediction(Entity))
			{
				Predicted.VisualOffset = Predicted.VisualOffset * std::exp(-DeltaSeconds / CorrectionSmoothingSeconds);
				if (Predicted.VisualOffset.LengthSquared() < 0.01f)
				{
					Predicted.VisualOffset = FVector2();
				}
				Characters2D->SetVisualOffset(*Scene, Entity, Predicted.VisualOffset);
				Characters2D->SimulateCharacter(*Scene, Entity, Move); // 예측: 바로 움직인다
			}
			else
			{
				Characters2D->FollowTransform(*Scene, Entity); // 예측 끔: 보내기만 하고 화면은 스냅샷 보간
			}
			Predicted.Moves.push_back(Move);
			while (Predicted.Moves.size() > MaxPredictedMoves)
			{
				Predicted.Moves.pop_front();
			}
			SendCharacterMoves2D(Entity);
		}
		else if (Mode == ENetMode::Client)
		{
			Characters2D->FollowTransform(*Scene, Entity);
		}
		else if (Owner >= 0)
		{
			FServerCharacter2D& Server = ServerCharacters2D[Entity];
			if (Server.Queue.empty())
			{
				continue; // 무브가 없으면 그 자리 (클라이언트가 시간을 정한다)
			}
			for (const FCharacterMove2D& Queued : Server.Queue)
			{
				Characters2D->SimulateCharacter(*Scene, Entity, Queued);
				Server.LastApplied = Queued.Sequence;
			}
			Server.Queue.clear();
			SendCharacterAck2D(Entity, Server.LastApplied);
		}
	}
}

void FGameWorld::SendCharacterMoves2D(FEntity Entity)
{
	if (Systems.Net == nullptr || Systems.Net->GetClientState() != FNetDriver::EClientState::Joined)
	{
		return;
	}
	const uint32 NetId = NetReplication::GetNetId(*Scene, Entity);
	const auto   Found = PredictedCharacters2D.find(Entity);
	if (NetId == InvalidNetId || Found == PredictedCharacters2D.end() || Found->second.Moves.empty())
	{
		return;
	}
	const std::deque<FCharacterMove2D>& Moves = Found->second.Moves;
	const size_t                        Count = std::min<size_t>(Moves.size(), MaxMovesPerPacket);
	FBinaryWriter Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::CharacterMoves2D));
	Writer.Write(NetId);
	Writer.Write(static_cast<uint8>(Count));
	for (size_t Index = Moves.size() - Count; Index < Moves.size(); ++Index)
	{
		const FCharacterMove2D& Move = Moves[Index];
		Writer.Write(Move.Sequence);
		Writer.Write(Move.DeltaSeconds);
		Writer.Write(Move.Input.X);
		Writer.Write(Move.Input.Y);
		Writer.Write(static_cast<uint8>((Move.bJumpPressed ? MoveFlag_JumpPressed : 0) | (Move.bJumpHeld ? MoveFlag_JumpHeld : 0) |
		                                (Move.bDash ? MoveFlag_Dash : 0) | (Move.bDropDown ? MoveFlag_DropDown : 0)));
		if (Move.bDash)
		{
			Writer.Write(Move.DashDirection.X);
			Writer.Write(Move.DashDirection.Y);
		}
	}
	Systems.Net->SendToServer(Writer.GetBuffer(), ENetReliability::Unreliable);
}

void FGameWorld::ReceiveCharacterMoves2D(FNetConnectionId Connection, const std::vector<uint8>& Message)
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
	const uint32                         NetId    = Reader.Read<uint32>();
	const uint8                          Count    = Reader.Read<uint8>();
	const FEntity                        Entity   = FindByNetId(*Scene, NetId);
	const FCharacterMovement2DComponent* Movement = Entity.IsValid() ? Scene->GetRegistry().TryGet<FCharacterMovement2DComponent>(Entity) : nullptr;
	if (!Reader.IsOk() || Movement == nullptr || GetOwner(Entity) != static_cast<int32>(Sender->PlayerId))
	{
		return; // 없는 캐릭터 / 남의 캐릭터
	}
	FServerCharacter2D& Server = ServerCharacters2D[Entity];
	for (uint8 Index = 0; Index < Count; ++Index)
	{
		FCharacterMove2D Move;
		Move.Sequence     = Reader.Read<uint32>();
		Move.DeltaSeconds = Reader.Read<float>();
		Move.Input.X      = Reader.Read<float>();
		Move.Input.Y      = Reader.Read<float>();
		const uint8 Flags = Reader.Read<uint8>();
		Move.bJumpPressed = (Flags & MoveFlag_JumpPressed) != 0;
		Move.bJumpHeld    = (Flags & MoveFlag_JumpHeld) != 0;
		Move.bDash        = (Flags & MoveFlag_Dash) != 0;
		Move.bDropDown    = (Flags & MoveFlag_DropDown) != 0;
		if (Move.bDash)
		{
			Move.DashDirection.X = Reader.Read<float>();
			Move.DashDirection.Y = Reader.Read<float>();
		}
		if (!Reader.IsOk() || !std::isfinite(Move.DeltaSeconds) || !IsFinite(Move.Input) || !IsFinite(Move.DashDirection))
		{
			return;
		}
		if (Move.Sequence <= Server.LastQueued)
		{
			continue; // 이미 받은 무브 (겹쳐 보낸 것)
		}
		Move.DeltaSeconds = std::clamp(Move.DeltaSeconds, 0.0f, FCharacterMove2D::MaxMoveDeltaSeconds);
		Move.Input        = CharacterMovement2DMath::ClampInput(Move.Input, Movement->Mode);
		Server.LastQueued = Move.Sequence;
		Server.Queue.push_back(Move);
	}
	if (Server.Queue.size() > MaxQueuedMoves)
	{
		Server.Queue.erase(Server.Queue.begin(), Server.Queue.end() - static_cast<std::ptrdiff_t>(MaxQueuedMoves));
	}
}

void FGameWorld::SendCharacterAck2D(FEntity Entity, uint32 Sequence)
{
	if (Systems.Net == nullptr)
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
	FBinaryWriter Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::CharacterAck2D));
	Writer.Write(NetId);
	Writer.Write(Sequence);
	WriteState(Writer, Characters2D->GetState(Entity));
	Systems.Net->Send(Target->Connection, Writer.GetBuffer(), ENetReliability::Unreliable);
}

void FGameWorld::ReceiveCharacterAck2D(const std::vector<uint8>& Message)
{
	if (Mode != ENetMode::Client || Scene == nullptr)
	{
		return;
	}
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32            NetId    = Reader.Read<uint32>();
	const uint32            Sequence = Reader.Read<uint32>();
	const FCharacterState2D State    = ReadState(Reader);
	const FEntity           Entity   = FindByNetId(*Scene, NetId);
	const auto              Found    = PredictedCharacters2D.find(Entity);
	if (!Reader.IsOk() || !IsFiniteState(State) || !Entity.IsValid() || Found == PredictedCharacters2D.end() ||
	    Sequence <= Found->second.LastAckSequence)
	{
		return; // 늦게 온(순서가 바뀐) ack
	}
	FPredictedCharacter2D& Predicted = Found->second;
	Predicted.LastAckSequence        = Sequence;
	while (!Predicted.Moves.empty() && Predicted.Moves.front().Sequence <= Sequence)
	{
		Predicted.Moves.pop_front();
	}
	if (!UsesClientPrediction(Entity) || !Characters2D->HasCharacter(Entity))
	{
		return; // 예측 끔: 위치는 스냅샷 보간이 맡는다
	}
	// 서버 상태에서 남은 무브를 다시 적용 → 지금 예측한 위치와 비교 (같은 무브 → 같은 결과라 보통 차이 없음)
	const FVector2 Before = Characters2D->GetState(Entity).Position;
	Characters2D->SetRecordEvents(false);
	Characters2D->SetState(*Scene, Entity, State);
	for (const FCharacterMove2D& Move : Predicted.Moves)
	{
		Characters2D->SimulateCharacter(*Scene, Entity, Move);
	}
	Characters2D->SetRecordEvents(true);
	const FVector2 After = Characters2D->GetState(Entity).Position;
	if (FVector2::Dot(After - Before, After - Before) > 1.0f)
	{
		++CharacterCorrections;
		const float Distance = (After - Before).Length();
		E_LOG(LogNet, Verbose, "2D 캐릭터 재조정: {:.1f}cm (순번 {})", Distance, Sequence);
		Predicted.VisualOffset = Predicted.VisualOffset + (Before - After);
		if (Predicted.VisualOffset.Length() > SnapCorrectionDistance)
		{
			Predicted.VisualOffset = FVector2();
		}
	}
	Characters2D->SetVisualOffset(*Scene, Entity, Predicted.VisualOffset);
}

bool FGameWorld::DispatchCharacter2DEvents()
{
	std::vector<FCharacterMovement2DSystem::FEvent> Events;
	Characters2D->ConsumeEvents(Events);
	if (Events.empty() || Scene == nullptr)
	{
		return false;
	}
	const FRegistry& Registry = Scene->GetRegistry();
	for (const FCharacterMovement2DSystem::FEvent& Event : Events)
	{
		if (!Registry.IsValid(Event.Entity))
		{
			continue;
		}
		if (Event.Events.bDashStarted)
		{
			Systems.Scripts->InvokeMethod(Event.Entity, "OnDashStarted", {});
		}
		if (Event.Events.bJumped && Registry.IsValid(Event.Entity))
		{
			Systems.Scripts->InvokeMethod(Event.Entity, "OnJumped", { FGameRpcValue::MakeNumber(static_cast<double>(Event.Events.JumpIndex), true) });
		}
		if (Event.Events.bLanded && Registry.IsValid(Event.Entity))
		{
			Systems.Scripts->InvokeMethod(Event.Entity, "OnLanded", {});
		}
		if (Mode != ENetMode::Client && Systems.GameModule != nullptr && Registry.IsValid(Event.Entity))
		{
			Systems.GameModule->Character2DEvent(*Scene, Event.Entity, Event.Events);
		}
	}
	return true;
}
