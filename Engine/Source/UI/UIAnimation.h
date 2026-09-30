#pragma once

#include "UI/UITypes.h"

#include <string>
#include <vector>

struct FUIWidget;

// UI 애니메이션 (UMG 위젯 애니메이션에 해당). 에셋(.eui)에 이름으로 저장되고 FUIInstance가 재생한다.
// 트랙 = (위젯 이름, 속성) 하나의 스칼라 곡선. 값은 재생 중 위젯 값에 그대로 쓰인다 (정지하면 그 값에 머문다).

// 애니메이션할 수 있는 속성 (파일에는 이름으로 저장 — 끝에만 추가)
enum class EUIAnimProperty : uint8
{
	Opacity = 0,  // RenderOpacity
	TranslationX, // 렌더 이동 (UI 단위, 레이아웃에 영향 없음)
	TranslationY,
	ScaleX, // 렌더 배율 (피벗 기준)
	ScaleY,
	ColorR, // 대표 색 (텍스트 = 글자 색, 진행 막대 = 채우기, 나머지 = 배경 브러시) sRGB 0~1
	ColorG,
	ColorB,
	ColorA,
	Percent, // 진행 막대 비율
	Count
};

// 키 → 다음 키 사이 보간
enum class EUIAnimInterp : uint8
{
	Linear = 0,
	Constant, // 다음 키까지 값 유지
	EaseIn,
	EaseOut,
	EaseInOut,
	Count
};

const char* ToString(EUIAnimProperty Value);
bool        FromString(std::string_view Text, EUIAnimProperty& Out);
const char* ToString(EUIAnimInterp Value);
bool        FromString(std::string_view Text, EUIAnimInterp& Out);

struct FUIAnimKey
{
	float         Time   = 0.0f; // 초
	float         Value  = 0.0f;
	EUIAnimInterp Interp = EUIAnimInterp::Linear;

	bool operator==(const FUIAnimKey& Other) const = default;
};

struct FUIAnimTrack
{
	std::string             Widget; // 대상 위젯 이름
	EUIAnimProperty         Property = EUIAnimProperty::Opacity;
	std::vector<FUIAnimKey> Keys;   // 시간 오름차순 (SortKeys)

	// 시간 t의 값: 첫 키 전 = 첫 값, 마지막 키 뒤 = 마지막 값. 키가 없으면 false
	bool Evaluate(float Time, float& OutValue) const;
	// 같은 시간(허용 1ms)이면 값만 바꾸고, 아니면 추가 후 정렬. 반환: 키 번호
	int32 SetKey(float Time, float Value, EUIAnimInterp Interp = EUIAnimInterp::Linear);
	void  SortKeys();

	bool operator==(const FUIAnimTrack& Other) const = default;
};

struct FUIAnimation
{
	std::string               Name;
	float                     Length = 1.0f; // 초 (재생 끝)
	std::vector<FUIAnimTrack> Tracks;

	FUIAnimTrack*       FindTrack(std::string_view Widget, EUIAnimProperty Property);
	const FUIAnimTrack* FindTrack(std::string_view Widget, EUIAnimProperty Property) const;
	FUIAnimTrack&       GetOrAddTrack(std::string_view Widget, EUIAnimProperty Property);
	// 마지막 키 시간 (키가 없으면 0)
	float GetLastKeyTime() const;
	// 트리에 시간 t 값을 쓴다 (없는 위젯은 건너뜀)
	void ApplyAt(FUIWidget& Root, float Time) const;

	bool operator==(const FUIAnimation& Other) const = default;
};

struct FUIAnimMath
{
	// 0~1 구간 보간 가중치
	static float Ease(EUIAnimInterp Interp, float Alpha);
	// 위젯의 속성 값 읽기/쓰기
	static float GetProperty(const FUIWidget& Widget, EUIAnimProperty Property);
	static void  SetProperty(FUIWidget& Widget, EUIAnimProperty Property, float Value);
	// 대표 색 (텍스트 = 글자, 진행 막대 = 채우기, 나머지 = 배경 브러시) — Lua Color와 같은 규칙
	static FVector4&       GetMainColor(FUIWidget& Widget);
	static const FVector4& GetMainColor(const FUIWidget& Widget);
};

// 재생 중인 애니메이션 하나 (FUIInstance가 소유)
struct FUIAnimationPlayback
{
	int32 AnimationIndex = -1;
	float Time           = 0.0f;
	float Speed          = 1.0f;
	int32 LoopsRemaining = 1; // 0 = 무한
};
