#pragma once

#include "UI/UITypes.h"

#include <string>
#include <vector>

class IUITextMeasurer;

struct FUIWidget;

// 한 프레임의 포인터 입력 (위치는 UI 단위)
struct FUIPointerInput
{
	FVector2 Position;
	bool     bInside   = false; // 포인터가 UI 영역 안
	bool     bDown     = false; // 왼쪽 버튼 누르고 있음
	bool     bPressed  = false; // 이번 프레임에 눌림
	bool     bReleased = false; // 이번 프레임에 뗌
	float    Wheel     = 0.0f;  // 휠 칸 수 (+ = 위로)
};

// 키보드: 탐색(포커스된 버튼 실행, Tab 이동) + 텍스트 상자 편집
struct FUIKeyInput
{
	bool bActivate      = false; // Enter/Space 눌림 (버튼 실행)
	bool bFocusNext     = false; // Tab
	bool bFocusPrevious = false; // Shift+Tab

	// 텍스트 상자 (포커스된 텍스트 상자가 있을 때만 쓰인다)
	std::u32string Typed;              // 입력 문자 (제어 문자 제외)
	bool           bBackspace = false; // 반복 포함
	bool           bDelete    = false;
	bool           bLeft      = false;
	bool           bRight     = false;
	bool           bHome      = false;
	bool           bEnd       = false;
	bool           bCommit    = false; // Enter
	bool           bCancel    = false; // Esc

	bool HasTextEdit() const { return !Typed.empty() || bBackspace || bDelete || bLeft || bRight || bHome || bEnd || bCommit || bCancel; }
};

enum class EUIEventType : uint8
{
	Clicked = 0, // 누른 버튼 위에서 뗌, 또는 포커스된 버튼을 키보드로 실행
	Pressed,
	Released,
	HoverBegin,
	HoverEnd,
	TextChanged,   // 텍스트 상자 내용이 바뀜
	TextCommitted, // 텍스트 상자 Enter 또는 포커스를 잃음
	AnimationFinished, // UI 애니메이션 재생 끝 (WidgetName = 애니메이션 이름)
};

struct FUIEvent
{
	EUIEventType Type     = EUIEventType::Clicked;
	uint32       WidgetId = 0;
	std::string  WidgetName;
};

// 포인터/키보드 → 위젯 상태(호버/눌림/포커스)와 이벤트. 레이아웃(Geometry/Clip)이 끝난 트리에 적용한다.
//   맞히기: 보이는(Visible) 위젯 중 가장 위 (자식이 부모보다, 캔버스는 ZOrder 큰 쪽이 위). 입력 불가(HitTestInvisible)는 자식까지 제외,
//   SelfHitTestInvisible은 자신만 제외. 포인터가 맞힌 위젯이 있으면 UI가 입력을 가져간다(게임으로 넘기지 않음).
//   버튼: 맞힌 위젯 또는 가장 가까운 버튼 조상. 휠: 가장 가까운 스크롤 박스 조상.
class FUIInputRouter
{
public:
	static constexpr float WheelStep = 48.0f; // 휠 한 칸 스크롤 (UI 단위)

	// 반환: 포인터가 UI 위에 있거나 UI 버튼을 누르고 있음 (게임 포인터 입력을 막아야 함)
	bool Process(FUIWidget& Root, const FUIPointerInput& Pointer, const FUIKeyInput& Keys, std::vector<FUIEvent>& OutEvents);

	// 가장 위의 맞힌 위젯 (없으면 nullptr)
	static FUIWidget* HitTest(FUIWidget& Root, const FVector2& Point);

	uint32 GetHoveredId() const { return HoveredId; }
	uint32 GetPressedId() const { return PressedId; }
	uint32 GetFocusedId() const { return FocusedId; }
	void   SetFocus(FUIWidget& Root, uint32 WidgetId);
	// 포커스가 텍스트 상자면 true (게임 키보드 입력을 막아야 함)
	bool   WantsKeyboard(FUIWidget& Root);
	void   Reset(FUIWidget& Root);

private:
	uint32 HoveredId = 0;
	uint32 PressedId = 0;
	uint32 FocusedId = 0;
};
