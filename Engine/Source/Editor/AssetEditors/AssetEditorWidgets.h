#pragma once

#include <filesystem>
#include <string>
#include <vector>

// 에셋 편집 창 공용 UI 도우미
struct FAssetEditorWidgets
{
	// ContentDirectory 아래 이미지 파일 목록. 경로는 RelativeTo 기준 상대 경로('/' 구분), 이름순
	static std::vector<std::string> ScanImageFiles(const std::filesystem::path& ContentDirectory, const std::filesystem::path& RelativeTo);

	// 텍스처 경로 선택 콤보 ("(없음)" + 목록). 반환: 바뀌었으면 true
	static bool TextureCombo(const char* Id, std::string& InOutPath, const std::vector<std::string>& Files, const char* EmptyLabel);

	// 흐린 색으로 줄바꿈되는 안내 문구
	static void Hint(const char* Text);
};
