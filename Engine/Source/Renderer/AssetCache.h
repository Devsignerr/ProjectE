#pragma once

#include "Renderer/GltfLoader.h"
#include "Renderer/Image.h"

#include <filesystem>

class FBinaryReader;
class FBinaryWriter;

// 쿠킹 에셋 캐시. 소스 에셋(glTF, PNG 등)을 엔진 바이너리로 변환해 두고, 다음 로드부터 파싱/디코딩을 건너뛴다.
//   위치: <프로젝트>/Cooked/<Content 기준 상대 경로><원본 확장자>.<쿠킹 확장자>
//         예) Content/DamagedHelmet.glb → Cooked/DamagedHelmet.glb.emodel
//   유효성: 형식 버전 일치 + 쿠킹 파일이 소스보다 새로움 (소스가 없으면 쿠킹본을 그대로 신뢰 — 패키지용)
//   Content 밖의 파일은 캐시하지 않고 항상 원본을 읽는다.
struct FAssetCache
{
	static constexpr uint32 ModelMagic   = 0x4C444D45; // "EMDL"
	static constexpr uint32 ImageMagic   = 0x58455445; // "ETEX"
	static constexpr uint32 ModelVersion = 2; // 2: 정점 탄젠트 + PBR 머티리얼 텍스처/팩터
	static constexpr uint32 ImageVersion = 1;

	static constexpr const wchar_t* ModelExtension = L".emodel";
	static constexpr const wchar_t* ImageExtension = L".etex";

	enum class ESource : uint8
	{
		Cooked,    // 쿠킹본 사용
		Converted, // 원본을 읽어 변환 (+ 쿠킹본 기록)
		Failed,
	};

	// 로드: 쿠킹본이 유효하면 사용, 아니면 원본을 읽고 bWriteCooked면 쿠킹본 기록
	static ESource LoadModelAsset(const std::filesystem::path& SourcePath, FModelData& OutModel, bool bWriteCooked = true);
	static ESource LoadImageAsset(const std::filesystem::path& SourcePath, FImage& OutImage, bool bWriteCooked = true);

	// 쿠킹 도구용: 캐시 상태와 무관하게 원본에서 다시 변환해 기록
	static bool CookModelAsset(const std::filesystem::path& SourcePath);
	static bool CookImageAsset(const std::filesystem::path& SourcePath);

	// 캐시 대상이 아니면 빈 경로
	static std::filesystem::path GetCookedPath(const std::filesystem::path& SourcePath, const wchar_t* CookedExtension);
	static std::filesystem::path GetCookedDirectory(); // 프로젝트 없으면 빈 경로

	// 쿠킹본이 소스 기준으로 최신인지 (파일 시각만 비교, 형식 버전은 로드 시 검사)
	static bool IsCookedUpToDate(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath);

	// ---- 직렬화 (순수 함수, 테스트용 공개)
	static void WriteModel(FBinaryWriter& Writer, const FModelData& Model);
	static bool ReadModel(FBinaryReader& Reader, FModelData& OutModel);
	static void WriteImage(FBinaryWriter& Writer, const FImage& Image);
	static bool ReadImage(FBinaryReader& Reader, FImage& OutImage);
};
