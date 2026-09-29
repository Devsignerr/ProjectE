#include "Scripting/ScriptSystem.h"

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
	PlayRuntime->SetAudioHooks(&AudioHooks);
	PlayRuntime->SetPhysicsHooks(&PhysicsHooks);
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
	std::error_code ErrorCode;
	if (!std::filesystem::is_regular_file(ContentDirectory / FStringConv::ToWide(ScriptAsset), ErrorCode))
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
