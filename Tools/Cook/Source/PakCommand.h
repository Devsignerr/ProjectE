#pragma once

#include <filesystem>
#include <vector>

// 패키징: Root 아래 Directories(Root 기준 상대 폴더)의 모든 파일을 pak 하나로 묶는다. 키는 Root 기준 (런타임 마운트 루트 = 패키지 루트).
// 성공 시 0 (프로세스 종료 코드)
int MakePak(const std::filesystem::path& PakFile, const std::filesystem::path& Root, const std::vector<std::filesystem::path>& Directories);
