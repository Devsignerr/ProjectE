#pragma once

#include "Renderer/GltfLoader.h"
#include "Renderer/Image.h"
#include "Renderer/TextureCompression.h"

#include <filesystem>

class FBinaryReader;
class FBinaryWriter;

// 쿠킹 에셋 캐시. 소스 에셋(glTF, PNG 등)을 엔진 바이너리로 변환해 두고, 다음 로드부터 파싱/디코딩을 건너뛴다.
//   위치: <프로젝트>/Cooked/<Content 기준 상대 경로><원본 확장자>.<쿠킹 확장자>
//         예) Content/DamagedHelmet.glb → Cooked/DamagedHelmet.glb.emodel
//             Content/UVChecker.png → Cooked/UVChecker.png.color.etex (텍스처는 용도별로 따로 쿠킹: 밉 + BC 압축)
//   유효성: 형식 버전 일치 + 쿠킹 파일이 소스보다 새로움 (소스가 없으면 쿠킹본을 그대로 신뢰 — 패키지용)
//   Content 밖의 파일은 캐시하지 않고 항상 원본을 읽는다.
struct FAssetCache
{
	static constexpr uint32 ModelMagic   = 0x4C444D45; // "EMDL"
	static constexpr uint32 TextureMagic = 0x32585445; // "ETX2"
	static constexpr uint32 ModelVersion = 12; // 12: LOD 형상 오차(오차 기반 LOD 선택), 11: 스킨 메시 LOD, 10: 임포트 MaxTriangles + 잎 메시 솎아내기 LOD, 9: 메시 UV 밀도(텍스처 밉 스트리밍), 8: 머티리얼 블렌드 모드/알파 컷오프/양면, 7: 메시 LOD(단순화 인덱스 + 화면 크기), 6: FBX + 임포트 설정(.eimport), 5: 이미지 BC 압축 + 밉, 4: 스킨/애니메이션, 3: 센티미터 단위(glTF ×100), 2: 정점 탄젠트 + PBR
	static constexpr uint32 TextureVersion = 1; // 1: 전체 밉 체인 + BC7/BC5/BC4 (용도별)
	static constexpr uint32 EnvironmentMagic   = 0x564E4545; // "EENV"
	static constexpr uint32 EnvironmentVersion = 1; // 1: 등장방형 RGBA16F, 폭 최대 2048 (상자 필터 축소)
	static constexpr const wchar_t* EnvironmentExtension = L".eenv";

	static constexpr const wchar_t* ModelExtension = L".emodel";
	// 텍스처 쿠킹 확장자 (용도별): .color.etex / .linear.etex / .normal.etex / .mask.etex
	static const wchar_t* GetTextureExtension(ETextureUsage Usage);

	enum class ESource : uint8
	{
		Cooked,    // 쿠킹본 사용
		Converted, // 원본을 읽어 변환 (+ 쿠킹본 기록)
		Failed,
	};

	// 로드: 쿠킹본이 유효하면 사용, 아니면 원본을 읽고 bWriteCooked면 쿠킹본 기록
	static ESource LoadModelAsset(const std::filesystem::path& SourcePath, FModelData& OutModel, bool bWriteCooked = true);
	static ESource LoadTextureAsset(const std::filesystem::path& SourcePath, ETextureUsage Usage, FCompressedTexture& OutTexture,
	                                bool bWriteCooked = true);

	// HDR 환경맵 (.hdr 등 → 등장방형 RGBA16F .eenv, 하늘/IBL 소스)
	static ESource LoadEnvironmentAsset(const std::filesystem::path& SourcePath, FEnvironmentImage& OutImage, bool bWriteCooked = true);

	// 쿠킹 도구용: 캐시 상태와 무관하게 원본에서 다시 변환해 기록
	static bool CookModelAsset(const std::filesystem::path& SourcePath);
	static bool CookTextureAsset(const std::filesystem::path& SourcePath, ETextureUsage Usage);
	static bool CookEnvironmentAsset(const std::filesystem::path& SourcePath);

	// 캐시 대상이 아니면 빈 경로
	static std::filesystem::path GetCookedPath(const std::filesystem::path& SourcePath, const wchar_t* CookedExtension);
	static std::filesystem::path GetCookedDirectory(); // 프로젝트 없으면 빈 경로

	// 모델 원본 읽기: 확장자별 로더(.fbx → FFbxLoader, 그 밖 glTF). LoadModelSource는 임포트 설정·추가 애니메이션까지 적용
	static bool LoadModelFile(const std::filesystem::path& SourcePath, FModelData& OutModel);
	static bool LoadModelSource(const std::filesystem::path& SourcePath, FModelData& OutModel);
	// 쿠킹에 쓴 임포트 설정 기록(<쿠킹 파일>.import)이 지금 설정과 같은지 / 기록 남기기
	static bool IsCookedWithCurrentImportSettings(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath);
	static void WriteImportSettingsRecord(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath);
	// 쿠킹본이 임포트 설정 파일/추가 애니메이션 파일보다 새로운지 (없는 파일은 무시)
	static bool IsCookedNewerThanImportInputs(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath);

	// 쿠킹본이 소스 기준으로 최신인지 (파일 시각만 비교, 형식 버전은 로드 시 검사)
	static bool IsCookedUpToDate(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath);

	// 모델 이미지를 머티리얼 용도(색상 > 노멀 > 선형 > 마스크 우선)로 밉 + BC 압축하고 원본 픽셀은 비운다
	static void CompressModelImages(FModelData& Model);
	// 메시마다 UV 밀도(TextureStreamingMath::ComputeUvDensity) 계산 — 쿠킹 전에
	static void ComputeModelUvDensities(FModelData& Model);

	// ---- 직렬화 (순수 함수, 테스트용 공개)
	// OutImagePayloadOffsets: 이미지마다 텍스처 본문이 시작하는 버퍼 위치 (밉 스트리밍 — 저장한 파일의 같은 위치)
	static void WriteModel(FBinaryWriter& Writer, const FModelData& Model, std::vector<uint64>* OutImagePayloadOffsets = nullptr);
	static bool ReadModel(FBinaryReader& Reader, FModelData& OutModel);
	static void WriteTexture(FBinaryWriter& Writer, const FCompressedTexture& Texture);
	static bool ReadTexture(FBinaryReader& Reader, FCompressedTexture& OutTexture);
	// .etex 파일 안 텍스처 본문 시작 위치 (머리 Magic + Version 다음)
	static constexpr uint64 TexturePayloadOffset = 8;
	static void WriteEnvironment(FBinaryWriter& Writer, const FEnvironmentImage& Image);
	static bool ReadEnvironment(FBinaryReader& Reader, FEnvironmentImage& OutImage);
};
