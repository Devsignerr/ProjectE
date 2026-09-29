#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct FEditorContext;

// 콘텐츠 디렉터리 탐색 + 모델을 씬에 추가
class FContentBrowserPanel
{
public:
	void Draw(FEditorContext& Context);

	bool bOpen = true;

private:
	struct FEntry
	{
		std::filesystem::path Path;
		std::string           DisplayName;
		std::string           Extension; // 소문자
		bool                  bDirectory = false;
		uint64_t              SizeInBytes = 0;
	};

	void Refresh(const std::filesystem::path& Root);

	std::filesystem::path CurrentDirectory;
	std::vector<FEntry>   Entries;
	bool                  bNeedsRefresh = true;
};
