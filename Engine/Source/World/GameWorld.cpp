#include "World/GameWorld.h"

#include "Core/Assert.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationTypes.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimationSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <stdexcept>

// 스크립트 RPC 메시지 (ENetMessageType::ScriptRpc, 신뢰):
//   uint32 대상 NetId, uint8 종류(EScriptRpcKind), string 이름, uint8 인자 수,
//   [uint8 태그(0 nil, 1 bool, 2 숫자, 3 문자열, 4 Vector3, 5 에셋, 6 엔티티), 값...]...
//   숫자 = double + uint8 정수 여부, 에셋 = 경로 + 확장자, 엔티티 = NetId (복제되지 않았으면 0 → 받는 쪽 무효 엔티티)
namespace
{
	const char* RpcPrefix(EScriptRpcKind Kind)
	{
		return Kind == EScriptRpcKind::Server ? "Server_" : (Kind == EScriptRpcKind::Client ? "Client_" : "Multicast_");
	}

	FEntity FindByNetId(FScene& Scene, uint32 NetId)
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

	std::vector<uint8> EncodeRpc(const FScene& Scene, uint32 NetId, EScriptRpcKind Kind, const std::string& Name, const std::vector<FScriptRpcArg>& Args)
	{
		FBinaryWriter Writer;
		Writer.Write(static_cast<uint8>(ENetMessageType::ScriptRpc));
		Writer.Write(NetId);
		Writer.Write(static_cast<uint8>(Kind));
		Writer.WriteString(Name);
		Writer.Write(static_cast<uint8>(std::min<size_t>(Args.size(), 255)));
		for (size_t Index = 0; Index < Args.size() && Index < 255; ++Index)
		{
			const FScriptRpcArg& Arg = Args[Index];
			if (Arg.bIsEntity)
			{
				Writer.Write(static_cast<uint8>(6));
				Writer.Write(NetReplication::GetNetId(Scene, Arg.Entity));
				continue;
			}
			switch (Arg.Value.Type)
			{
			case EScriptValueType::Bool:
				Writer.Write(static_cast<uint8>(1));
				Writer.Write(static_cast<uint8>(Arg.Value.bBool ? 1 : 0));
				break;
			case EScriptValueType::Number:
				Writer.Write(static_cast<uint8>(2));
				Writer.Write(Arg.Value.Number);
				Writer.Write(static_cast<uint8>(Arg.Value.bInteger ? 1 : 0));
				break;
			case EScriptValueType::String:
				Writer.Write(static_cast<uint8>(3));
				Writer.WriteString(Arg.Value.String);
				break;
			case EScriptValueType::Vector3:
				Writer.Write(static_cast<uint8>(4));
				Writer.Write(Arg.Value.Vector);
				break;
			case EScriptValueType::Asset:
				Writer.Write(static_cast<uint8>(5));
				Writer.WriteString(Arg.Value.String);
				Writer.WriteString(Arg.Value.AssetFilter);
				break;
			default:
				Writer.Write(static_cast<uint8>(0));
				break;
			}
		}
		return Writer.GetBuffer();
	}
} // namespace

void FGameWorld::Init(const FGameWorldSystems& InSystems)
{
	E_CHECKF(InSystems.Scripts != nullptr, "FGameWorld: 스크립트 시스템은 필수입니다");
	Systems = InSystems;
	Systems.Scripts->SetContentDirectory(Systems.ContentDirectory);

	FPhysicsSystem* Physics = Systems.Physics;
	if (Physics == nullptr)
	{
		return;
	}
	Systems.Scripts->SetPhysicsHooks({
		[Physics](const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Raycast(Origin, Direction, MaxDistance, Hit))
			{
				return false;
			}
			OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance };
			return true;
		},
		[Physics](FEntity Entity, const FVector3& Force) { Physics->AddForce(Entity, Force); },
		[Physics](FEntity Entity, const FVector3& Impulse) { Physics->AddImpulse(Entity, Impulse); },
		[Physics](FEntity Entity, const FVector3& Velocity) { Physics->SetVelocity(Entity, Velocity); },
		[Physics](FEntity Entity) { return Physics->GetVelocity(Entity); },
		[Physics](FEntity Entity) { return Physics->GetMass(Entity); },
	});
}

