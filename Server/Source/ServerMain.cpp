#include "ServerApplication.h"

// 콘솔 서브시스템: 로그가 콘솔에 출력되고 Ctrl+C로 정상 종료한다
int main()
{
	FServerApplication App;
	return App.Run();
}
