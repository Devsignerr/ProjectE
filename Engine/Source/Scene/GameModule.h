#pragma once

#include "Core/CoreTypes.h"

class FScene;
struct FAnimNotifyEvent;

// 게임 모듈 인터페이스. 프로젝트의 C++ 게임 코드(<프로젝트>/Source → <이름>.dll)가 구현한다.
// 게임 모듈은 엔진 DLL(ProjectEEngine.dll)을 링크하고 같은 컴파일러/CRT로 빌드되어야 한다 (STL 객체가 경계를 넘는다).
//   OnLoad:      로드 직후 한 번. 게임 컴포넌트를 FTypeRegistry에 등록하면 인스펙터/직렬화/Lua에 자동 노출된다
//   OnBeginPlay: 게임 시작 (런타임 씬 로드 후, 에디터 플레이 시작 — 플레이 씬)
//   OnUpdate:    매 프레임 게임 C++ 시스템 (스크립트 뒤, 애니메이션/물리 앞). 대량 순회는 여기(C++)에 둔다
//   OnAnimNotify: 애니메이션 노티파이 (Scene/AnimNotify.h). 플레이 중, 발생 다음 프레임 OnUpdate 직전에 이벤트마다 한 번
//   OnEndPlay:   게임 종료 (에디터 정지 포함)
//   OnUnload:    언로드 직전 (등록 타입은 호스트가 제거한다)
class IGameModule
{
public:
	virtual ~IGameModule() = default;

	virtual void OnLoad() {}
	virtual void OnBeginPlay(FScene& /*Scene*/) {}
	virtual void OnUpdate(FScene& /*Scene*/, float /*DeltaSeconds*/) {}
	virtual void OnAnimNotify(FScene& /*Scene*/, const FAnimNotifyEvent& /*Event*/) {}
	virtual void OnEndPlay(FScene& /*Scene*/) {}
	virtual void OnUnload() {}
};

// 게임 모듈과 엔진이 약속한 인터페이스 버전 (IGameModule 가상 함수 구성이 바뀌면 올린다)
inline constexpr uint32 GameModuleApiVersion = 2; // 2: OnAnimNotify 추가

// 게임 모듈 .cpp 하나에 한 번: E_IMPLEMENT_GAME_MODULE(FMyGameModule)
#define E_IMPLEMENT_GAME_MODULE(ModuleClass)                                                   \
	extern "C" __declspec(dllexport) uint32 ProjectE_GetGameModuleApiVersion()               \
	{                                                                                          \
		return GameModuleApiVersion;                                                           \
	}                                                                                          \
	extern "C" __declspec(dllexport) IGameModule* ProjectE_CreateGameModule()                \
	{                                                                                          \
		static ModuleClass Instance;                                                           \
		return &Instance;                                                                      \
	}
