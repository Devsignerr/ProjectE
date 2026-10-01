#include "Scene/GameModuleHost.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Scene/Scene.h"

#include <vector>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using FGetVersionFunc = uint32 (*)();
	using FCreateFunc     = IGameModule* (*)();

	// 엔진 DLL 안의 함수 지역 static (바이너리마다 따로 생기지 않도록 .cpp에 둔다)
	std::vector<FGameModuleHost::FOwnerCleanup>& GetUnloadCleanups()
	{
		static std::vector<FGameModuleHost::FOwnerCleanup> Cleanups;
		return Cleanups;
	}
}

void FGameModuleHost::AddUnloadCleanup(FOwnerCleanup Cleanup)
{
	GetUnloadCleanups().push_back(std::move(Cleanup));
}

FGameModuleHost::~FGameModuleHost()
{
	Unload();
}

std::filesystem::path FGameModuleHost::GetDefaultModulePath(const std::string& Name)
{
	wchar_t Buffer[MAX_PATH]{};
	GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
	return std::filesystem::path(Buffer).parent_path() / (FStringConv::ToWide(Name) + L".dll");
}

bool FGameModuleHost::Load(const std::filesystem::path& DllPath)
{
	E_CHECKF(Module == nullptr, "게임 모듈이 이미 로드되어 있습니다: {}", Name);

	HMODULE Handle = LoadLibraryW(DllPath.c_str());
	if (Handle == nullptr)
	{
		E_LOG(LogScene, Error, "게임 모듈을 로드하지 못했습니다 (오류 {}): {}", GetLastError(), FStringConv::ToUtf8(DllPath.wstring()));
		return false;
	}

	const auto GetVersion = reinterpret_cast<FGetVersionFunc>(reinterpret_cast<void*>(GetProcAddress(Handle, "ProjectE_GetGameModuleApiVersion")));
	const auto Create     = reinterpret_cast<FCreateFunc>(reinterpret_cast<void*>(GetProcAddress(Handle, "ProjectE_CreateGameModule")));
	if (GetVersion == nullptr || Create == nullptr)
	{
		E_LOG(LogScene, Error, "게임 모듈 진입점이 없습니다 (E_IMPLEMENT_GAME_MODULE 누락): {}", FStringConv::ToUtf8(DllPath.wstring()));
		FreeLibrary(Handle);
		return false;
	}
	if (GetVersion() != GameModuleApiVersion)
	{
		E_LOG(LogScene, Error, "게임 모듈 인터페이스 버전 불일치 (모듈 {}, 엔진 {}): 게임 모듈을 다시 빌드하세요", GetVersion(), GameModuleApiVersion);
		FreeLibrary(Handle);
		return false;
	}

	Library = Handle;
	Module  = Create();
	Name    = FStringConv::ToUtf8(DllPath.stem().wstring());

	FTypeRegistry&    Registry      = FTypeRegistry::Get();
	const std::string PreviousOwner = Registry.GetRegistrationOwner();
	const size_t      TypesBefore   = Registry.GetTypes().size();
	Registry.SetRegistrationOwner(Name);
	Module->OnLoad();
	Registry.SetRegistrationOwner(PreviousOwner);

	E_LOG(LogScene, Display, "게임 모듈 로드: {} (타입 {}개 등록)", Name, Registry.GetTypes().size() - TypesBefore);
	return true;
}

void FGameModuleHost::Attach(IGameModule& InModule, std::string InName)
{
	Unload();
	Module = &InModule;
	Name   = std::move(InName);
	FTypeRegistry&    Registry      = FTypeRegistry::Get();
	const std::string PreviousOwner = Registry.GetRegistrationOwner();
	Registry.SetRegistrationOwner(Name);
	Module->OnLoad();
	Registry.SetRegistrationOwner(PreviousOwner);
}

