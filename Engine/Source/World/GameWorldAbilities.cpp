// FGameWorld의 능력 시스템 부분 (Scene/Ability — 규칙 기준은 Scene/Ability/AbilitySystem.cpp 머리 주석).
//
// 순서 (게임플레이 틱, 스크립트 갱신 바로 뒤 · 캐릭터 이동 전):
//   1) 입력 발동: AcceptInput 컴포넌트 중 이 프로세스가 조종하는 엔티티(IsLocallyControlled)에서 능력 표 InputAction이 이번 틱에 눌렸으면
//      TryActivateAbility (소유 클라이언트는 예측 발동 + AbilityActivate 메시지, 서버/Standalone은 바로 발동)
//   2) FAbilitySystem::Tick (효과 시간·주기 → 능력 스크립트/C++ 능력 재개 → 복제 문자열 → 이벤트 감지)
//   3) MoveSpeed 속성 → FCharacterMovementComponent::MaxWalkSpeed (있을 때, 모든 역할 — 소유 클라이언트는 예측 값으로 무브를 만든다)
//   4) 이벤트: 엔티티 스크립트 OnAttributeChanged(이름, 새 값, 이전 값) / OnTagChanged(태그, 수) / OnAbilityActivated(이름) /
//      OnAbilityEnded(이름, 취소됨) / OnAbilityFailed(이름, 이유) → (서버) 게임 모듈 OnAbilityEvent
//   데미지(IncomingDamage)는 Gameplay::ApplyDamage로 쌓이므로 같은 틱 게임플레이 규칙 단계에서 OnDamaged/OnDeath가 불린다.
// 메시지 (신뢰):
//   AbilityActivate (클라이언트 → 서버): uint32 NetId, string 능력, uint32 예측 키. 보낸 플레이어가 엔티티 소유자가 아니면 버린다
//   AbilityResult   (서버 → 소유 클라이언트): uint32 NetId, uint32 예측 키, uint8 취소(1)/거절(0), string 이유
#include "World/GameWorld.h"

#include "Core/Profiling.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement.h"
#include "Scene/Ability/AbilitySystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>

namespace
{
	FEntity FindEntityByNetId(FScene& Scene, uint32 NetId)
	{
		FEntity Result;
		if (NetId != InvalidNetId)
		{
			Scene.GetRegistry().View<FNetIdComponent>().Each([&](FEntity Entity, FNetIdComponent& Component) {
				if (Component.NetId == NetId)
				{
					Result = Entity;
				}
			});
		}
		return Result;
	}

	constexpr const char* MoveSpeedAttribute = "MoveSpeed";
} // namespace

void FGameWorld::ConnectAbilities()
{
	FScriptSystem* Scripts = Systems.Scripts;
	Scripts->SetAbilitySystem(Abilities.get());
	Abilities->SetScriptHooks({
		[Scripts](const FAbilityScriptStart& Start) { return Scripts->StartAbilityScript(Start); },
		[Scripts](uint32 InstanceId, bool bCancelled) { Scripts->StopAbilityScript(InstanceId, bCancelled); },
		[Scripts](float DeltaSeconds) { Scripts->TickAbilityScripts(DeltaSeconds); },
	});
	FAbilityNetHooks NetHooks;
	NetHooks.IsLocallyControlled = [this](FEntity Entity) { return IsLocallyControlled(Entity); };
	NetHooks.SendActivate        = [this](FEntity Entity, const std::string& Ability, uint32 Key) {
		const uint32 NetId = Scene != nullptr ? NetReplication::GetNetId(*Scene, Entity) : InvalidNetId;
		if (Systems.Net == nullptr || NetId == InvalidNetId || Systems.Net->GetClientState() != FNetDriver::EClientState::Joined)
		{
			return; // 복제되지 않은 엔티티: 처리되지 않은 예측은 시간 초과로 버려진다
		}
		FBinaryWriter Writer;
		Writer.Write(static_cast<uint8>(ENetMessageType::AbilityActivate));
		Writer.Write(NetId);
		Writer.WriteString(Ability);
		Writer.Write(Key);
		Systems.Net->SendToServer(Writer.GetBuffer(), ENetReliability::Reliable);
	};
	NetHooks.SendResult = [this](FEntity Entity, uint32 Key, bool bCancelled, const std::string& Reason) {
		const int32  Owner = GetOwner(Entity);
		const uint32 NetId = Scene != nullptr ? NetReplication::GetNetId(*Scene, Entity) : InvalidNetId;
		if (Systems.Net == nullptr || Owner < 0 || NetId == InvalidNetId)
		{
			return;
		}
		FBinaryWriter Writer;
		Writer.Write(static_cast<uint8>(ENetMessageType::AbilityResult));
		Writer.Write(NetId);
		Writer.Write(Key);
		Writer.Write(static_cast<uint8>(bCancelled ? 1 : 0));
		Writer.WriteString(Reason);
		for (const FNetDriver::FRemotePlayer& Player : Systems.Net->GetPlayers())
		{
			if (static_cast<int32>(Player.PlayerId) == Owner)
			{
				Systems.Net->Send(Player.Connection, Writer.GetBuffer(), ENetReliability::Reliable);
			}
		}
	};
	Abilities->SetNetHooks(std::move(NetHooks));
}

