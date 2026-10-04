#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"
#include "Scene/Sprite/FlipbookAsset.h"
#include "Scene/Sprite/SpriteAsset.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

E_DECLARE_ENGINE_LOG_CATEGORY(LogSprite2D)

// .esprite / .eflipbook / .etileset 공유 캐시 (엔진 DLL 전역 하나, 메인 스레드 전용 — FDataLibrary와 같은 관례).
//   - 경로는 Content 기준(FPrefabLibrary의 Content 폴더) 또는 절대. 키 = 절대 경로 소문자
//   - 읽기는 FFileSystem(pak → 디스크). 경로마다 한 번 읽고 실패(없음/형식 오류)도 nullptr로 캐시해 경고를 한 번만 낸다
//   - 형식 경고는 "[2D] <경로>: ..." 로그로 한 번. 받은 객체는 불변(shared_ptr<const>) — 고치려면 사본을 Save*
//   - Invalidate(경로): 그 항목만 지우고 세대를 올린다. 세대(GetGeneration)가 바뀌면 들고 있는 쪽(컴포넌트 런타임)이 다시 Load
//   - 핫 리로드: 에디터 파일 감시가 바뀐 파일을 Invalidate, 콘텐츠 브라우저 이동은 전체 Invalidate
class FSprite2DLibrary
{
public:
	static FSprite2DLibrary& Get();

	std::shared_ptr<const FSpriteAsset>   LoadSprite(const std::string& AssetPath);
	std::shared_ptr<const FFlipbookAsset> LoadFlipbook(const std::string& AssetPath);
	std::shared_ptr<const FTilesetAsset>  LoadTileset(const std::string& AssetPath);

	void   Invalidate();
	void   Invalidate(const std::string& AssetPath);
	uint32 GetGeneration() const { return Generation; }

	// 저장 (편집기/도구): 파일 쓰기(디스크) + 그 경로 Invalidate
	bool SaveSprite(const std::string& AssetPath, const FSpriteAsset& Asset, std::string* OutError = nullptr);
	bool SaveFlipbook(const std::string& AssetPath, const FFlipbookAsset& Asset, std::string* OutError = nullptr);
	bool SaveTileset(const std::string& AssetPath, const FTilesetAsset& Asset, std::string* OutError = nullptr);

	// 편집기 실시간 미리보기: 그 경로의 캐시 항목을 저장 안 한 사본으로 바꾸고 세대를 올린다 (파일은 그대로 — Invalidate(경로)하면 파일 상태로 돌아감).
	// 2D 에셋 편집기의 "씬에 실시간 반영"(기본 끔)만 쓴다 — 닫을 때·끌 때 Invalidate
	void SetSpritePreview(const std::string& AssetPath, std::shared_ptr<const FSpriteAsset> Asset);
	void SetFlipbookPreview(const std::string& AssetPath, std::shared_ptr<const FFlipbookAsset> Asset);

	// 확장자(소문자, "." 포함)가 이 라이브러리 형식인가
	static bool IsSprite2DExtension(const std::wstring& LowerExtension);

	// 에셋 안 상대 참조(Texture, Sprite) → Content 기준 경로 ("/" 구분. Content 밖이면 절대 경로). 참조가 비면 빈 문자열.
	// OwnerAssetPath = 참조를 가진 에셋의 경로(Content 기준 또는 절대). 참조가 절대 경로면 그대로
	static std::string ResolveReference(const std::string& OwnerAssetPath, const std::string& Reference);

	std::filesystem::path ResolvePath(const std::string& AssetPath) const; // Content 기준 → 절대

private:
	static std::wstring MakeKey(const std::string& AssetPath);
	template <typename TAsset>
	std::shared_ptr<const TAsset> LoadCached(std::unordered_map<std::wstring, std::shared_ptr<const TAsset>>& Cache, const std::string& AssetPath, const char* Kind);
	bool SaveText(const std::string& AssetPath, const std::string& Text, std::string* OutError);

	std::unordered_map<std::wstring, std::shared_ptr<const FSpriteAsset>>   Sprites;
	std::unordered_map<std::wstring, std::shared_ptr<const FFlipbookAsset>> Flipbooks;
	std::unordered_map<std::wstring, std::shared_ptr<const FTilesetAsset>>  Tilesets;
	uint32                                                                  Generation = 1;
};
