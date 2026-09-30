#pragma once

#include "Core/CoreTypes.h"

#include <imgui.h>

#include <IconsFontAwesome6.h>

#include <string>
#include <string_view>

// 에디터 UI 테마 (언리얼 5 풍 어두운 회색 + 파란 강조색 하나) + 아이콘 글꼴(Font Awesome 6 Solid, ICON_FA_*) 도우미.
// 패널 제목은 PanelTitle로 만들어 아이콘이 붙어도 창 ID(도킹 위치)가 영문 ID로 고정되게 한다.
struct FEditorTheme
{
	// 강조색 (선택/활성/체크)
	static constexpr ImVec4 Accent      = ImVec4(0.00f, 0.44f, 0.88f, 1.00f);
	static constexpr ImVec4 AccentHover = ImVec4(0.10f, 0.52f, 0.95f, 1.00f);
	static constexpr ImVec4 TextDim     = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
	static constexpr ImVec4 Success     = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
	static constexpr ImVec4 Warning     = ImVec4(0.95f, 0.72f, 0.25f, 1.00f);
	static constexpr ImVec4 Danger      = ImVec4(0.92f, 0.33f, 0.30f, 1.00f);

	// 색/크기 (Scale = DPI 배율)
	static void Apply(float Scale);

	// 아이콘 글꼴을 합친 기본/굵은 글꼴 로드 (FImGuiLayer::Init). 굵은 글꼴이 없으면 기본 글꼴
	static void LoadFonts(float Scale);
	static ImFont* GetBoldFont();

	// "아이콘  이름###Id" (보이는 제목과 무관하게 ID 고정)
	static std::string PanelTitle(const char* Icon, std::string_view Label, std::string_view Id);

	// 에셋 종류별 아이콘/색 (콘텐츠 브라우저, 편집 창). Extension은 소문자 (".glb")
	struct FAssetStyle
	{
		const char* Icon  = ICON_FA_FILE;
		ImU32       Color = IM_COL32(150, 150, 150, 255);
		const char* Label = "파일";
	};
	static FAssetStyle GetAssetStyle(std::string_view Extension, bool bDirectory);

	// 리플렉션 컴포넌트 이름("StaticMeshComponent") → 아이콘 (인스펙터 헤더)
	static const char* GetComponentIcon(std::string_view TypeName);

	// 아이콘 + 글자 버튼. bActive면 강조색 배경 (도구 막대 토글)
	static bool ToolButton(const char* Icon, const char* Tooltip, bool bActive);
};
