#pragma once

#include <filesystem>
#include <string_view>

// 처리되지 않은 SEH 예외(액세스 위반 등)를 잡아 예외 코드와 심볼화된 콜스택을 stderr와 로그 파일에 남긴다.
// 덤프 폴더가 지정되면 <폴더>/<시각>/에 미니덤프(Minidump.dmp) + 보고서(CrashReport.txt) + 로그 사본을 쓴다.
// Fatal 로그(E_CHECK 실패 등)도 같은 경로로 덤프를 남긴다.
// 디버거가 붙어 있으면 아무것도 하지 않는다 (디버거가 먼저 처리).
class FCrashHandler
{
public:
	// 여러 번 호출해도 한 번만 설치된다
	static void Install();

	// 크래시 덤프를 쓸 폴더 (FPaths 초기화 후 FApplication이 <Saved>/Crashes로 지정). 비어 있으면 덤프를 쓰지 않는다
	static void SetDumpDirectory(const std::filesystem::path& Directory);
	// 크래시 후 알림 대화 상자 (패키지 창 앱만 — 자동 검증/서버에서는 끈다)
	static void SetShowDialog(bool bShow);

	// Fatal 로그 경로: 현재 스레드 상태로 덤프를 남긴다 (FLog가 중단 직전에 부른다)
	static void ReportFatal(std::string_view Message);
};
