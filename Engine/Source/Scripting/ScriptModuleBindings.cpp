#include "Scripting/LuaRuntime.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <deque>
#include <format>
#include <stdexcept>
#include <unordered_set>

E_DECLARE_LOG_CATEGORY(LogScript)

// Script.Require — Lua 모듈 (공용 데이터/함수 파일)
//   local Items = Script.Require("Scripts/RPG/ItemDatabase.lua")   -- Content 기준 경로
// 규칙:
//   - 파일은 FFileSystem(pak → 디스크)으로 읽어 Lua 상태(플레이 런타임/에디터 런타임 각각)마다 한 번만 실행하고, 반환값(아무 Lua 값, nil이면 true)을
//     정규화 경로(대소문자 무시) 키로 캐시한다. 같은 상태 안에서는 모든 호출자가 같은 값(같은 테이블)을 받는다
//   - 표준 require/package/dofile/loadfile은 계속 막혀 있다 (Content 밖 파일·절대 경로·".."로 Content를 벗어나는 경로도 오류)
//   - 실행 중 오류·파일 없음·순환(A가 실행 중에 다시 A를 Require)은 Lua 오류로 호출자에게 전파된다. 실패한 모듈은 캐시하지 않는다(다음 호출이 다시 시도)
//   - 핫 리로드: 모듈 파일이 바뀌면(FScriptSystem::ReloadScript) 새 코드를 실행해 보고 성공했을 때만 값을 교체한다(실패 시 기존 값 유지 + 오류 로그).
//     그 뒤 이 모듈을 Require한 파일을 따라가며 — 모듈은 캐시를 지워 다음 Require에 다시 실행하고, 클래스 파일은 클래스 핫 리로드와 같이 다시 실행한다
//     (그래서 파일 맨 위 `local Items = Script.Require(...)`가 새 값을 받는다). 의존 기록 = 파일 실행 중이면 그 파일, 아니면 실행 중인 인스턴스의 클래스
//   - 모듈 값은 Lua 상태 안 공유 테이블이므로 한 인스턴스가 고치면 모두에게 보인다 (상태는 인스턴스/GameManager에 두고 모듈은 정의·함수 위주로)

std::string FLuaRuntime::GetRequireDependent() const
{
	if (!ExecutingFiles.empty())
	{
		return ExecutingFiles.back();
	}
	if (CurrentInstance.IsValid())
	{
		if (const auto Found = Instances.find(CurrentInstance.ToId()); Found != Instances.end())
		{
			return Found->second.ClassKey;
		}
	}
	return std::string();
}

bool FLuaRuntime::ExecuteModuleFile(const std::string& Key, const std::string& AssetName, const std::filesystem::path& AbsolutePath,
                                    sol::object& OutValue, std::string& OutError)
{
	bool              bRead  = false;
	const std::string Source = ReadScriptSource(AbsolutePath, bRead);
	if (!bRead)
	{
		OutError = "모듈 파일을 열 수 없습니다: " + AssetName;
		return false;
	}
	sol::load_result Chunk = Lua.load(Source, "@" + AssetName);
	if (!Chunk.valid())
	{
		const sol::error Error = Chunk;
		OutError               = Error.what();
		return false;
	}
	// 반환값이 0개인 파일도 정확히 1개(nil)로 받도록 감싸서 부른다 (오류 처리기와 결과 수 계산이 클래스 파일과 같아진다)
	const sol::function Function = Chunk.get<sol::function>();
	ExecutingFiles.push_back(Key);
	sol::protected_function_result Result = ModuleCaller(Function);
	ExecutingFiles.pop_back();
	if (!Result.valid())
	{
		const sol::error Error = Result;
		OutError               = Error.what();
		return false;
	}
	sol::object Returned = Result;
	OutValue             = Returned.get_type() == sol::type::lua_nil ? sol::make_object(Lua, true) : Returned;
	return true;
}