void FGameModuleHost::Unload()
{
	if (Module == nullptr)
	{
		return;
	}
	Module->OnUnload();
	const size_t Removed = FTypeRegistry::Get().RemoveTypesByOwner(Name);
	for (const FOwnerCleanup& Cleanup : GetUnloadCleanups())
	{
		Cleanup(Name);
	}
	E_LOG(LogScene, Display, "게임 모듈 언로드: {} (타입 {}개 제거)", Name, Removed);
	Module   = nullptr;
	Library  = nullptr; // DLL은 프로세스 종료까지 유지 (클래스 주석 참고)
	bPlaying = false;
}

void FGameModuleHost::BeginPlay(FScene& Scene)
{
	if (Module != nullptr && !bPlaying)
	{
		bPlaying = true;
		Module->OnBeginPlay(Scene);
	}
}

void FGameModuleHost::Update(FScene& Scene, float DeltaSeconds)
{
	if (Module != nullptr && bPlaying)
	{
		// 직전 애니메이션 갱신에서 모인 노티파이 (순회 중 모듈이 씬 구조를 바꿀 수 있으므로 먼저 복사)
		std::vector<FAnimNotifyEvent> Events;
		Scene.GetRegistry().View<FAnimationComponent>().Each([&](FEntity, FAnimationComponent& Animation) {
			Events.insert(Events.end(), Animation.Runtime.PendingNotifies.begin(), Animation.Runtime.PendingNotifies.end());
		});
		for (const FAnimNotifyEvent& Event : Events)
		{
			Module->OnAnimNotify(Scene, Event);
		}
		Module->OnUpdate(Scene, DeltaSeconds);
	}
}

void FGameModuleHost::EndPlay(FScene& Scene)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnEndPlay(Scene);
		bPlaying = false;
	}
}

void FGameModuleHost::SetNet(IGameNet* Net)
{
	if (Module != nullptr)
	{
		Module->SetNet(Net);
	}
}

void FGameModuleHost::PlayerJoined(FScene& Scene, uint32 PlayerId, FEntity Pawn)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnPlayerJoined(Scene, PlayerId, Pawn);
	}
}

void FGameModuleHost::PlayerLeft(FScene& Scene, uint32 PlayerId)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnPlayerLeft(Scene, PlayerId);
	}
}

void FGameModuleHost::Rpc(FScene& Scene, FEntity Target, EGameRpcKind Kind, const std::string& RpcName, const FGameRpcArgs& Args)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnRpc(Scene, Target, Kind, RpcName, Args);
	}
}

void FGameModuleHost::Damaged(FScene& Scene, FEntity Target, float Amount, FEntity Instigator)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnDamaged(Scene, Target, Amount, Instigator);
	}
}

void FGameModuleHost::Death(FScene& Scene, FEntity Target, FEntity Instigator)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnDeath(Scene, Target, Instigator);
	}
}

void FGameModuleHost::Respawned(FScene& Scene, FEntity Target)
{
	if (Module != nullptr && bPlaying)
	{
		Module->OnRespawned(Scene, Target);
	}
}

bool FGameModuleHost::WantsCollisionEvents(const FScene& Scene, FEntity Entity) const
{
	return Module != nullptr && bPlaying && Module->WantsCollisionEvents(Scene, Entity);
}

void FGameModuleHost::CollisionEvent(FScene& Scene, const FCollisionEvent& Event)
{
	if (Module == nullptr || !bPlaying)
	{
		return;
	}
	switch (Event.Type)
	{
	case ECollisionEventType::CollisionBegin: Module->OnCollisionBegin(Scene, Event); break;
	case ECollisionEventType::CollisionEnd:   Module->OnCollisionEnd(Scene, Event); break;
	case ECollisionEventType::TriggerEnter:   Module->OnTriggerEnter(Scene, Event); break;
	case ECollisionEventType::TriggerExit:    Module->OnTriggerExit(Scene, Event); break;
	default:                                  break;
	}
}
