#pragma once

#include "Core/CoreTypes.h"

#include <string>
#include <string_view>
#include <vector>

// 프로세스 명령줄 파싱. 지원 형태: "--flag", "--key value", "--key=value"
class FCommandLine
{
public:
	// 프로세스의 실제 명령줄(GetCommandLineW)에서 생성. 첫 인자(실행 파일 경로)는 제외된다.
	static FCommandLine FromProcess();

	// 인자 문자열만 파싱 (실행 파일 이름 없음). Windows 인용 규칙(CommandLineToArgvW)을 따른다. 테스트/도구용.
	static FCommandLine Parse(std::wstring_view Arguments);

	bool HasFlag(std::wstring_view Flag) const;

	// "--key value" 또는 "--key=value"의 value. 없으면 빈 문자열
	std::wstring GetValue(std::wstring_view Key) const;

	const std::vector<std::wstring>& GetArguments() const { return Arguments; }

private:
	std::vector<std::wstring> Arguments;
};
