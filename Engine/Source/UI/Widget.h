#pragma once

#include "UI/UITypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// 부모 안에서의 배치 정보. 어떤 항목을 쓰는지는 부모 종류가 정한다 (UMG 슬롯).
//   캔버스: Anchor/Offsets/Alignment/bAutoSize/ZOrder
//   가로/세로 박스, 스크롤: Padding/HAlign/VAlign/SizeRule/FillWeight (스크롤은 SizeRule 무시)
//   오버레이/보더/버튼: Padding/HAlign/VAlign
//   균일 그리드: Row/Column/Padding/HAlign/VAlign
struct FUISlot
{
	// ---- 캔버스
	// 축마다 AnchorMin == AnchorMax면 Offsets = (위치 X, 위치 Y, 너비, 높이) — 위치는 앵커점 기준, Alignment가 피벗.
	// 다르면(늘이기) 그 축의 Offsets = (왼쪽 여백, 위 여백, 오른쪽 여백, 아래 여백)이며 Alignment는 쓰지 않는다.
	FVector2  AnchorMin = FVector2(0.0f, 0.0f);
	FVector2  AnchorMax = FVector2(0.0f, 0.0f);
	FUIMargin Offsets   = FUIMargin(0.0f, 0.0f, 100.0f, 40.0f);
	FVector2  Alignment = FVector2(0.0f, 0.0f);
	bool      bAutoSize = false; // 앵커가 점인 축의 크기를 원하는 크기로 (Offsets 너비/높이 무시)
	int32     ZOrder    = 0;     // 큰 값이 위에 그려진다 (같으면 순서대로)

	// ---- 박스/오버레이/그리드/스크롤/보더/버튼
	FUIMargin   Padding;
	EUIHAlign   HAlign     = EUIHAlign::Fill;
	EUIVAlign   VAlign     = EUIVAlign::Fill;
	EUISizeRule SizeRule   = EUISizeRule::Auto;
	float       FillWeight = 1.0f;

	// ---- 균일 그리드
	int32 Row    = 0;
	int32 Column = 0;

	bool IsAnchorPointX() const { return AnchorMin.X == AnchorMax.X; }
	bool IsAnchorPointY() const { return AnchorMin.Y == AnchorMax.Y; }
	bool operator==(const FUISlot& Other) const = default;
};

// 위젯의 저장되는 값 (복사 가능). 종류와 관계없이 모든 항목을 갖고, 종류별로 쓰는 항목만 의미가 있다.
struct FUIWidgetData
{
	EUIWidgetType Type = EUIWidgetType::Canvas;
	std::string   Name;                // 스크립트/코드에서 찾는 이름 (에셋 안에서 겹치지 않게 에디터가 유지)
	EUIVisibility Visibility = EUIVisibility::Visible;
	bool          bEnabled   = true;   // false면 입력을 받지 않고 버튼은 비활성 모양 (자식에게도 적용)
	float         RenderOpacity = 1.0f; // 자식까지 곱해진다
	FVector2      MinSize;             // 원하는 크기의 최솟값 (0 = 제한 없음)
	FUISlot       Slot;
	// 렌더 변환 (UMG Render Transform): 레이아웃이 끝난 뒤 그리기/맞히기에만 적용, 자식에게 누적. 회전 없음
	FVector2      RenderTranslation;                    // UI 단위
	FVector2      RenderScale = FVector2(1.0f, 1.0f);
	FVector2      RenderPivot = FVector2(0.5f, 0.5f);   // 자기 영역 비율 (배율 중심)

	// ---- 모양: 보더/이미지 배경, 버튼 기본 상태, 진행 막대 배경
	FUIBrush  Brush;
	FUIMargin ContentPadding; // 보더/버튼: 자식까지의 안쪽 여백

	// ---- 이미지
	FVector2 ImageSize = FVector2(64.0f, 64.0f); // 원하는 크기

	// ---- 텍스트
	std::string    Text;
	std::string    Font;            // Content 기준 .ttf/.otf. 비면 기본 글꼴
	float          FontSize = 24.0f; // UI 단위 줄 높이 기준 크기
	FVector4       TextColor    = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	EUITextJustify Justify      = EUITextJustify::Left;
	bool           bWrap        = false; // 영역 너비에서 줄바꿈 (공백 기준, 긴 단어는 글자 단위)
	float          OutlineWidth = 0.0f;  // UI 단위
	FVector4       OutlineColor = FVector4(0.0f, 0.0f, 0.0f, 1.0f);
	FVector2       ShadowOffset;          // (0, 0)이면 그림자 없음
	FVector4       ShadowColor  = FVector4(0.0f, 0.0f, 0.0f, 0.6f);

	// ---- 버튼 (Brush = 기본 상태)
	FUIBrush HoveredBrush;
	FUIBrush PressedBrush;
	FUIBrush DisabledBrush;

	// ---- 진행 막대 (Brush = 배경)
	float            Percent = 0.5f; // 0~1
	FUIBrush         FillBrush;
	EUIFillDirection FillDirection = EUIFillDirection::LeftToRight;