sol::object FLuaRuntime::RequireModule(const std::string& AssetPath)
{
	if (AssetPath.empty())
	{
		throw std::runtime_error("Script.Require: 경로가 비었습니다");
	}
	const std::filesystem::path Relative = std::filesystem::path(FStringConv::ToWide(AssetPath)).lexically_normal();
	if (Relative.is_absolute() || Relative.has_root_name() || Relative.has_root_directory() || (!Relative.empty() && *Relative.begin() == L".."))
	{
		throw std::runtime_error("Script.Require: Content 기준 상대 경로만 쓸 수 있습니다: " + AssetPath);
	}
	const std::filesystem::path AbsolutePath = ContentDirectory / Relative;
	const std::string           Key          = MakeClassKey(AbsolutePath);

	// 의존 기록 (핫 리로드 전파용)
	if (const std::string Dependent = GetRequireDependent(); !Dependent.empty() && Dependent != Key)
	{
		std::vector<std::string>& Dependents = ModuleDependents[Key];
		if (std::find(Dependents.begin(), Dependents.end(), Dependent) == Dependents.end())
		{
			Dependents.push_back(Dependent);
		}
	}

	if (const auto Found = Modules.find(Key); Found != Modules.end())
	{
		if (!Found->second.bLoading)
		{
			return Found->second.Value;
		}
		// 순환: 실행 스택에서 이 모듈부터 지금까지
		std::string Chain;
		const auto  Start = std::find(ExecutingFiles.begin(), ExecutingFiles.end(), Key);
		for (auto It = Start; It != ExecutingFiles.end(); ++It)
		{
			const auto Module = Modules.find(*It);
			Chain += (Module != Modules.end() ? Module->second.AssetName : *It) + " → ";
		}
		throw std::runtime_error("Script.Require 순환 참조: " + Chain + AssetPath);
	}

	FScriptModule& Module = Modules[Key];
	Module.AssetName      = AssetPath;
	Module.bLoading       = true;
	sol::object Value;
	std::string Error;
	const bool  bOk = ExecuteModuleFile(Key, AssetPath, AbsolutePath, Value, Error);
	if (!bOk)
	{
		Modules.erase(Key); // 실패는 캐시하지 않는다 (참조 무효 — Module을 더 쓰지 않는다)
		throw std::runtime_error(std::format("Script.Require 실패: {}\n{}", AssetPath, Error));
	}
	FScriptModule& Loaded = Modules[Key];
	Loaded.Value          = Value;
	Loaded.bLoading       = false;
	return Value;
}

bool FLuaRuntime::ReloadModule(const std::string& Key, const std::filesystem::path& ScriptPath, bool& bOutSucceeded)
{
	bOutSucceeded = false;
	const auto Found = Modules.find(Key);
	if (Found == Modules.end())
	{
		return false;
	}
	if (Found->second.bLoading)
	{
		return true; // 실행 중 (모듈 코드 안에서 핫 리로드가 불릴 일은 없지만 안전하게)
	}
	const std::string AssetName = Found->second.AssetName;
	Found->second.bLoading      = true;
	sol::object Value;
	std::string Error;
	const bool  bOk = ExecuteModuleFile(Key, AssetName, ScriptPath, Value, Error);
	FScriptModule& Module = Modules[Key];
	Module.bLoading       = false;
	if (!bOk)
	{
		ReportError(std::format("모듈 다시 로드 실패 (기존 값 유지): {}\n{}", AssetName, Error));
		return true;
	}
	Module.Value  = Value;
	bOutSucceeded = true;
	E_LOG(LogScript, Display, "모듈 다시 로드: {}", AssetName);

	// 의존 파일 전파 (너비 우선, 한 번씩): 모듈은 캐시 제거, 클래스는 다시 실행
	std::unordered_set<std::string> Visited{ Key };
	std::deque<std::string>         Queue;
	std::vector<std::string>        ClassesToReload;
	for (const std::string& Dependent : ModuleDependents[Key])
	{
		Queue.push_back(Dependent);
	}
	while (!Queue.empty())
	{
		const std::string Current = Queue.front();
		Queue.pop_front();
		if (!Visited.insert(Current).second)
		{
			continue;
		}
		if (Modules.erase(Current) > 0)
		{
			if (const auto Next = ModuleDependents.find(Current); Next != ModuleDependents.end())
			{
				Queue.insert(Queue.end(), Next->second.begin(), Next->second.end());
			}
		}
		else if (Classes.contains(Current))
		{
			ClassesToReload.push_back(Current);
		}
	}
	for (const std::string& ClassKey : ClassesToReload)
	{
		const auto Class = Classes.find(ClassKey);
		if (Class == Classes.end())
		{
			continue;
		}
		bool bClassSucceeded = false;
		ReloadClassByKey(ClassKey, ContentDirectory / FStringConv::ToWide(Class->second->AssetName), bClassSucceeded);
		bOutSucceeded = bOutSucceeded && bClassSucceeded;
	}
	return true;
}

void FLuaRuntime::RegisterModuleBindings()
{
	sol::load_result Caller = Lua.load("local Chunk = ...; return (Chunk())", "=Script.Require");
	ModuleCaller            = sol::protected_function(Caller.get<sol::function>(), Traceback);
	sol::table ScriptTable = Lua.create_named_table("Script");
	ScriptTable["Require"] = [this](const std::string& AssetPath) { return RequireModule(AssetPath); };
}