void FGameWorld::BeginPlay(FScene& InScene, ENetMode InMode)
{
	if (IsPlaying())
	{
		EndPlay();
	}
	Scene                 = &InScene;
	Mode                  = InMode;
	const bool bClient    = Mode == ENetMode::Client;
	const bool bDedicated = Mode == ENetMode::DedicatedServer;

	// 스크립트 네트워크 정보: 실행 위치 필터, 모드, 로컬 플레이어, 소유권(가장 가까운 복제 조상의 OwnerPlayerId)
	FScriptNetHooks NetHooks;
	NetHooks.bRunServerScripts = !bClient;
	NetHooks.bRunClientScripts = !bDedicated;
	NetHooks.bIsServer         = !bClient;
	NetHooks.bIsClient         = !bDedicated;
	NetHooks.ModeName          = ToString(Mode);
	NetHooks.GetLocalPlayerId  = [this]() {
		if (Mode == ENetMode::DedicatedServer)
		{
			return -1; // 전용 서버에는 로컬 플레이어가 없다
		}
		return static_cast<int32>(Systems.Net != nullptr ? Systems.Net->GetLocalPlayerId() : 0);
	};
	NetHooks.GetOwner          = [this](FEntity Entity) { return GetEntityOwner(Entity); };
	NetHooks.SendRpc           = [this](FEntity Target, EScriptRpcKind Kind, const std::string& Name, const std::vector<FScriptRpcArg>& Args) {
		RouteScriptRpc(Target, static_cast<uint8>(Kind), Name, Args);
	};
	NetHooks.ResolveInput = [this](FEntity Entity, const FInput* LocalInput) { return ResolveInput(Entity, LocalInput); };
	RemoteInputs.clear();
	InputSequence = 0;
	Systems.Scripts->SetNetHooks(std::move(NetHooks));

	if (Systems.Physics != nullptr)
	{
		if (bClient)
		{
			// 서버가 시뮬레이션하는 복제 엔티티(NetId 보유)는 키네마틱: 복제 트랜스폼을 따라가며 로컬 물체와 충돌
			Systems.Physics->SetKinematicOverride([](const FScene& Target, FEntity Entity) { return Target.GetRegistry().Has<FNetIdComponent>(Entity); });
		}
		else
		{
			Systems.Physics->SetKinematicOverride(nullptr);
		}
		Systems.Physics->Begin();
	}
	if (Systems.GameModule != nullptr && !bClient) // 게임 모듈(C++ 게임 로직)은 서버에서만
	{
		Systems.GameModule->BeginPlay(InScene);
	}
	Systems.Scripts->BeginPlay(InScene);
}

void FGameWorld::EndPlay()
{
	if (!IsPlaying())
	{
		return;
	}
	Systems.Scripts->EndPlay();
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->EndPlay(*Scene);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->End();
	}
	Scene = nullptr;
}

void FGameWorld::TickGameplay(float DeltaSeconds, const FInput* Input)
{
	if (!IsPlaying())
	{
		return;
	}
	if (Mode == ENetMode::Client && Input != nullptr)
	{
		SendLocalInput(*Input); // 서버 스크립트가 이 플레이어 소유 엔티티에서 읽는다
	}
	Systems.Scripts->Update(DeltaSeconds, Input); // 실행 위치 필터는 BeginPlay에서 정했다
	if (Systems.Scripts->ConsumeSceneStructureChanged() && Systems.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->Update(*Scene, DeltaSeconds);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->Update(*Scene, DeltaSeconds);
	}
	Scene->UpdateTransforms();
	for (auto& [PlayerId, Remote] : RemoteInputs)
	{
		Remote.Input.EndFrame(); // 원격 입력의 눌림/떼어짐은 서버 틱 한 번만
	}
}