	// ---- 텍스트 상자 (Text/Font/FontSize/TextColor/ContentPadding 공유, Brush = 기본 배경)
	std::string HintText;                                // 비어 있고 포커스가 없을 때 흐리게
	FVector4    HintColor = FVector4(1.0f, 1.0f, 1.0f, 0.35f);
	int32       MaxLength = 0;                           // 글자 수 제한 (0 = 없음)
	FUIBrush    FocusedBrush;                            // 포커스 중 배경

	// ---- 스크롤
	EUIOrientation Orientation      = EUIOrientation::Vertical;
	float          ScrollbarWidth   = 8.0f;
	FVector4       ScrollbarColor   = FVector4(1.0f, 1.0f, 1.0f, 0.35f);

	// ---- 균일 그리드
	FVector2 MinCellSize;
};

// 실행 중에만 쓰는 값 (저장/복제 안 함). 레이아웃/입력이 채운다.
struct FUIWidgetState
{
	uint32   Id = 0;       // 트리 안 고유 번호 (FUIWidget::AssignIds)
	FVector2 DesiredSize;  // 자기 슬롯 여백 제외
	FUIRect  Geometry;     // 배치된 영역 (UI 단위)
	FUIRect  Clip = FUIRect::Infinite(); // 이 위젯이 그려질 때의 잘림 영역 (조상 스크롤 박스, 레이아웃 좌표)
	// 렌더 변환까지 적용한 결과 (그리기/맞히기는 이것을 쓴다). 변환이 없으면 Geometry/Clip과 같다
	FUIRect  VisualGeometry;
	FUIRect  VisualClip   = FUIRect::Infinite();
	FVector2 VisualScale  = FVector2(1.0f, 1.0f); // 레이아웃 좌표 → 화면 UI 좌표: p * VisualScale + VisualOffset
	FVector2 VisualOffset;
	float    ScrollOffset = 0.0f; // 스크롤: 현재 위치 (0 ~ ScrollMax)
	float    ScrollMax    = 0.0f;
	bool     bHovered     = false;
	bool     bPressed     = false;
	bool     bFocused     = false;
	int32    CaretIndex   = 0;    // 텍스트 상자: 캐럿 위치 (코드 포인트)
	float    TextScroll   = 0.0f; // 텍스트 상자: 가로 스크롤 (UI 단위, 캐럿이 보이도록)
	float    CaretTime    = 0.0f; // 깜빡임 (편집하면 0)
};

// 위젯 트리 노드. 자식을 소유하고 부모는 비소유 포인터(자식을 붙일 때 설정, 트리 수명 동안 유효).
struct FUIWidget : FUIWidgetData
{
	std::vector<std::unique_ptr<FUIWidget>> Children;
	FUIWidget*                              Parent = nullptr;
	FUIWidgetState                          State;

	FUIWidget() = default;
	explicit FUIWidget(EUIWidgetType InType) { Type = InType; }

	// 종류별 기본값으로 새 위젯 (이름 = 종류 이름)
	static std::unique_ptr<FUIWidget> Create(EUIWidgetType Type);
	// 종류별 기본 표시 (배치 패널 = SelfHitTestInvisible, 텍스트/진행 막대 = HitTestInvisible, 나머지 Visible)
	static EUIVisibility GetDefaultVisibility(EUIWidgetType Type);
	// 저장 값 + 자식 전체 복제 (실행 상태는 초기화)
	std::unique_ptr<FUIWidget> Clone() const;

	// 자식으로 붙인다 (Index < 0이면 끝). 반환: 붙인 위젯
	FUIWidget*                 AddChild(std::unique_ptr<FUIWidget> Child, int32 Index = -1);
	// 자식에서 떼어 소유권을 돌려준다 (자식이 아니면 nullptr)
	std::unique_ptr<FUIWidget> RemoveChild(const FUIWidget* Child);
	int32                      GetChildIndex(const FUIWidget* Child) const;

	// 이 종류가 가질 수 있는 자식 수 (패널은 제한 없음 = -1, 보더/버튼 = 1, 나머지 = 0)
	static int32 GetMaxChildren(EUIWidgetType Type);
	bool         CanAddChild() const;
	bool         IsAncestorOf(const FUIWidget* Other) const;

	// 깊이 우선 (자신 포함). 이름이 같으면 먼저 만난 것
	FUIWidget*       FindByName(std::string_view InName);
	const FUIWidget* FindByName(std::string_view InName) const;
	FUIWidget*       FindById(uint32 Id);
	void             ForEach(const std::function<void(FUIWidget&)>& Visitor);
	void             ForEach(const std::function<void(const FUIWidget&)>& Visitor) const;

	// 트리 전체에 1부터 번호를 매기고 Parent 포인터를 다시 맞춘다
	void AssignIds();
	// 자신과 조상이 모두 bEnabled인지
	bool IsEnabledInHierarchy() const;
};
