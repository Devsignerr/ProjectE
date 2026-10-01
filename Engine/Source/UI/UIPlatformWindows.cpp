#include "UI/UIPlatform.h"

#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <cstring>

std::string FUIPlatform::GetSystemLanguage()
{
	wchar_t Name[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(Name, LOCALE_NAME_MAX_LENGTH) <= 0)
	{
		return {};
	}
	return FStringConv::ToUtf8(Name);
}

bool FUIPlatform::SetClipboardText(const std::string& Utf8)
{
	const std::wstring Wide = FStringConv::ToWide(Utf8);
	if (!OpenClipboard(nullptr))
	{
		return false;
	}
	EmptyClipboard();
	bool          bOk    = false;
	const size_t  Bytes  = (Wide.size() + 1) * sizeof(wchar_t);
	const HGLOBAL Memory = GlobalAlloc(GMEM_MOVEABLE, Bytes);
	if (Memory != nullptr)
	{
		if (void* Locked = GlobalLock(Memory))
		{
			std::memcpy(Locked, Wide.c_str(), Bytes);
			GlobalUnlock(Memory);
			bOk = SetClipboardData(CF_UNICODETEXT, Memory) != nullptr;
		}
		if (!bOk)
		{
			GlobalFree(Memory); // 성공하면 클립보드가 소유한다
		}
	}
	CloseClipboard();
	return bOk;
}

std::string FUIPlatform::GetClipboardText()
{
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr))
	{
		return {};
	}
	std::string Result;
	if (const HANDLE Data = GetClipboardData(CF_UNICODETEXT))
	{
		if (const auto* Locked = static_cast<const wchar_t*>(GlobalLock(Data)))
		{
			Result = FStringConv::ToUtf8(Locked);
			GlobalUnlock(Data);
		}
	}
	CloseClipboard();
	return Result;
}
