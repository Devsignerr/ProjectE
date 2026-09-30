#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>
#include <vector>

struct FModelData;

// 모델(.glb/.gltf/.fbx) 임포트 설정 = 원본 옆 사이드카 "<원본 파일 이름>.eimport" (JSON). 없으면 기본값.
// 쿠킹 단계(FAssetCache)에서 원본을 읽은 직후 적용되고, 설정 파일이 바뀌면 쿠킹본을 다시 만든다.
struct FModelImportSettings
{
	static constexpr const wchar_t* Extension = L".eimport";

	float Scale      = 1.0f; // 원본 단위 변환(glTF/FBX → cm) 뒤에 곱하는 배율
	float YawDegrees = 0.0f; // 위(+Z)축 기준 회전 (앞 방향 맞추기)

	bool bImportMaterials  = true;  // false면 머티리얼/텍스처 없이 기본 머티리얼
	bool bImportSkin       = true;  // false면 스킨(뼈대 변형) 없이 정적 메시로
	bool bImportAnimations = true;
	bool bRecomputeNormals  = false; // 부드러운 법선을 새로 계산 (원본 법선 무시)
	bool bRecomputeTangents = false; // UV로 탄젠트 새로 계산

	// 이 모델에 덧붙일 애니메이션 파일들 (원본 폴더 기준 상대 경로). 채널은 노드 이름으로 맞춘다
	std::vector<std::string> AnimationSources;

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json);

	static std::filesystem::path GetSidecarPath(const std::filesystem::path& SourcePath);
	// 사이드카가 없거나 읽지 못하면 기본값
	static FModelImportSettings LoadForSource(const std::filesystem::path& SourcePath);
	bool                        SaveForSource(const std::filesystem::path& SourcePath) const;

	bool IsDefault() const;

	// 모델 데이터에 설정 적용 (크기/방향은 새 최상위 노드로 — 정점·역바인드·애니메이션 키는 그대로)
	void Apply(FModelData& Model) const;
	// 다른 모델의 애니메이션을 노드 이름으로 맞춰 붙인다. 반환: 붙인 클립 수
	static uint32 MergeAnimations(FModelData& Target, const FModelData& Source, const std::string& ClipPrefix);
};
