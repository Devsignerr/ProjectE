#pragma once

#include "Core/CoreTypes.h"
#include "Renderer/TextureCompression.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 패키지 의존성 매니페스트 (ProjectECook --package-manifest). 패키지에 넣을 Content 파일과 쿠킹할 에셋을 루트에서 참조를 따라가 정한다.
//
// 루트 (런타임 기준 — 에디터 시작 맵은 넣지 않는다):
//   프로젝트 설정 Maps.GameDefaultMap, 서버 기본 맵(GetServerDefaultMap), Maps.PlayerPrefab,
//   Localization.StringTables(";" 구분, 비면 Content/Localization/*.estrings),
//   Packaging.AdditionalAssets(";" 구분 Content 기준 파일/폴더 — 스캐너가 못 보는 동적 경로용, 폴더는 하위 전체),
//   Config/*.json(위 섹션 파일 Maps/Packaging/Localization 제외)의 모든 문자열.
//   명시 루트(맵·프리팹·문자열 표·AdditionalAssets)가 없으면 오류(W 줄 + 실패).
//
// 참조 판정 (FPackageDependencyScanner):
//   텍스트 에셋(아래 TextExtensions)의 모든 문자열 리터럴을 본다 — JSON은 "..."(이스케이프 해제), Lua는 "..."/'...'/[[...]]
//   (주석 제외). 해제한 문자열 안에 다시 따옴표가 있으면(씬 스크립트 PropertyOverrides 같은 이스케이프된 JSON) 재귀로 본다.
//   문자열을 ";"로 나눈 조각마다, 그리고 "<경로>:<이름>"(리타기팅 클립 등)은 ":" 앞부분도 후보로 삼는다.
//   후보가 (대소문자 무시, "/"·"\" 모두) ① Content 기준 ② 참조한 파일의 폴더 기준으로 실제 파일이면 참조다(".." 허용, Content 밖은 무시).
//   Content 기준으로 실제 폴더이고 후보에 경로 구분자가 있으면(예 Lua "Audio/RPG/" .. 이름) 폴더 하위 전체를 넣는다(동적 경로 안전장치).
//   "primitive:cube"/"foliage:..." 같은 내장 이름, URL("://"), 절대 경로, 1024자 넘는 문자열(base64 등)은 자연히/명시적으로 무시된다.
//   추가 규칙: 모델(.gltf/.glb/.fbx)을 참조하면 옆의 <모델>.emeta/<모델>.eimport, 씬(.escene)은 옆의 <씬 이름>.enav도 넣는다.
//
// 모델 원본 문맥: .gltf(uri — 버퍼/이미지, 퍼센트 인코딩 해제)와 .eimport(추가 애니메이션 등)가 참조한 파일은 "내부(I)"다 —
//   쿠킹 입력일 뿐 .emodel에 포함되므로 패키지에 넣지 않고 따로 쿠킹하지도 않는다. 다른 문맥에서도 참조되면 일반 파일이 된다.
//
// 이미지 용도: .emat(부모 체인 포함, 각 .emat을 FMaterialAsset으로 읽어 슬롯/텍스처 파라미터 용도)는 그 용도,
//   그 밖의 참조(UI/파티클/쿠키/데이터 표 아이콘 등 — 엔진의 공개 LoadTexture는 모두 색상)는 Color. 용도가 하나도 없으면 Color.
//
// 매니페스트 파일 (UTF-8, 탭 구분, '#'은 주석, 경로는 Content 기준 "/" 구분·정렬):
//   F <경로>                  패키지에 넣는 파일 (원본 모델/이미지는 쿠킹본이 있으면 Package.ps1이 원본을 뺀다)
//   M <경로>                  쿠킹할 모델 (.emodel)
//   T <경로> <용도,...>        따로 쿠킹할 이미지와 용도 (color/linear/normal/mask → .<용도>.etex)
//   I <경로>                  모델 원본만 참조하는 파일 (glTF 버퍼/이미지 — 패키지·단독 쿠킹 제외, -IncludeSources면 넣음)
//   D <폴더>                  폴더 참조로 통째로 넣은 폴더 (확인용)
//   W <메시지>                경고/오류
struct FPackageManifest
{
	std::set<std::string>                          Files;       // F
	std::set<std::string>                          Models;      // M
	std::map<std::string, std::set<ETextureUsage>> Images;      // T
	std::set<std::string>                          Internal;    // I
	std::set<std::string>                          Directories; // D
	std::vector<std::string>                       Warnings;    // W
	bool                                           bHasErrors = false; // 명시 루트가 없음

