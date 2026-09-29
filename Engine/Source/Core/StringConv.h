#pragma once

#include <string>
#include <string_view>

// UTF-8 <-> UTF-16 변환 (Win32 API 경계용)
struct FStringConv
{
	static std::wstring ToWide(std::string_view Utf8);
	static std::string  ToUtf8(std::wstring_view Wide);
};
