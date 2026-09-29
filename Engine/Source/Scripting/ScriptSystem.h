#pragma once

#include "Core/ECS/Entity.h"
#include "Scripting/ScriptValue.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class FInput;
class FLuaRuntime;
class FScene;

// Lua 스크립트 컴포넌트(FScriptComponent) 실행 시스템.
//
// 스크립트 형식 (Unity 스타일 테이블 반환):
//   local Rotator = { Properties = { Speed = 90.0 } }  -- 기본값 선언 (인스펙터에 표시, 씬에서 덮어쓰기)
//   function Rotator:OnStart() end                     -- 첫 OnUpdate 직전 한 번
//   function Rotator:OnUpdate(dt) end                  -- 매 프레임
//   function Rotator:OnDestroy() end                   -- 엔티티/컴포넌트 제거 또는 플레이 종료 시
//   return Rotator
// 인스턴스(self)마다 self.entity(엔티티 핸들)와 self.Properties(기본값 + 오버라이드 복사본)가 있다.
//
// 플레이 세션마다 새 Lua 상태를 만든다 (전역 상태가 세션 사이에 남지 않음, Unity의 도메인 리로드와 같은 효과).
// 스크립트 오류는 로그로 남기고 해당 인스턴스만 멈춘다 (핫 리로드 시 재개). 절대 크래시하지 않는다.
// 성능: 매 프레임 대량 순회는 C++ 시스템에 둔다. 스크립트 호출은 인스턴스당 OnUpdate 한 번.
class FScriptSystem
{
public:
	FScriptSystem();
	~FScriptSystem();

	FScriptSystem(const FScriptSystem&)            = delete;
	FScriptSystem& operator=(const FScriptSystem&) = delete;

	// ScriptAsset 상대 경로의 기준 (프로젝트 Content 디렉터리)
	void                         SetContentDirectory(const std::filesystem::path& Directory);
	const std::filesystem::path& GetContentDirectory() const { return ContentDirectory; }

	// ---- 플레이 세션
	// Scene은 EndPlay까지 살아 있어야 한다 (비소유)
	bool BeginPlay(FScene& Scene);
	// 새 스크립트 컴포넌트 인스턴스화(+OnStart) → OnUpdate → 지연 삭제 적용. Input은 nullptr 허용 (입력 없음)
	void Update(float DeltaSeconds, const FInput* Input);
	// 모든 인스턴스 OnDestroy 후 Lua 상태 파괴
	void EndPlay();
	bool IsPlaying() const { return PlayRuntime != nullptr; }
	// 스크립트가 엔티티/컴포넌트를 추가·제거했는가 (호출 시 초기화). true면 앱이 에셋 참조(메시 등)를 다시 해석한다
	bool ConsumeSceneStructureChanged();

	// ---- 핫 리로드: 변경된 .lua 파일 (절대 경로). 실패하면 기존 스크립트를 유지한다. 반환: 성공 여부
	bool ReloadScript(const std::filesystem::path& ScriptPath);

	// ---- 에디터: 스크립트가 선언한 Properties (이름순). 로드 실패 시 nullptr + OutError
	const std::vector<FScriptPropertyDecl>* GetPropertyDecls(const std::string& ScriptAsset, std::string* OutError = nullptr);

	// ---- 테스트/디버그
	bool         RunString(std::string_view Code);                             // 플레이 상태에서 Lua 코드 실행 (오류는 로그 + false)
	size_t       GetInstanceCount() const;                                     // 살아 있는 스크립트 인스턴스 수
	FScriptValue GetInstanceProperty(FEntity Entity, const std::string& Name); // self.Properties[Name]
	uint32       GetErrorCount() const { return ErrorCount; }                  // 누적 스크립트 오류 수

private:
	std::filesystem::path        ContentDirectory;
	std::unique_ptr<FLuaRuntime> PlayRuntime;  // 플레이 중에만 존재
	std::unique_ptr<FLuaRuntime> EditorRuntime; // 프로퍼티 선언 조회용 (씬 없음, 게임 로직 실행 안 함)
	uint32                       ErrorCount = 0;
};
