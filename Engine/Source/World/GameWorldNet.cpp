// FGameWorld의 네트워크 부분: 스크립트 네트워크 훅, RPC(스크립트/게임 모듈 공용), 입력 커맨드, 플레이어 이벤트
#include "World/GameWorld.h"

#include "Core/Assert.h"
#include "Core/Paths.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <stdexcept>

// RPC 메시지 (ENetMessageType::ScriptRpc, 신뢰):
//   uint32 대상 NetId, uint8 종류(EGameRpcKind), string 이름(접두사 없음), uint8 인자 수,
//   [uint8 태그(0 nil, 1 bool, 2 숫자, 3 문자열, 4 Vector3, 5 에셋, 6 엔티티), 값...]...
//   숫자 = double + uint8 정수 여부, 에셋 = 경로 + 확장자, 엔티티 = NetId (복제되지 않았으면 0 → 받는 쪽 무효 엔티티)
// 입력 커맨드 (ENetMessageType::PlayerInput, 비신뢰, 클라이언트 → 서버):
//   uint32 순번, 키 비트(EKey::Count비트를 바이트로), 마우스 버튼 비트(1바이트), int32 마우스 X, int32 마우스 Y, float 휠
//   상태 전체를 보내므로 손실돼도 다음 커맨드로 복구된다 (순번이 오래된 것은 버린다)
namespace
{
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

	std::vector<uint8> EncodeRpc(const FScene& Scene, uint32 NetId, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args)
	{
		FBinaryWriter Writer;
		Writer.Write(static_cast<uint8>(ENetMessageType::ScriptRpc));
		Writer.Write(NetId);
		Writer.Write(static_cast<uint8>(Kind));
		Writer.WriteString(Name);
		Writer.Write(static_cast<uint8>(std::min<size_t>(Args.size(), 255)));
		for (size_t Index = 0; Index < Args.size() && Index < 255; ++Index)
		{
			const FGameRpcValue& Arg = Args[Index];
			switch (Arg.Type)
			{
			case FGameRpcValue::EType::Bool:
				Writer.Write(static_cast<uint8>(1));
				Writer.Write(static_cast<uint8>(Arg.bBool ? 1 : 0));
				break;
			case FGameRpcValue::EType::Number:
				Writer.Write(static_cast<uint8>(2));
				Writer.Write(Arg.Number);
				Writer.Write(static_cast<uint8>(Arg.bInteger ? 1 : 0));
				break;
			case FGameRpcValue::EType::String:
				Writer.Write(static_cast<uint8>(3));
				Writer.WriteString(Arg.String);
				break;
			case FGameRpcValue::EType::Vector3:
				Writer.Write(static_cast<uint8>(4));
				Writer.Write(Arg.Vector);
				break;
			case FGameRpcValue::EType::Asset:
				Writer.Write(static_cast<uint8>(5));
				Writer.WriteString(Arg.String);
				Writer.WriteString(Arg.AssetFilter);
				break;
			case FGameRpcValue::EType::Entity:
				Writer.Write(static_cast<uint8>(6));
				Writer.Write(NetReplication::GetNetId(Scene, Arg.Entity));
				break;
			default:
				Writer.Write(static_cast<uint8>(0));
				break;
			}
		}
		return Writer.GetBuffer();
	}
} // namespace