void FGameWorld::TickAbilities(float DeltaSeconds)
{
	if (!Abilities->IsPlaying())
	{
		return;
	}
	E_PROFILE_SCOPE("능력");
	// 1. 입력 발동 (조종하는 쪽)
	if (TickLocalInput != nullptr)
	{
		std::vector<FEntity> Controlled;
		for (const FEntity Entity : Abilities->GetEntities())
		{
			const FAbilitySystemComponent* Component = Abilities->Find(Entity);
			if (Component != nullptr && Component->bAcceptInput && IsLocallyControlled(Entity))
			{
				Controlled.push_back(Entity);
			}
		}
		const FInput* Input = TickLocalInput;
		for (const FEntity Entity : Controlled)
		{
			Abilities->HandleInput(Entity, [Input](const std::string& Action) { return Input->WasActionPressed(Action); });
		}
	}

	// 2. 시스템
	Abilities->Tick(DeltaSeconds);

	// 3. 이동 속도 속성 → 캐릭터 이동
	for (const FEntity Entity : Abilities->GetEntities())
	{
		float                         Speed    = 0.0f;
		FCharacterMovementComponent* Movement = Scene->GetRegistry().TryGet<FCharacterMovementComponent>(Entity);
		if (Movement != nullptr && Abilities->GetAttribute(Entity, MoveSpeedAttribute, Speed))
		{
			Movement->MaxWalkSpeed = std::max(0.0f, Speed);
		}
	}

	// 4. 이벤트
	DispatchAbilityEvents();
}

void FGameWorld::DispatchAbilityEvents()
{
	for (const FAbilityEvent& Event : Abilities->ConsumeEvents())
	{
		if (!Scene->GetRegistry().IsValid(Event.Entity))
		{
			continue;
		}
		switch (Event.Type)
		{
		case EAbilityEventType::AttributeChanged:
			Systems.Scripts->InvokeMethod(Event.Entity, "OnAttributeChanged",
			                              { FGameRpcValue::MakeString(Event.Name), FGameRpcValue::MakeNumber(Event.Value), FGameRpcValue::MakeNumber(Event.OldValue) });
			break;
		case EAbilityEventType::TagChanged:
			Systems.Scripts->InvokeMethod(Event.Entity, "OnTagChanged", { FGameRpcValue::MakeString(Event.Name), FGameRpcValue::MakeNumber(Event.Count, true) });
			break;
		case EAbilityEventType::AbilityActivated:
			Systems.Scripts->InvokeMethod(Event.Entity, "OnAbilityActivated", { FGameRpcValue::MakeString(Event.Name) });
			break;
		case EAbilityEventType::AbilityEnded:
			Systems.Scripts->InvokeMethod(Event.Entity, "OnAbilityEnded", { FGameRpcValue::MakeString(Event.Name), FGameRpcValue::MakeBool(Event.bCancelled) });
			break;
		case EAbilityEventType::AbilityFailed:
			Systems.Scripts->InvokeMethod(Event.Entity, "OnAbilityFailed", { FGameRpcValue::MakeString(Event.Name), FGameRpcValue::MakeString(Event.Reason) });
			break;
		}
		if (Systems.GameModule != nullptr && Mode != ENetMode::Client && Scene->GetRegistry().IsValid(Event.Entity))
		{
			Systems.GameModule->AbilityEvent(*Scene, Event);
		}
	}
}

void FGameWorld::ReceiveAbilityActivate(FNetConnectionId Connection, const std::vector<uint8>& Message)
{
	if (Mode == ENetMode::Client || Systems.Net == nullptr)
	{
		return;
	}
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32      NetId   = Reader.Read<uint32>();
	const std::string Ability = Reader.ReadString();
	const uint32      Key     = Reader.Read<uint32>();
	if (!Reader.IsOk() || !Reader.IsAtEnd())
	{
		E_LOG(LogNet, Warning, "잘못된 능력 발동 메시지 (연결 {})", Connection);
		return;
	}
	const FEntity Target = FindEntityByNetId(*Scene, NetId);
	if (!Target.IsValid())
	{
		return;
	}
	const std::vector<FNetDriver::FRemotePlayer>& Players = Systems.Net->GetPlayers();
	const auto Sender = std::find_if(Players.begin(), Players.end(), [Connection](const FNetDriver::FRemotePlayer& Player) { return Player.Connection == Connection; });
	if (Sender == Players.end() || static_cast<int32>(Sender->PlayerId) != GetOwner(Target))
	{
		E_LOG(LogNet, Warning, "능력 발동 거부: {} (보낸 플레이어가 대상 엔티티 소유자가 아님)", Ability);
		return;
	}
	Abilities->ServerHandleActivate(Target, Ability, Key);
}

void FGameWorld::ReceiveAbilityResult(const std::vector<uint8>& Message)
{
	if (Mode != ENetMode::Client)
	{
		return;
	}
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32      NetId      = Reader.Read<uint32>();
	const uint32      Key        = Reader.Read<uint32>();
	const bool        bCancelled = Reader.Read<uint8>() != 0;
	const std::string Reason     = Reader.ReadString();
	if (!Reader.IsOk() || !Reader.IsAtEnd())
	{
		E_LOG(LogNet, Warning, "잘못된 능력 결과 메시지");
		return;
	}
	if (const FEntity Target = FindEntityByNetId(*Scene, NetId); Target.IsValid())
	{
		Abilities->ClientHandleResult(Target, Key, bCancelled, Reason);
		DispatchAbilityEvents(); // 거절 이벤트는 바로 (다음 틱까지 기다리지 않는다)
	}
}