void FGameWorld::TickPresentation(FScene& TargetScene, float DeltaSeconds)
{
	FAnimationSystem::Update(TargetScene, DeltaSeconds);
	TargetScene.UpdateTransforms();
	if (Systems.Resources != nullptr)
	{
		FSceneAssetResolver::ResolveParticles(TargetScene, *Systems.Resources, Systems.ContentDirectory);
	}
	FParticleSystem::Update(TargetScene, DeltaSeconds);
}

int32 FGameWorld::GetEntityOwner(FEntity Entity) const
{
	for (FEntity Current = Entity; Scene != nullptr && Scene->GetRegistry().IsValid(Current); Current = Scene->GetParent(Current))
	{
		if (const FReplicatedComponent* Replicated = Scene->GetRegistry().TryGet<FReplicatedComponent>(Current))
		{
			return Replicated->OwnerPlayerId;
		}
	}
	return -1;
}

void FGameWorld::RouteScriptRpc(FEntity Target, uint8 KindValue, const std::string& Name, const std::vector<FScriptRpcArg>& Args)
{
	const EScriptRpcKind Kind    = static_cast<EScriptRpcKind>(KindValue);
	const std::string    Method  = RpcPrefix(Kind) + Name;
	const bool           bServer = Mode != ENetMode::Client;
	const bool           bOnline = Systems.Net != nullptr && Systems.Net->GetMode() != ENetMode::Standalone;
	const uint32         NetId   = NetReplication::GetNetId(*Scene, Target);

	switch (Kind)
	{
	case EScriptRpcKind::Server:
		if (bServer)
		{
			Systems.Scripts->InvokeMethod(Target, Method, Args); // 서버에서 부르면 바로 실행
		}
		else if (NetId == InvalidNetId)
		{
			throw std::runtime_error("CallServer: 복제되지 않은 엔티티입니다 (ReplicatedComponent 필요)");
		}
		else if (Systems.Net != nullptr)
		{
			Systems.Net->SendToServer(EncodeRpc(*Scene, NetId, Kind, Name, Args), ENetReliability::Reliable);
		}
		break;

	case EScriptRpcKind::Client:
	{
		if (!bServer)
		{
			throw std::runtime_error("CallClient는 서버에서만 부를 수 있습니다");
		}
		const int32 Owner = GetEntityOwner(Target);
		if (Owner < 0)
		{
			E_LOG(LogNet, Warning, "CallClient({}): 소유 플레이어가 없는 엔티티입니다", Name);
			break;
		}
		const int32 LocalPlayer = static_cast<int32>(Systems.Net != nullptr ? Systems.Net->GetLocalPlayerId() : 0);
		if (Mode != ENetMode::DedicatedServer && Owner == LocalPlayer)
		{
			Systems.Scripts->InvokeMethod(Target, Method, Args); // 호스트(로컬 플레이어) 소유
			break;
		}
		if (bOnline && NetId != InvalidNetId)
		{
			for (const FNetDriver::FRemotePlayer& Player : Systems.Net->GetPlayers())
			{
				if (static_cast<int32>(Player.PlayerId) == Owner)
				{
					Systems.Net->Send(Player.Connection, EncodeRpc(*Scene, NetId, Kind, Name, Args), ENetReliability::Reliable);
				}
			}
		}
		break;
	}

	case EScriptRpcKind::Multicast:
		if (!bServer)
		{
			throw std::runtime_error("CallMulticast는 서버에서만 부를 수 있습니다");
		}
		Systems.Scripts->InvokeMethod(Target, Method, Args);
		if (bOnline && NetId != InvalidNetId)
		{
			Systems.Net->Broadcast(EncodeRpc(*Scene, NetId, Kind, Name, Args), ENetReliability::Reliable);
		}
		break;
	}
}