void FGameWorld::InstallScriptNetHooks()
{
	const bool      bClient    = Mode == ENetMode::Client;
	const bool      bDedicated = Mode == ENetMode::DedicatedServer;
	FScriptNetHooks NetHooks;
	NetHooks.bRunServerScripts = !bClient;
	NetHooks.bRunClientScripts = !bDedicated;
	NetHooks.bIsServer         = IsServer();
	NetHooks.bIsClient         = IsClient();
	NetHooks.ModeName          = ToString(Mode);
	NetHooks.GetLocalPlayerId  = [this]() { return GetLocalPlayerId(); };
	NetHooks.GetOwner          = [this](FEntity Entity) { return GetOwner(Entity); };
	NetHooks.SendRpc           = [this](FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args) { RouteRpc(Target, Kind, Name, Args); };
	NetHooks.ResolveInput      = [this](FEntity Entity, const FInput* LocalInput) { return ResolveInput(Entity, LocalInput); };

	// 세션 (로비)
	NetHooks.FindSessions = [this]() { SessionSearch.StartSearch(FPaths::HasProject() ? FPaths::GetProjectName() : std::string(), LanDiscoveryPort); };
	NetHooks.GetSessions  = [this]() {
		std::vector<FScriptLanSession> Result;
		for (const FLanSession& Session : SessionSearch.GetSessions())
		{
			Result.push_back({ Session.Name, Session.SceneAsset, Session.Address, Session.Players, Session.MaxPlayers });
		}
		return Result;
	};
	NetHooks.Host       = [this](int32 Port) { PendingSessionRequest = FNetSessionRequest{ FNetSessionRequest::EType::Host, {}, static_cast<uint16>(std::clamp(Port, 0, 65535)) }; };
	NetHooks.Connect    = [this](const std::string& Address) { PendingSessionRequest = FNetSessionRequest{ FNetSessionRequest::EType::Connect, Address, 0 }; };
	NetHooks.Disconnect = [this]() { PendingSessionRequest = FNetSessionRequest{ FNetSessionRequest::EType::Disconnect, {}, 0 }; };
	NetHooks.GetState   = [this]() -> std::string {
		if (Systems.Net == nullptr || Systems.Net->GetMode() == ENetMode::Standalone)
		{
			return "Standalone";
		}
		if (Systems.Net->IsServer())
		{
			return "Hosting";
		}
		switch (Systems.Net->GetClientState())
		{
		case FNetDriver::EClientState::Connecting: return "Connecting";
		case FNetDriver::EClientState::Joined:     return "Connected";
		case FNetDriver::EClientState::Failed:     return "Failed";
		default:                                   return "Standalone";
		}
	};
	NetHooks.GetFailureReason = [this]() { return Systems.Net != nullptr ? Systems.Net->GetFailureReason() : std::string(); };
	Systems.Scripts->SetNetHooks(std::move(NetHooks));
}

std::optional<FNetSessionRequest> FGameWorld::ConsumeSessionRequest()
{
	std::optional<FNetSessionRequest> Request = std::move(PendingSessionRequest);
	PendingSessionRequest.reset();
	return Request;
}

void FGameWorld::SetNetMode(ENetMode InMode)
{
	// 스크립트 실행 필터가 바뀌는 전환(클라이언트 ↔ 서버)은 월드를 다시 시작해야 한다
	E_CHECKF((Mode == ENetMode::Client) == (InMode == ENetMode::Client), "SetNetMode: 클라이언트 ↔ 서버 전환은 BeginPlay로 다시 시작하세요");
	Mode = InMode;
	InstallScriptNetHooks();
}

int32 FGameWorld::GetLocalPlayerId() const
{
	if (Mode == ENetMode::DedicatedServer)
	{
		return -1; // 전용 서버에는 로컬 플레이어가 없다
	}
	return static_cast<int32>(Systems.Net != nullptr ? Systems.Net->GetLocalPlayerId() : 0);
}

int32 FGameWorld::GetOwner(FEntity Entity) const
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

void FGameWorld::CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args)
{
	if (!IsPlaying())
	{
		return;
	}
	try
	{
		RouteRpc(Target, Kind, Name, Args);
	}
	catch (const std::runtime_error& Error)
	{
		E_LOG(LogNet, Warning, "게임 모듈 RPC 무시 ({}{}): {}", GetRpcMethodPrefix(Kind), Name, Error.what());
	}
}

void FGameWorld::InvokeRpcLocally(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args)
{
	Systems.Scripts->InvokeMethod(Target, GetRpcMethodPrefix(Kind) + Name, Args);
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->Rpc(*Scene, Target, Kind, Name, Args); // 게임 모듈은 서버에서만 돈다
	}
}

