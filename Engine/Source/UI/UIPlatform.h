#pragma once

#include <string>

// UI가 쓰는 운영체제 기능 (구현은 UIPlatformWindows.cpp — 공개 헤더에 Windows.h를 넣지 않는다)
struct FUIPlatform
{
	// Windows 표시 언어 ("ko-KR"). 실패하면 빈 문자열
	static std::string GetSystemLanguage();
	// 클립보드 (UTF-8). 실패하면 false / 빈 문자열
	static bool        SetClipboardText(const std::string& Utf8);
	static std::string GetClipboardText();
};
