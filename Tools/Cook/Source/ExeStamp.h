#pragma once

#include <filesystem>

// 패키징: 복사한 런타임 exe에 현재 프로젝트(.eproject)의 아이콘·버전 정보를 써 넣는다. 성공 시 0 (프로세스 종료 코드)
int StampExecutable(const std::filesystem::path& ExecutablePath);
