#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>

// 콘텐츠 브라우저 "만들기" 항목용 2D 에셋 파일 생성 (Phase 56-5b). 모두 겹치지 않는 이름(FAssetFileOps::MakeUniquePath)으로
// FSprite2DLibrary::Save*를 거쳐 쓴다. 에셋 안 텍스처 경로 = 이미지 파일 이름(같은 폴더 — 이 파일 폴더 기준 규약).
namespace Sprite2DAssetCreation
{
	bool IsImageFile(const std::filesystem::path& Path);

	// 이미지 옆 <이름>.esprite: 텍스처 크기를 채우고 전체 이미지 슬라이스 하나. bGridDialog면 처음 열 때 격자 대화 (FSpriteAtlasEditor::RequestGridDialog)
	std::filesystem::path CreateSpriteAtlas(const std::filesystem::path& Image, bool bGridDialog, std::string* OutError = nullptr);
	// 이미지 옆 <이름>.etileset (타일 크기 지정)
	std::filesystem::path CreateTileset(const std::filesystem::path& Image, int32 TileWidth, int32 TileHeight, std::string* OutError = nullptr);
	// 빈 플립북 파일 (Path에 그대로 — 콘텐츠 브라우저 CreateAsset이 이름을 정한다)
	bool SaveDefaultFlipbook(const std::filesystem::path& Path);

	// 콘텐츠 브라우저 항목 문맥 메뉴: 이미지면 "스프라이트 아틀라스 만들기"/"타일셋 만들기" 항목을 그린다 (ImGui 메뉴 안에서).
	// 반환 = 만든 파일 경로 (없으면 빈 경로). OutError = 실패 문구
	std::filesystem::path DrawImageContextMenu(const std::filesystem::path& Image, std::string& OutError);
} // namespace Sprite2DAssetCreation
