#include "Core/StringConv.h"

#include "Core/Platform/WindowsHeaders.h"

std::wstring FStringConv::ToWide(std::string_view Utf8)
{
	if (Utf8.empty())
	{
		return {};
	}

	const int SourceLength = static_cast<int>(Utf8.size());
	const int Length = MultiByteToWideChar(CP_UTF8, 0, Utf8.data(), SourceLength, nullptr, 0);
	if (Length <= 0)
	{
		return {};
	}

	std::wstring Result(static_cast<size_t>(Length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Utf8.data(), SourceLength, Result.data(), Length);
	return Result;
}

std::string FStringConv::ToUtf8(std::wstring_view Wide)
{
	if (Wide.empty())
	{
		return {};
	}

	const int SourceLength = static_cast<int>(Wide.size());
	const int Length = WideCharToMultiByte(CP_UTF8, 0, Wide.data(), SourceLength, nullptr, 0, nullptr, nullptr);
	if (Length <= 0)
	{
		return {};
	}

	std::string Result(static_cast<size_t>(Length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, Wide.data(), SourceLength, Result.data(), Length, nullptr, nullptr);
	return Result;
}