bool FGameWorld::HandleNetMessage(FNetConnectionId Connection, const std::vector<uint8>& Message)
{
	if (Message.empty() || !IsPlaying())
	{
		return false;
	}
	if (Message[0] == static_cast<uint8>(ENetMessageType::PlayerInput))
	{
		ReceivePlayerInput(Connection, Message);
		return true;
	}
	if (Message[0] != static_cast<uint8>(ENetMessageType::ScriptRpc))
	{
		return false;
	}
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32               NetId = Reader.Read<uint32>();
	const EScriptRpcKind       Kind  = static_cast<EScriptRpcKind>(Reader.Read<uint8>());
	const std::string          Name  = Reader.ReadString();
	const uint8                Count = Reader.Read<uint8>();
	std::vector<FScriptRpcArg> Args(Count);
	for (FScriptRpcArg& Arg : Args)
	{
		switch (Reader.Read<uint8>())
		{
		case 1:
			Arg.Value = FScriptValue::MakeBool(Reader.Read<uint8>() != 0);
			break;
		case 2:
		{
			const double Number = Reader.Read<double>();
			Arg.Value           = FScriptValue::MakeNumber(Number, Reader.Read<uint8>() != 0);
			break;
		}
		case 3:
			Arg.Value = FScriptValue::MakeString(Reader.ReadString());
			break;
		case 4:
			Arg.Value = FScriptValue::MakeVector3(Reader.Read<FVector3>());
			break;
		case 5:
		{
			std::string Path = Reader.ReadString();
			Arg.Value        = FScriptValue::MakeAsset(std::move(Path), Reader.ReadString());
			break;
		}
		case 6:
			Arg.bIsEntity = true;
			Arg.Entity    = FindByNetId(*Scene, Reader.Read<uint32>());
			break;
		default:
			break;
		}
	}
	if (!Reader.IsOk() || !Reader.IsAtEnd())
	{
		E_LOG(LogNet, Warning, "잘못된 스크립트 RPC 메시지 (연결 {})", Connection);
		return true;
	}

	const FEntity Target = FindByNetId(*Scene, NetId);
	if (!Target.IsValid())
	{
		return true; // 이미 파괴됐거나 아직 생성 전
	}
	if (Mode != ENetMode::Client)
	{
		// 서버: 클라이언트는 Server RPC만, 그리고 대상 엔티티의 소유자만 부를 수 있다
		static const std::vector<FNetDriver::FRemotePlayer> NoPlayers;
		const std::vector<FNetDriver::FRemotePlayer>& Players = Systems.Net != nullptr ? Systems.Net->GetPlayers() : NoPlayers;
		const auto Sender = std::find_if(Players.begin(), Players.end(), [Connection](const FNetDriver::FRemotePlayer& Player) { return Player.Connection == Connection; });
		if (Kind != EScriptRpcKind::Server || Sender == Players.end() || static_cast<int32>(Sender->PlayerId) != GetEntityOwner(Target))
		{
			E_LOG(LogNet, Warning, "RPC 거부: {}{} (보낸 플레이어가 대상 엔티티 소유자가 아님)", RpcPrefix(Kind), Name);
			return true;
		}
	}
	else if (Kind == EScriptRpcKind::Server)
	{
		return true; // 클라이언트는 Server RPC를 받지 않는다
	}
	Systems.Scripts->InvokeMethod(Target, RpcPrefix(Kind) + Name, Args);
	return true;
}

// 입력 커맨드 (ENetMessageType::PlayerInput, 비신뢰, 클라이언트 → 서버):
//   uint32 순번, 키 비트(EKey::Count비트를 바이트로), 마우스 버튼 비트(1바이트), int32 마우스 X, int32 마우스 Y, float 휠
// 상태 전체를 보내므로 손실돼도 다음 커맨드로 복구된다 (순번이 오래된 것은 버린다)
void FGameWorld::SendLocalInput(const FInput& Input)
{
	if (Systems.Net == nullptr || Systems.Net->GetClientState() != FNetDriver::EClientState::Joined)
	{
		return;
	}
	FBinaryWriter Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::PlayerInput));
	Writer.Write(++InputSequence);
	const FInput::FKeyBits& Keys = Input.GetKeyStates();
	for (size_t Byte = 0; Byte < (Keys.size() + 7) / 8; ++Byte)
	{
		uint8 Bits = 0;
		for (size_t Bit = 0; Bit < 8 && Byte * 8 + Bit < Keys.size(); ++Bit)
		{
			Bits |= Keys[Byte * 8 + Bit] ? static_cast<uint8>(1u << Bit) : 0;
		}
		Writer.Write(Bits);
	}
	static_assert(static_cast<size_t>(EMouseButton::Count) <= 8, "마우스 버튼 비트는 1바이트");
	Writer.Write(static_cast<uint8>(Input.GetButtonStates().to_ulong()));
	Writer.Write(Input.GetMouseX());
	Writer.Write(Input.GetMouseY());
	Writer.Write(Input.GetMouseWheelDelta());
	Systems.Net->SendToServer(Writer.GetBuffer(), ENetReliability::Unreliable);
}

