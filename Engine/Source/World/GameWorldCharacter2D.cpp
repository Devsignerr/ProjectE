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
//     서버만 아는 일(움직이는 발판의 지금 위치와 다른 과거 위치, 서버 순간이동)은 이 재조정으로 맞춘다. 근처 복제 2D 동적 바디는
//     물리 예측(GameWorldPhysicsPrediction2D.cpp)이 로컬에서 시뮬레이션하고, 다시 적용할 때 그 바디를 기록 위치에 잠시 두는 경로도 함께 본다.
//     ack 무브의 로컬 시각(MoveTimes)은 물리 예측이 스냅샷 시각을 로컬 기록에 맞추는 데 쓴다 (3D와 같은 시계)
//   예측 옵션(UsesClientPrediction = 컴포넌트 bClientPrediction && 프로젝트 설정): 끄면 보내기만 하고 스냅샷 보간으로 보여 준다
//   넉백/발사 (entity:LaunchCharacter/AddKnockback — 규칙은 CharacterMovement2D.h): 엔티티에 쌓였다가 다음 무브에 실린다 (무브 필드라 같은 무브 → 같은 결과).
//     조종하는 쪽(Standalone, 서버 소유를 서버가, 예측하는 소유 클라이언트): 자기 다음 무브 — 클라이언트면 무브와 함께 서버로 간다 (서버는 성분을 MaxLaunchSpeed로 자름)
//     서버의 원격 플레이어 캐릭터: 서버가 준 넉백은 다음에 적용하는 받은 무브(큐 첫 무브)에 합친다 — 받은 무브가 없으면 올 때까지 기다린다.
//       결과(속도·경직 타이머)는 ack 상태로 소유 클라이언트에 가고, 클라이언트는 재조정(ack 상태 + 남은 무브 다시 적용)으로 같은 궤적을 받는다
//       (넉백을 실은 무브까지는 클라이언트가 넉백 없이 예측했으므로 그만큼 보정이 생긴다 — 화면 오프셋으로 흡수)
//     클라이언트의 다른 캐릭터: 무시 (서버 결과를 복제로 받는다). 같은 넉백을 서버와 소유 클라이언트가 둘 다 부르면 두 번 실리므로 한쪽(보통 서버)에서만
//     시간 정지(게임 시간 배율 0 — Standalone 전용)인 틱은 무브를 시뮬레이션하지 않는다: 쌓인 입력은 버리고 넉백은 다음 무브까지 남긴다
//   메시지 (비신뢰):
//     CharacterMoves2D: uint32 NetId, uint8 개수, [uint32 순번, float dt, float 입력 X, float 입력 Y,
//                       uint8 플래그(1 점프 누름, 2 점프 누르고 있음, 4 대시, 8 내려가기, 16 발사) (+ 대시면 float 방향 X, float 방향 Y)
//                       (+ 발사면 float 속도 X, float 속도 Y, uint8 덮어쓰기(1 X, 2 Y), float 경직 초)]...
//     CharacterAck2D:   uint32 NetId, uint32 순번, FCharacterState2D (WriteState 순서)
//   서버는 dt를 MaxMoveDeltaSeconds로 자르고 입력을 길이 1로 자르며, 소유자가 아닌 연결의 무브는 버린다.
// 이벤트(점프/착지/대시 시작/밟기 — 밟기는 착지 뒤 OnStomped(other) + 밟힌 쪽 OnStompedBy(other))는 시뮬레이션한 쪽에서만 난다: 서버/Standalone(서버 스크립트·게임 모듈), 예측하는 소유 클라이언트(그 클라이언트의
//   ClientOnly/Both 스크립트). 다른 클라이언트에서 보이는 캐릭터는 내지 않는다.

namespace
{
	constexpr uint8  MaxMovesPerPacket          = 8;
	constexpr size_t MaxPredictedMoves          = 240;
	constexpr size_t MaxQueuedMoves             = 120;
	constexpr float  MaxReplayPenetration2D     = 1.0f; // cm, 3D MaxReplayPenetration과 같은 값
	constexpr float  CorrectionSmoothingSeconds = 0.1f;
	constexpr float  SnapCorrectionDistance     = 150.0f;

	enum EMoveFlags : uint8
	{
		MoveFlag_JumpPressed = 1,
		MoveFlag_JumpHeld    = 2,
		MoveFlag_Dash        = 4,
		MoveFlag_DropDown    = 8,
		MoveFlag_Launch      = 16,
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
		Writer.Write(State.StunTimer);
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
		State.StunTimer         = Reader.Read<float>();
		return State;
	}

