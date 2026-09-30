#pragma once

#include "UI/UIAsset.h"
#include "UI/UIDrawList.h"
#include "UI/UIInput.h"

#include <vector>

class FUIFontLibrary;

// 실행 중인 UI 하나: 에셋 복제본 트리 + 배율 + 입력 상태. 게임(스크립트/C++)은 이 트리의 위젯 값을 바꾼다.
// 프레임 순서: Update(레이아웃 → 입력 → 이벤트) → Paint(그리기 목록)
class FUIInstance
{
public:
	FUIInstance();
	explicit FUIInstance(const FUIAsset& Asset);

	void SetAsset(const FUIAsset& Asset);

	FUIWidget&       GetRoot() { return *Asset.Root; }
	const FUIWidget& GetRoot() const { return *Asset.Root; }
	const FUIAsset&  GetAsset() const { return Asset; }
	FUIWidget*       FindWidget(std::string_view Name) { return Asset.Root->FindByName(Name); }

	// Viewport: UI를 놓을 화면 픽셀 영역. Pointer는 화면 픽셀 기준 (nullptr이면 입력 없음). 반환: 포인터가 UI 위에 있음
	bool Update(const FUIRect& Viewport, const FUIPointerInput* PointerPixels, const FUIKeyInput* Keys, FUIFontLibrary& Fonts,
	            std::vector<FUIEvent>& OutEvents, float DeltaSeconds = 0.0f);
	// 레이아웃만 (입력 없이)
	void Layout(const FUIRect& Viewport, FUIFontLibrary& Fonts);
	void Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const;

	const FUITransform& GetTransform() const { return Transform; }
	const FUIRect&      GetViewport() const { return Viewport; }
	FUIInputRouter&     GetInputRouter() { return Router; }
	bool                IsPointerOverUI() const { return bPointerOver; }
	// ---- 애니메이션 (에셋 Animations). 같은 이름을 다시 재생하면 처음부터. Loops 0 = 무한, Speed 음수 = 거꾸로
	bool PlayAnimation(std::string_view Name, int32 Loops = 1, float Speed = 1.0f);
	void StopAnimation(std::string_view Name); // 현재 값에 멈춘다
	void StopAllAnimations();
	bool IsAnimationPlaying(std::string_view Name) const;
	// 재생 진행 (Update가 부른다). 끝난 애니메이션은 AnimationFinished 이벤트 (WidgetName = 애니메이션 이름)
	void TickAnimations(float DeltaSeconds, std::vector<FUIEvent>& OutEvents);

	// 포커스된 텍스트 상자가 있어 키보드를 가져감
	bool                WantsKeyboard() { return Router.WantsKeyboard(*Asset.Root); }

private:
	FUIAsset       Asset;
	FUIInputRouter Router;
	FUITransform   Transform;
	FUIRect        Viewport;
	bool           bPointerOver = false;

	std::vector<FUIAnimationPlayback> Playing;
};
