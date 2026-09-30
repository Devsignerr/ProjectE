#pragma once

#include "UI/UIAnimation.h"
#include "UI/Widget.h"

#include <filesystem>
#include <memory>
#include <string>

// .eui 파일 (JSON, UMG 위젯 블루프린트에 해당하는 UI 레이아웃). 텍스처/글꼴 경로는 Content 기준.
//   { "Version": 1, "DesignSize": [1920, 1080], "ScaleMode": "MatchHeight",
//     "Root": { "Type": "Canvas", "Name": "Root", ..., "Children": [ { "Type": "Button", "Slot": { ... }, ... } ] },
//     "Animations": [ { "Name": "Intro", "Length": 0.5, "Tracks": [ { "Widget": "Menu", "Property": "Opacity", "Keys": [[0, 0, "EaseOut"], [0.5, 1, "Linear"]] } ] } ] }
// 위젯마다 공통 값 + 종류별 값 + 부모 종류에 맞는 "Slot"을 쓴다. 없는 키는 종류별 기본값(FUIWidget::Create).
struct FUIAsset
{
	static constexpr const wchar_t* Extension = L".eui";
	static constexpr int32          Version   = 2; // 2: 애니메이션 + 렌더 변환 (1은 그대로 읽힌다)

	FVector2                   DesignSize = FVector2(1920.0f, 1080.0f);
	EUIScaleMode               ScaleMode  = EUIScaleMode::MatchHeight;
	std::unique_ptr<FUIWidget> Root;
	std::vector<FUIAnimation>  Animations;

	FUIAnimation*       FindAnimation(std::string_view Name);
	const FUIAnimation* FindAnimation(std::string_view Name) const;

	FUIAsset();
	FUIAsset(FUIAsset&&) noexcept            = default;
	FUIAsset& operator=(FUIAsset&&) noexcept = default;

	// 빈 캔버스 루트 하나
	static FUIAsset MakeDefault();
	FUIAsset        Clone() const;

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json);
	bool        LoadFromFile(const std::filesystem::path& Path);
	bool        SaveToFile(const std::filesystem::path& Path) const;

	// 위젯 하위 트리 ↔ JSON 문자열 (에디터 복사/붙여넣기). 슬롯도 함께 저장한다
	static std::string                WidgetToJsonString(const FUIWidget& Widget);
	static std::unique_ptr<FUIWidget> WidgetFromJsonString(const std::string& Json);
};
