#pragma once

// 처리되지 않은 SEH 예외(액세스 위반 등)를 잡아 예외 코드와 심볼화된 콜스택을 stderr와 로그 파일에 남긴다.
// 디버거가 붙어 있으면 아무것도 하지 않는다 (디버거가 먼저 처리).
class FCrashHandler
{
public:
	// 여러 번 호출해도 한 번만 설치된다
	static void Install();
};
