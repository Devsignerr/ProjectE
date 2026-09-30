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
	            std::vector<FUIEvent>& OutEvents);
	// 레이아웃만 (입력 없이)
	void Layout(const FUIRect& Viewport, FUIFontLibrary& Fonts);
	void Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const;

	const FUITransform& GetTransform() const { return Transform; }
	const FUIRect&      GetViewport() const { return Viewport; }
	FUIInputRouter&     GetInputRouter() { return Router; }
	bool                IsPointerOverUI() const { return bPointerOver; }

private:
	FUIAsset       Asset;
	FUIInputRouter Router;
	FUITransform   Transform;
	FUIRect        Viewport;
	bool           bPointerOver = false;
};
