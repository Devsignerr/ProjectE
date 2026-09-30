#pragma once

#include <filesystem>
#include <vector>

// 콘텐츠 브라우저에서 끄는 에셋 목록. ImGui 페이로드에는 형식만 싣고 경로는 여기에 둔다 (여러 파일, 가변 길이).
// 받는 쪽: 뷰포트(배치/머티리얼 지정), 계층(추가), 인스펙터 에셋 칸, 콘텐츠 폴더(이동)
struct FContentDragDrop
{
	static constexpr const char* PayloadType = "PE_CONTENT_ASSETS";

	static std::vector<std::filesystem::path>& GetPaths()
	{
		static std::vector<std::filesystem::path> Paths;
		return Paths;
	}

	// 지금 끌고 있는(또는 방금 놓은) 콘텐츠 페이로드를 받는다. 마우스를 놓은 프레임에만 경로를 돌려준다
	static const std::vector<std::filesystem::path>* AcceptPayload();
};
