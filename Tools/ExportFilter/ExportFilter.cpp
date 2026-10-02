// 엔진 DLL 자동 내보내기 목록(.def) 거르기 (Engine/CMakeLists.txt가 빌드 중에 실행하는 호스트 도구)
// 사용: EExportFilter <입력 .def> <출력 .def>
//
// 거르는 심볼 (DLL 내보내기 한도 65535):
//  - sol2 내부 ("@sol@@" = 최상위 네임스페이스 sol) — DLL 밖은 sol 심볼을 쓰지 않는다
//  - 이름이 std:: 범위인 함수 (std 템플릿 인스턴스: vector<FFoo>::push_back 등) — 헤더 템플릿이라 쓰는 쪽이 각자 인스턴스화한다.
//    엔진 함수가 std 타입을 인자로 받는 경우와 구분하려고 맹글링 문자열이 아니라 UnDecorateSymbolName(이름만)의 앞부분을 본다.
//    데이터 심볼(DATA — 함수 지역 static 가드/정적 멤버)은 바이너리마다 따로 생기면 의미가 달라지므로 남긴다.

#include <Windows.h>
#include <DbgHelp.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	bool IsStdScopedFunction(const std::string& Symbol)
	{
		char Buffer[8192];
		const DWORD Length = UnDecorateSymbolName(Symbol.c_str(), Buffer, sizeof(Buffer), UNDNAME_NAME_ONLY);
		return Length >= 5 && std::string(Buffer, 5) == "std::";
	}
}

int main(int ArgCount, char** Args)
{
	if (ArgCount != 3)
	{
		std::cerr << "사용: EExportFilter <입력 .def> <출력 .def>\n";
		return 1;
	}

	std::ifstream Input(Args[1]);
	if (!Input)
	{
		std::cerr << "입력 .def를 열 수 없음: " << Args[1] << "\n";
		return 1;
	}

	std::vector<std::string> Kept;
	std::string Line;
	size_t SolCount = 0;
	size_t StdCount = 0;
	while (std::getline(Input, Line))
	{
		const size_t Begin = Line.find_first_not_of(" \t");
		if (Begin == std::string::npos || Line.compare(Begin, 7, "EXPORTS") == 0)
		{
			continue;
		}
		const size_t End = Line.find_first_of(" \t", Begin);
		const std::string Symbol = Line.substr(Begin, End == std::string::npos ? std::string::npos : End - Begin);
		const bool bData = Line.find("DATA", Begin + Symbol.size()) != std::string::npos;

		if (Symbol.find("@sol@@") != std::string::npos)
		{
			++SolCount;
			continue;
		}
		if (!bData && IsStdScopedFunction(Symbol))
		{
			++StdCount;
			continue;
		}
		Kept.push_back(Line);
	}

	if (Kept.size() > 65535)
	{
		std::cerr << "엔진 DLL 내보내기 " << Kept.size() << "개 > 65535 (Tools/ExportFilter에서 더 거를 것)\n";
		return 1;
	}

	std::ofstream Output(Args[2], std::ios::trunc);
	Output << "EXPORTS\n";
	for (const std::string& Entry : Kept)
	{
		Output << Entry << "\n";
	}
	std::cout << "엔진 DLL 내보내기 " << Kept.size() << "개 (sol " << SolCount << "개, std 함수 " << StdCount << "개 제외)\n";
	return Output ? 0 : 1;
}