	bool IsFiniteState(const FCharacterState2D& State)
	{
		return IsFinite(State.Position) && IsFinite(State.Velocity) && IsFinite(State.DashDirection) && std::isfinite(State.CoyoteTimer) &&
		       std::isfinite(State.JumpBufferTimer) && std::isfinite(State.DashTimer) && std::isfinite(State.DashCooldownTimer) &&
		       std::isfinite(State.DropTimer) && std::isfinite(State.StunTimer);
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
	if (DeltaSeconds <= 0.0f)
	{
		// 시간 정지 (게임 시간 배율 0/히트스톱 — Standalone): 무브 없음. 쌓인 입력은 버리고 넉백은 다음 무브까지 남긴다
		for (const FEntity Entity : Characters)
		{
			Characters2D->ConsumePendingMove(Entity, 0.0f);
		}
		return;
	}
	for (const FEntity Entity : Characters)
	{
		const int32      Owner = GetOwner(Entity);
		FCharacterMove2D Move  = Characters2D->ConsumePendingMove(Entity, DeltaSeconds); // 조종하지 않는 쪽 입력은 버린다
		if (IsLocallyControlled(Entity))
		{
			Characters2D->MergePendingLaunch(Entity, Move); // 넉백/발사는 자기 무브에 (클라이언트면 무브와 함께 서버로)
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
				Predicted.MoveTimes.emplace_back(Move.Sequence, PredictionClock); // 물리 예측 시각 맞추기 (결과는 이번 프레임 2D 스텝 뒤에 기록)
				while (Predicted.MoveTimes.size() > MaxPredictedMoves)
				{
					Predicted.MoveTimes.pop_front();
				}
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
			FCharacterMove2D Ignored;
			if (Characters2D->MergePendingLaunch(Entity, Ignored)) // 남의 캐릭터 넉백은 서버가 준다 (복제로 받는다)
			{
				E_LOG(LogNet, Verbose, "2D 캐릭터 넉백 무시: 이 클라이언트가 조종하지 않는 캐릭터 (서버에서 부를 것)");
			}
			Characters2D->FollowTransform(*Scene, Entity);
		}
		else if (Owner >= 0)
		{
			FServerCharacter2D& Server = ServerCharacters2D[Entity];
			if (Server.Queue.empty())
			{
				continue; // 무브가 없으면 그 자리 (클라이언트가 시간을 정한다) — 서버가 준 넉백도 다음 무브까지 기다린다
			}
			Characters2D->MergePendingLaunch(Entity, Server.Queue.front()); // 서버가 준 넉백 → 이번에 적용하는 첫 무브 (결과는 ack 상태로)
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
		                                (Move.bDash ? MoveFlag_Dash : 0) | (Move.bDropDown ? MoveFlag_DropDown : 0) | (Move.bLaunch ? MoveFlag_Launch : 0)));
		if (Move.bDash)
		{
			Writer.Write(Move.DashDirection.X);
			Writer.Write(Move.DashDirection.Y);
		}
		if (Move.bLaunch)
		{
			Writer.Write(Move.LaunchVelocity.X);
			Writer.Write(Move.LaunchVelocity.Y);
			Writer.Write(static_cast<uint8>((Move.bLaunchOverrideX ? 1 : 0) | (Move.bLaunchOverrideY ? 2 : 0)));
			Writer.Write(Move.StunSeconds);
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
		Move.bLaunch      = (Flags & MoveFlag_Launch) != 0;
		if (Move.bDash)
		{
			Move.DashDirection.X = Reader.Read<float>();
			Move.DashDirection.Y = Reader.Read<float>();
		}
		if (Move.bLaunch)
		{
			Move.LaunchVelocity.X = Reader.Read<float>();
			Move.LaunchVelocity.Y = Reader.Read<float>();
			const uint8 Overrides = Reader.Read<uint8>();
			Move.bLaunchOverrideX = (Overrides & 1) != 0;
			Move.bLaunchOverrideY = (Overrides & 2) != 0;
			Move.StunSeconds      = Reader.Read<float>();
		}
		if (!Reader.IsOk() || !std::isfinite(Move.DeltaSeconds) || !IsFinite(Move.Input) || !IsFinite(Move.DashDirection) || !IsFinite(Move.LaunchVelocity) ||
		    !std::isfinite(Move.StunSeconds))
		{
			return;
		}
		Move.LaunchVelocity.X = std::clamp(Move.LaunchVelocity.X, -FCharacterMove2D::MaxLaunchSpeed, FCharacterMove2D::MaxLaunchSpeed);
		Move.LaunchVelocity.Y = std::clamp(Move.LaunchVelocity.Y, -FCharacterMove2D::MaxLaunchSpeed, FCharacterMove2D::MaxLaunchSpeed);
		Move.StunSeconds      = std::clamp(Move.StunSeconds, 0.0f, FCharacterMove2D::MaxStunSeconds);
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
	// 물리 예측: 이 ack 무브를 시뮬레이션한 시각 (3D와 같은 시계 — 스냅샷 상태 ↔ 로컬 기록)
	while (!Predicted.MoveTimes.empty() && Predicted.MoveTimes.front().first < Sequence)
	{
		Predicted.MoveTimes.pop_front();
	}
	if (!Predicted.MoveTimes.empty() && Predicted.MoveTimes.front().first == Sequence)
	{
		LastAckMoveTime = Predicted.MoveTimes.front().second;
	}
	// 서버 상태에서 남은 무브를 다시 적용 → 지금 예측한 위치와 비교 (같은 무브 → 같은 결과라 보통 차이 없음).
	// 2D 물리 예측 바디가 있으면 3D처럼 두 가지로 다시 적용해 지금 예측에 가까운 쪽을 쓴다 (GameWorldPhysicsPrediction2D.cpp 머리 주석):
	//   1) 바디를 지금 자리에 둔 채, 2) 무브마다 바디를 그 무브를 처음 시뮬레이션할 때의 기록 위치로 잠시 옮겨 (바디 재시뮬레이션 없음)
	const FVector2 Before = Characters2D->GetState(Entity).Position;
	Characters2D->SetRecordEvents(false);
	const auto Replay = [&](bool bPoseBodies) {
		Characters2D->SetState(*Scene, Entity, State);
		size_t TimeIndex = 0;
		for (const FCharacterMove2D& Move : Predicted.Moves)
		{
			while (bPoseBodies && TimeIndex < Predicted.MoveTimes.size() && Predicted.MoveTimes[TimeIndex].first < Move.Sequence)
			{
				++TimeIndex;
			}
			if (bPoseBodies && TimeIndex < Predicted.MoveTimes.size() && Predicted.MoveTimes[TimeIndex].first == Move.Sequence)
			{
				PoseBodiesForReplay2D(Predicted.MoveTimes[TimeIndex].second);
			}
			Characters2D->SimulateCharacter(*Scene, Entity, Move);
		}
		if (bPoseBodies)
		{
			RestoreBodiesAfterReplay2D();
		}
		return Characters2D->GetState(Entity);
	};
	//   ②의 결과가 지금 자리로 돌아온 바디에 MaxReplayPenetration2D 넘게 묻히면 쓰지 않는다 (3D와 같은 겹침 거부). 2D 이동기는 동적 바디를 옆으로 막지
	//   않으므로 이동 중에는 겹침이 생겨도 그대로 지나가지만, 결과 자리에 묻힌 채 다음 2D 스텝을 맞으면 키네마틱 대리 캡슐이 그 바디를 무한 질량으로
	//   밀어내 튕겨 낸다 — 기록 때 바디가 있던 자리(예: 떨어지던 상자 위)에 섰는데 바디가 그사이 옆·위로 옮겨 와 지금은 그 자리를 차지한 경우
	FCharacterState2D Replayed = Replay(false);
	if (!PredictedBodies2D.empty())
	{
		const FCharacterState2D Posed = Replay(true);
		Characters2D->SetState(*Scene, Entity, Posed); // 바디가 지금 자리에 돌아온 상태로 겹침 검사
		const bool bPosedClear = Characters2D->GetDynamicPenetration(Entity) <= MaxReplayPenetration2D;
		if (bPosedClear && (Posed.Position - Before).LengthSquared() < (Replayed.Position - Before).LengthSquared())
		{
			Replayed = Posed;
		}
		else if (!bPosedClear)
		{
			++PredictionStats2D.ReplayOverlapRejects;
		}
		Characters2D->SetState(*Scene, Entity, Replayed);
	}
	Characters2D->SetRecordEvents(true);
	const FVector2 After = Replayed.Position;
	if (FVector2::Dot(After - Before, After - Before) > 1.0f)
	{
		++CharacterCorrections;
		const float Distance = (After - Before).Length();
		++PredictionStats2D.Corrections;
		PredictionStats2D.CorrectionMax = std::max(PredictionStats2D.CorrectionMax, Distance);
		PredictionStats2D.BigCorrections += Distance > 5.0f ? 1u : 0u;
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
		if (Event.Events.bStomped && Registry.IsValid(Event.Entity))
		{
			// 밟기 (CharacterCollision Block/Push): 밟은 쪽 OnStomped(other), 밟힌 쪽 OnStompedBy(other) — 착지 뒤
			const FEntity Other = FEntity::FromId(Event.Events.StompedEntity);
			const bool    bOther = Registry.IsValid(Other);
			Systems.Scripts->InvokeMethod(Event.Entity, "OnStomped", { bOther ? FGameRpcValue::MakeEntity(Other) : FGameRpcValue() });
			if (bOther && Registry.IsValid(Event.Entity))
			{
				Systems.Scripts->InvokeMethod(Other, "OnStompedBy", { FGameRpcValue::MakeEntity(Event.Entity) });
			}
		}
		if (Mode != ENetMode::Client && Systems.GameModule != nullptr && Registry.IsValid(Event.Entity))
		{
			Systems.GameModule->Character2DEvent(*Scene, Event.Entity, Event.Events);
		}
	}
	return true;
}