void FGameWorld::ReceivePlayerInput(FNetConnectionId Connection, const std::vector<uint8>& Message)
{
	if (Mode == ENetMode::Client || Systems.Net == nullptr)
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
	const uint32     Sequence = Reader.Read<uint32>();
	FInput::FKeyBits Keys;
	for (size_t Byte = 0; Byte < (Keys.size() + 7) / 8; ++Byte)
	{
		const uint8 Bits = Reader.Read<uint8>();
		for (size_t Bit = 0; Bit < 8 && Byte * 8 + Bit < Keys.size(); ++Bit)
		{
			Keys[Byte * 8 + Bit] = (Bits >> Bit) & 1u;
		}
	}
	const FInput::FButtonBits Buttons(Reader.Read<uint8>());
	const int32               MouseX = Reader.Read<int32>();
	const int32               MouseY = Reader.Read<int32>();
	const float               Wheel  = Reader.Read<float>();
	FRemoteInput&             Remote = RemoteInputs[Sender->PlayerId];
	if (!Reader.IsOk() || !Reader.IsAtEnd() || Sequence <= Remote.LastSequence)
	{
		return; // 잘렸거나 늦게 도착한(재정렬) 커맨드
	}
	Remote.LastSequence = Sequence;
	Remote.Input.SetState(Keys, Buttons, MouseX, MouseY, Wheel);
}

const FInput* FGameWorld::ResolveInput(FEntity Entity, const FInput* LocalInput) const
{
	if (Mode == ENetMode::Client)
	{
		return LocalInput; // 클라이언트 스크립트는 자기 입력
	}
	const int32 Owner       = GetEntityOwner(Entity);
	const int32 LocalPlayer = Mode == ENetMode::DedicatedServer ? -1 : static_cast<int32>(Systems.Net != nullptr ? Systems.Net->GetLocalPlayerId() : 0);
	if (Owner < 0)
	{
		return Mode == ENetMode::DedicatedServer ? nullptr : LocalInput; // 서버 소유: 호스트 입력 (1인용 동작 그대로)
	}
	if (Owner == LocalPlayer)
	{
		return LocalInput;
	}
	const auto Found = RemoteInputs.find(static_cast<uint32>(Owner));
	return Found != RemoteInputs.end() ? &Found->second.Input : nullptr;
}

void FGameWorld::OnPlayerJoined(uint32 PlayerId, FEntity Pawn)
{
	if (!IsPlaying() || Mode == ENetMode::Client)
	{
		return;
	}
	FScriptRpcArg Id;
	Id.Value = FScriptValue::MakeNumber(static_cast<double>(PlayerId), true);
	FScriptRpcArg PawnArg;
	PawnArg.bIsEntity = true;
	PawnArg.Entity    = Pawn;
	Systems.Scripts->BroadcastMethod("OnPlayerJoined", { Id, PawnArg });
}

void FGameWorld::OnPlayerLeft(uint32 PlayerId)
{
	RemoteInputs.erase(PlayerId);
	if (!IsPlaying() || Mode == ENetMode::Client)
	{
		return;
	}
	FScriptRpcArg Id;
	Id.Value = FScriptValue::MakeNumber(static_cast<double>(PlayerId), true);
	Systems.Scripts->BroadcastMethod("OnPlayerLeft", { Id });
}
