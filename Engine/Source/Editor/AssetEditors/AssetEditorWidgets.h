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

	// 직전 위젯을 콘텐츠 브라우저 이미지 드롭 대상으로 만든다 (놓으면 RelativeTo 기준 상대 경로). 반환: 바뀌었으면 true
	static bool AcceptTextureDrop(std::string& InOutPath, const std::filesystem::path& RelativeTo);

	// 흐린 색으로 줄바꿈되는 안내 문구
	static void Hint(const char* Text);
};
