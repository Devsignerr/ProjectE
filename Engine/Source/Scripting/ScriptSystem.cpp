#include "Scripting/ScriptSystem.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scripting/LuaRuntime.h"

E_DECLARE_LOG_CATEGORY(LogScript)

FScriptSystem::FScriptSystem()  = default;
FScriptSystem::~FScriptSystem() = default;

void FScriptSystem::SetContentDirectory(const std::filesystem::path& Directory)
{
	if (Directory == ContentDirectory)
	{
		return;
	}
	ContentDirectory = Directory;
	EditorRuntime.reset(); // 캐시 키(절대 경로)가 바뀌므로 다시 만든다
}

bool FScriptSystem::BeginPlay(FScene& Scene)
{
	EndPlay();
	PlayRuntime = std::make_unique<FLuaRuntime>(ContentDirectory, ErrorCount);
	++PlaySession;
	PlayRuntime->SetAudioHooks(&AudioHooks);
	PlayRuntime->SetPhysicsHooks(&PhysicsHooks);
	PlayRuntime->SetNetHooks(&NetHooks);
	PlayRuntime->SetAIHooks(&AIHooks);
	PlayRuntime->SetAppHooks(&AppHooks);
	PlayRuntime->SetSteamHooks(&SteamHooks);
	PlayRuntime->SetPersistentValues(&PersistentValues);
	PlayRuntime->SetScene(&Scene);
	E_LOG(LogScript, Display, "스크립트 플레이 시작");
	return true;
}

void FScriptSystem::Update(float DeltaSeconds, const FInput* Input)
{
	if (PlayRuntime)
	{
		PlayRuntime->Update(DeltaSeconds < MaxDeltaSeconds ? DeltaSeconds : MaxDeltaSeconds, Input);
	}
}

void FScriptSystem::LateUpdate(float DeltaSeconds, const FInput* Input)
{
	if (PlayRuntime)
	{
		PlayRuntime->LateUpdate(DeltaSeconds < MaxDeltaSeconds ? DeltaSeconds : MaxDeltaSeconds, Input);
	}
}

void FScriptSystem::EndPlay()
{
	if (!PlayRuntime)
	{
		return;
	}
	PlayRuntime->DestroyAllInstances();
	PlayRuntime.reset();
	E_LOG(LogScript, Display, "스크립트 플레이 종료");
}

bool FScriptSystem::ConsumeSceneStructureChanged()
{
	return PlayRuntime && PlayRuntime->ConsumeStructureChanged();
}

bool FScriptSystem::ReloadScript(const std::filesystem::path& ScriptPath)
{
	bool bAnySucceeded = false;
	bool bAnyFound     = false;
	for (FLuaRuntime* Runtime : { PlayRuntime.get(), EditorRuntime.get() })
	{
		if (Runtime == nullptr)
		{
			continue;
		}
		bool bSucceeded = false;
		if (Runtime->ReloadClass(ScriptPath, bSucceeded))
		{
			bAnyFound = true;
			bAnySucceeded |= bSucceeded;
		}
	}
	return !bAnyFound || bAnySucceeded;
}

const std::vector<FScriptPropertyDecl>* FScriptSystem::GetPropertyDecls(const std::string& ScriptAsset, std::string* OutError)
{
	if (ScriptAsset.empty())
	{
		if (OutError)
		{
			*OutError = "스크립트가 지정되지 않았습니다";
		}
		return nullptr;
	}
	// 경로 입력 중(존재하지 않는 파일)에는 조용히 실패한다 (키 입력마다 오류 로그를 남기지 않도록)
	if (!FFileSystem::Exists(ContentDirectory / FStringConv::ToWide(ScriptAsset)))
	{
		if (OutError)
		{
			*OutError = "스크립트 파일이 없습니다: " + ScriptAsset;
		}
		return nullptr;
	}
	if (!EditorRuntime)
	{
		EditorRuntime = std::make_unique<FLuaRuntime>(ContentDirectory, ErrorCount);
	}
	const FLuaRuntime::FScriptClass& Class = EditorRuntime->LoadClass(ScriptAsset);
	if (!Class.bValid)
	{
		if (OutError)
		{
			*OutError = Class.Error;
		}
		return nullptr;
	}
	return &Class.Decls;
}

bool FScriptSystem::RunString(std::string_view Code)
{
	if (!PlayRuntime)
	{
		E_LOG(LogScript, Warning, "RunString: 플레이 중이 아닙니다");
		return false;
	}
	return PlayRuntime->RunString(Code);
}

size_t FScriptSystem::GetInstanceCount() const
{
	return PlayRuntime ? PlayRuntime->GetInstanceCount() : 0;
}

FScriptValue FScriptSystem::GetInstanceProperty(FEntity Entity, const std::string& Name)
{
	return PlayRuntime ? PlayRuntime->GetInstanceProperty(Entity, Name) : FScriptValue{};
}

void FScriptSystem::SetAudioHooks(FScriptAudioHooks Hooks)
{
	AudioHooks = std::move(Hooks);
}

void FScriptSystem::SetPhysicsHooks(FScriptPhysicsHooks Hooks)
{
	PhysicsHooks = std::move(Hooks);
}

void FScriptSystem::SetNetHooks(FScriptNetHooks Hooks)
{
	NetHooks = std::move(Hooks);
}

void FScriptSystem::SetAIHooks(FScriptAIHooks Hooks)
{
	AIHooks = std::move(Hooks);
}

void FScriptSystem::SetAppHooks(FScriptAppHooks Hooks)
{
	AppHooks = std::move(Hooks);
}

void FScriptSystem::SetSteamHooks(FScriptSteamHooks Hooks)
{
	SteamHooks = std::move(Hooks);
}

FScriptObjectHandle FScriptSystem::CreateObject(const std::string& ScriptAsset, const std::string& PropertyOverrides, FEntity Entity)
{
	if (!PlayRuntime)
	{
		return 0;
	}
	const uint32 LocalId = PlayRuntime->CreateObject(ScriptAsset, PropertyOverrides, Entity);
	return LocalId == 0 ? 0 : (static_cast<uint64>(PlaySession) << 32) | LocalId;
}

bool FScriptSystem::CallObject(FScriptObjectHandle Handle, const char* Method, const float* DeltaSeconds, FScriptValue& OutResult, bool* bOutFound)
{
	OutResult = FScriptValue{};
	if (bOutFound)
	{
		*bOutFound = false;
	}
	if (!PlayRuntime || static_cast<uint32>(Handle >> 32) != PlaySession)
	{
		return false; // 지난 세션의 핸들
	}
	return PlayRuntime->CallObject(static_cast<uint32>(Handle & 0xFFFFFFFFu), Method, DeltaSeconds, OutResult, bOutFound);
}

void FScriptSystem::DestroyObject(FScriptObjectHandle Handle)
{
	if (PlayRuntime && static_cast<uint32>(Handle >> 32) == PlaySession)
	{
		PlayRuntime->DestroyObject(static_cast<uint32>(Handle & 0xFFFFFFFFu));
	}
}

bool FScriptSystem::InvokeMethod(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args)
{
	return PlayRuntime != nullptr && PlayRuntime->InvokeMethod(Target, MethodName, Args);
}

void FScriptSystem::BroadcastMethod(const std::string& MethodName, const FGameRpcArgs& Args)
{
	if (PlayRuntime != nullptr)
	{
		PlayRuntime->BroadcastMethod(MethodName, Args);
	}
}

bool FScriptSystem::RequestDestroy(FEntity Entity)
{
	if (!PlayRuntime)
	{
		return false;
	}
	PlayRuntime->RequestDestroy(Entity);
	return true;
}