	std::string ToText() const;
	bool        WriteToFile(const std::filesystem::path& Path) const;
	static bool ReadFromFile(const std::filesystem::path& Path, FPackageManifest& OutManifest);
	static bool ParseText(std::string_view Text, FPackageManifest& OutManifest);
};

namespace PackageManifest
{
	// 텍스트 에셋에서 문자열 리터럴(해제한 값)을 모두 꺼낸다. bLua면 Lua 문법(주석 건너뛰기, '...'·[[...]]), 아니면 JSON "..."만.
	// 해제한 값에 따옴표가 있으면 그 안의 문자열도 JSON으로 재귀해 덧붙인다 (이스케이프된 JSON — 씬 PropertyOverrides)
	std::vector<std::string> ExtractStrings(std::string_view Text, bool bLua);

	// 참조 문자열 → 경로 후보 (";" 분리, 앞뒤 공백 제거, "\"→"/", "<경로>:<이름>"의 앞부분 추가, URL/절대 경로/빈 값/너무 긴 값 제외)
	std::vector<std::string> SplitReferenceCandidates(std::string_view Value);

	// "a/b/../c" → "a/c" (Content 기준 상대 경로 정규화). 루트 밖으로 나가거나 비면 빈 문자열
	std::string NormalizeRelativePath(std::string_view Path);

	// 용도 이름 (매니페스트 표기): color/linear/normal/mask
	const char* GetUsageName(ETextureUsage Usage);
	bool        ParseUsageName(std::string_view Name, ETextureUsage& OutUsage);
} // namespace PackageManifest

// Content 폴더 하나에 대한 의존성 스캐너 (디스크 직접 — 쿠킹 도구 전용)
class FPackageDependencyScanner
{
public:
	explicit FPackageDependencyScanner(const std::filesystem::path& ContentDirectory);

	// Content 기준 파일 또는 폴더(폴더는 하위 전체). 없으면 경고 + bRequired면 오류
	bool AddRoot(std::string_view ContentRelative, bool bRequired, std::string_view Label);
	// JSON 파일의 모든 문자열을 Content 기준 참조로 본다 (Config/*.json)
	void AddRootsFromJsonFile(const std::filesystem::path& JsonFile);

	FPackageManifest Run();

private:
	enum class EContext : uint8
	{
		Package,     // 패키지에 넣는 파일이 참조
		ModelSource, // 모델 원본(glTF/.eimport)이 참조 — 쿠킹 입력
	};

	struct FPending
	{
		std::string Path;
		EContext    Context;
	};

	const std::string* FindFile(const std::string& NormalizedLower) const;
	const std::string* FindDirectory(const std::string& NormalizedLower) const;
	void               AddDirectory(const std::string& Directory);
	// 파일 하나를 문맥에 넣는다. bColorUse = 머티리얼 밖 참조(이미지면 Color 용도)
	void AddFile(const std::string& Path, EContext Context, bool bColorUse);
	void ResolveValue(std::string_view Value, const std::string& ReferrerDirectory, EContext Context, bool bFromMaterial);
	void ScanFile(const std::string& Path, EContext Context);
	void ScanMaterialUsages(const std::string& Path);

	std::filesystem::path              Content;
	std::map<std::string, std::string> FileIndex;      // 소문자 → 실제 경로
	std::map<std::string, std::string> DirectoryIndex; // 소문자 → 실제 경로
	std::set<std::string>              Shipped;
	std::set<std::string>              InternalFiles;
	std::set<std::string>              ScannedInternal;
	std::vector<FPending>              Queue;
	FPackageManifest                   Result;
};

// 현재 프로젝트(FPaths + FProjectSettings)의 매니페스트
FPackageManifest BuildProjectPackageManifest();
