#include "RuntimeApplication.h"

// 창 서브시스템(/ENTRY:mainCRTStartup)으로 링크되므로 콘솔 없이 실행된다. 로그는 디버거 출력 창으로 간다.
int main()
{
	FRuntimeApplication App;
	return App.Run();
}
