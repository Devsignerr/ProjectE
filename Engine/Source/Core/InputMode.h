#pragma once

#include "Core/CoreTypes.h"

#include <string_view>

class FInput;

// 입력 모드 (언리얼 SetInputMode식) — 이번 프레임 입력을 게임 UI와 게임 로직에 어떻게 나눌지
//   GameOnly : 게임이 모든 입력을 받는다. 게임 UI는 그려지지만 포인터/키보드를 받지 않는다. 모드에 들어갈 때 커서 숨김 + 창에 가둠
//   GameAndUI: (기본, Phase 44 이전 동작) 게임 UI가 먼저 보고, 가져간 포인터/키보드만 게임에서 뺀다. 커서 보임
//   UIOnly   : 게임 UI만 입력을 받는다. 게임에는 빈 입력(키/마우스/시점/액션/게임패드 없음). 커서 보임
// 번호는 저장하지 않는다(스크립트는 이름 사용). 추가는 끝에.
enum class EInputMode : uint8
{
	GameOnly,
	GameAndUI,
	UIOnly,
};

const char* ToString(EInputMode Mode);
// "GameOnly" / "GameAndUI" / "UIOnly" (대소문자 무시). 모르는 이름이면 false
bool TryParseInputMode(std::string_view Name, EInputMode& OutMode);

// 모드별 입력 배분 규칙 (순수)
struct FInputModeRouting
{
	bool bUIInput    = true;  // 게임 UI가 포인터/키보드를 받는다
	bool bGameInput  = true;  // 게임 로직이 입력을 받는다 (UI가 가져간 부분은 뺀다)
	bool bLockCursor = false; // 모드에 들어갈 때 커서를 숨기고 가둔다 (이후 Game.SetMouseLocked로 바꿀 수 있다)
};
FInputModeRouting GetInputModeRouting(EInputMode Mode);

// 게임 로직에 넘길 입력을 고른다 (런타임/에디터 플레이 공용 — 순수).
//   bUIPointer/bUIKeyboard = 게임 UI가 이번 프레임에 포인터/키보드를 가져갔는가 (FUIInputResult, 콘솔 포함)
//   반환: Raw 그대로이거나, 필요한 부분을 비운 사본을 Storage에 만들어 그 참조 (Storage는 반환값보다 오래 살아야 한다)
const FInput& SelectGameInput(EInputMode Mode, const FInput& Raw, bool bUIPointer, bool bUIKeyboard, FInput& Storage);

// 현재 입력 모드 (프로세스 전역 하나 — 엔진 DLL 안에 상태가 있다. 메인 스레드 전용).
//   바꾸는 쪽: Lua Game.SetInputMode / 게임 모듈(C++) FInputModeState::Set
//   적용하는 쪽: 런타임/에디터 앱이 매 프레임 Get()으로 입력을 나누고, GetRevision()이 바뀌면 커서 기본값(잠금)을 다시 적용한다
//   초기화: FGameWorld::BeginPlay/EndPlay가 Reset() → 플레이 시작/정지·맵 전환마다 GameAndUI로 돌아간다 (각 맵 스크립트가 OnStart에서 정함)
//   멀티플레이: 로컬 화면 상태이므로 클라이언트에서 도는 스크립트(ClientOnly/Both)나 리슨 서버 호스트에서 부른다
class FInputModeState
{
public:
	static EInputMode Get();
	static void       Set(EInputMode Mode); // 같은 모드여도 리비전이 오른다 (커서 잠금 다시 적용)
	static void       Reset();              // GameAndUI
	static uint32     GetRevision();        // Set/Reset마다 1씩 오른다
};