void FGameWorld::RouteRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args)
{
	const bool   bServer = Mode != ENetMode::Client;
	const bool   bOnline = Systems.Net != nullptr && Systems.Net->GetMode() != ENetMode::Standalone;
	const uint32 NetId   = NetReplication::GetNetId(*Scene, Target);

	switch (Kind)
	{
	case EGameRpcKind::Server:
		if (bServer)
		{
			InvokeRpcLocally(Target, Kind, Name, Args); // 서버에서 부르면 바로 실행
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

	case EGameRpcKind::Client:
	{
		if (!bServer)
		{
			throw std::runtime_error("CallClient는 서버에서만 부를 수 있습니다");
		}
		const int32 Owner = GetOwner(Target);
		if (Owner < 0)
		{
			E_LOG(LogNet, Warning, "CallClient({}): 소유 플레이어가 없는 엔티티입니다", Name);
			break;
		}
		if (Owner == GetLocalPlayerId())
		{
			InvokeRpcLocally(Target, Kind, Name, Args); // 호스트(로컬 플레이어) 소유
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

	case EGameRpcKind::Multicast:
		if (!bServer)
		{
			throw std::runtime_error("CallMulticast는 서버에서만 부를 수 있습니다");
		}
		InvokeRpcLocally(Target, Kind, Name, Args);
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
	switch (static_cast<ENetMessageType>(Message[0]))
	{
	case ENetMessageType::PlayerInput: ReceivePlayerInput(Connection, Message); return true;
	case ENetMessageType::ScriptRpc:   ReceiveRpc(Connection, Message); return true;
	default:                           return false;
	}
}

void FGameWorld::ReceiveRpc(FNetConnectionId Connection, const std::vector<uint8>& Message)
{
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32       NetId = Reader.Read<uint32>();
	const EGameRpcKind Kind  = static_cast<EGameRpcKind>(Reader.Read<uint8>());
	const std::string  Name  = Reader.ReadString();
	FGameRpcArgs       Args(Reader.Read<uint8>());
	for (FGameRpcValue& Arg : Args)
	{
		switch (Reader.Read<uint8>())
		{
		case 1:
			Arg = FGameRpcValue::MakeBool(Reader.Read<uint8>() != 0);
			break;
		case 2:
		{
			const double Number = Reader.Read<double>();
			Arg                 = FGameRpcValue::MakeNumber(Number, Reader.Read<uint8>() != 0);
			break;
		}
		case 3:
			Arg = FGameRpcValue::MakeString(Reader.ReadString());
			break;
		case 4:
			Arg = FGameRpcValue::MakeVector3(Reader.Read<FVector3>());
			break;
		case 5:
		{
			std::string Path = Reader.ReadString();
			Arg              = FGameRpcValue::MakeAsset(std::move(Path), Reader.ReadString());
			break;
		}
		case 6:
			Arg = FGameRpcValue::MakeEntity(FindByNetId(*Scene, Reader.Read<uint32>()));
			break;
		default:
			break;
		}
	}
	if (!Reader.IsOk() || !Reader.IsAtEnd() || Kind > EGameRpcKind::Multicast)
	{
		E_LOG(LogNet, Warning, "잘못된 RPC 메시지 (연결 {})", Connection);
		return;
	}

	const FEntity Target = FindByNetId(*Scene, NetId);
	if (!Target.IsValid())
	{
		return; // 이미 파괴됐거나 아직 생성 전
	}
	if (Mode != ENetMode::Client)
	{
		// 서버: 클라이언트는 Server RPC만, 그리고 대상 엔티티의 소유자만 부를 수 있다
		const std::vector<FNetDriver::FRemotePlayer>& Players = Systems.Net->GetPlayers();
		const auto Sender = std::find_if(Players.begin(), Players.end(), [Connection](const FNetDriver::FRemotePlayer& Player) { return Player.Connection == Connection; });
		if (Kind != EGameRpcKind::Server || Sender == Players.end() || static_cast<int32>(Sender->PlayerId) != GetOwner(Target))
		{
			E_LOG(LogNet, Warning, "RPC 거부: {}{} (보낸 플레이어가 대상 엔티티 소유자가 아님)", GetRpcMethodPrefix(Kind), Name);
			return;
		}
	}
	else if (Kind == EGameRpcKind::Server)
	{
		return; // 클라이언트는 Server RPC를 받지 않는다
	}
	InvokeRpcLocally(Target, Kind, Name, Args);
}

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
	const int32 Owner = GetOwner(Entity);
	if (Owner < 0)
	{
		return Mode == ENetMode::DedicatedServer ? nullptr : LocalInput; // 서버 소유: 호스트 입력 (1인용 동작 그대로)
	}
	if (Owner == GetLocalPlayerId())
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
	Systems.Scripts->BroadcastMethod("OnPlayerJoined", { FGameRpcValue::MakeNumber(static_cast<double>(PlayerId), true), FGameRpcValue::MakeEntity(Pawn) });
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->PlayerJoined(*Scene, PlayerId, Pawn);
	}
}

void FGameWorld::OnPlayerLeft(uint32 PlayerId)
{
	RemoteInputs.erase(PlayerId);
	if (!IsPlaying() || Mode == ENetMode::Client)
	{
		return;
	}
	Systems.Scripts->BroadcastMethod("OnPlayerLeft", { FGameRpcValue::MakeNumber(static_cast<double>(PlayerId), true) });
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->PlayerLeft(*Scene, PlayerId);
	}
}
