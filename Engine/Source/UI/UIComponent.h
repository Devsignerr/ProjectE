#pragma once

#include "UI/UIInput.h"

#include <memory>
#include <string>
#include <vector>

class FUIInstance;

// 실행 중 상태 (직렬화/리플렉션 제외). 복사하면 비어 있는 상태가 된다 — 플레이 모드 복제(FSceneCloner)나
// 엔티티 복제가 인스턴스를 공유하지 않고 각자 에셋에서 새로 만들게 하기 위해서다.
struct FUIComponentRuntime
{
	std::shared_ptr<FUIInstance> Instance;
	std::string                  LoadedAsset; // Instance를 만든 에셋 경로 (바뀌면 다시 만든다)
	std::vector<FUIEvent>        Events;      // 이번 프레임 이벤트 (FUISystem::Update가 비우고 채움 → 스크립트/게임 모듈이 읽음)
	bool                         bPointerOver = false;
	// 이번 프레임 마우스 커서 (화면 픽셀 — 이 UI 뷰포트와 같은 좌표계, FUISystem::Update가 입력 모드와 무관하게 채운다).
	// Lua Input.GetMouseUIPosition이 레이아웃 좌표로 바꾼다 (Camera.WorldToScreen과 같은 공간)
	FVector2                     CursorPixels;
	bool                         bCursorInside = false; // 커서가 화면(에디터: 플레이 뷰포트 이미지) 안

	FUIComponentRuntime() = default;
	FUIComponentRuntime(const FUIComponentRuntime&) {}
	FUIComponentRuntime& operator=(const FUIComponentRuntime&) { return *this; }
	FUIComponentRuntime(FUIComponentRuntime&&) noexcept            = default;
	FUIComponentRuntime& operator=(FUIComponentRuntime&&) noexcept = default;
};

// 화면 UI 하나 (.eui). 플레이 중(런타임/에디터 플레이)에 화면 전체 위에 그려진다. 여러 개면 ZOrder가 큰 것이 위이고 입력도 먼저 받는다.
// 이벤트는 같은 엔티티(없으면 가장 가까운 조상)의 스크립트 OnUIClicked_<위젯 이름> 등으로 전달된다.
struct FUIComponent
{
	std::string Asset;               // Content 기준 .eui 경로
	int32       ZOrder         = 0;
	bool        bVisible       = true;  // false면 그리지도 입력을 받지도 않는다
	bool        bReceiveInput  = true;  // false면 그리기만 (HUD)
	bool        bKeyboardFocus = false; // Tab/Enter/Space로 버튼 포커스·실행 (메뉴 화면). 게임 키와 겹치므로 기본 꺼짐

	FUIComponentRuntime Runtime;
};
