#pragma once

#include "UI/UIAsset.h"
#include "UI/UIDrawList.h"
#include "UI/UIInput.h"

#include <optional>
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
	            std::vector<FUIEvent>& OutEvents, float DeltaSeconds = 0.0f, float GameTimeScale = 1.0f);
	// 레이아웃만 (입력 없이)
	void Layout(const FUIRect& Viewport, FUIFontLibrary& Fonts);
	void Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const;

	const FUITransform& GetTransform() const { return Transform; }
	const FUIRect&      GetViewport() const { return Viewport; }
	FUIInputRouter&     GetInputRouter() { return Router; }
	bool                IsPointerOverUI() const { return bPointerOver; }
	// ---- 애니메이션 (에셋 Animations). 같은 이름을 다시 재생하면 처음부터. Loops 0 = 무한, Speed 음수 = 거꾸로
	//   UseGameTime: 비면 에셋 값(FUIAnimation::bUseGameTime), 참이면 게임 시간 배율·히트스톱을 따른다 (TickAnimations GameTimeScale)
	bool PlayAnimation(std::string_view Name, int32 Loops = 1, float Speed = 1.0f, std::optional<bool> UseGameTime = std::nullopt);
	void StopAnimation(std::string_view Name); // 현재 값에 멈춘다
	void StopAllAnimations();
	bool IsAnimationPlaying(std::string_view Name) const;
	// 재생 진행 (Update가 부른다). 끝난 애니메이션은 AnimationFinished 이벤트 (WidgetName = 애니메이션 이름)
	//   DeltaSeconds = 실제 시간, 게임 시간 재생은 × GameTimeScale (게임 시간 배율 — 히트스톱이면 0)
	void TickAnimations(float DeltaSeconds, std::vector<FUIEvent>& OutEvents, float GameTimeScale = 1.0f);

	// ---- 실행 중 트리 편집 (스크립트 entity:CloneWidget/RemoveWidget). 규칙:
	//   CloneWidget: 템플릿(루트가 아닌 위젯)을 자식까지 깊은 복사해 템플릿의 부모 끝에 붙인다. 복제 루트 이름 = NewName,
	//                자손 이름 = NewName + "." + 원래 이름 (빈 이름은 그대로). 이름이 이미 있거나 부모가 자식을 더 받을 수 없으면 nullptr + OutError.
	//                값(보이기 포함)은 템플릿 그대로 — 숨겨 둔 템플릿이면 복제본도 숨겨져 있다. 애니메이션 트랙은 원래 이름만 대상
	//   RemoveWidget: 이름으로 찾은 위젯(루트 제외)과 자손을 뗀다. 없으면 false
	//   위젯 번호는 이 인스턴스 안에서 다시 쓰지 않는다 (입력 상태가 지워진 위젯 번호를 가리켜도 다른 위젯과 섞이지 않음)
	FUIWidget* CloneWidget(std::string_view TemplateName, const std::string& NewName, std::string& OutError);
	bool       RemoveWidget(std::string_view Name);
	// 스크립트가 레이아웃에 영향을 주는 값을 바꿈 → FUISystem::Paint가 그리기 전에 같은 뷰포트로 다시 레이아웃 (한 프레임 늦지 않게)
	void MarkLayoutDirty() { bLayoutDirty = true; }
	bool IsLayoutDirty() const { return bLayoutDirty; }

	// 포커스된 텍스트 상자가 있어 키보드를 가져감
	bool                WantsKeyboard() { return Router.WantsKeyboard(*Asset.Root); }
	// 포커스된 텍스트 상자의 캐럿 영역 (화면 픽셀, IME 조합 글자 포함). 없으면 false
	bool                GetTextCaretPixels(FUIRect& Out, FUIFontLibrary& Fonts);

private:
	FUIAsset       Asset;
	FUIInputRouter Router;
	FUITransform   Transform;
	FUIRect        Viewport;
	bool           bPointerOver = false;
	bool           bLayoutDirty = false;
	uint32         NextWidgetId = 1; // 복제 위젯에 줄 다음 번호 (SetAsset 때 최대 번호 + 1)

	std::vector<FUIAnimationPlayback> Playing;
};
