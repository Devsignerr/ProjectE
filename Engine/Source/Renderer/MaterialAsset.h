#pragma once

#include "Renderer/Material.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/TextureCompression.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// .emat 파일 내용 (JSON, PBR 금속/거칠기). 텍스처 경로는 .emat 파일이 있는 디렉터리 기준 상대 경로, 비어 있으면 기본 텍스처.
//   { "Name": "Checker", "BaseColorFactor": [1,1,1,1], "Metallic": 0, "Roughness": 0.8,
//     "EmissiveFactor": [0,0,0], "NormalScale": 1, "OcclusionStrength": 1,
//     "BaseColorTexture": "../UVChecker.png", "MetallicRoughnessTexture": "", "NormalTexture": "",
//     "OcclusionTexture": "", "EmissiveTexture": "" }
//   + "BlendMode": "Opaque"|"Masked"|"Translucent"|"Additive", "AlphaCutoff": 0.5, "TwoSided": false (없으면 기본값)
// 구 형식(Blinn-Phong)의 BaseColorTint는 BaseColorFactor로 읽고, SpecularColor/Shininess/SpecularStrength는 무시한다.
//
// 머티리얼 인스턴스: "Parent"(부모 .emat, 이 파일 폴더 기준 상대 경로)가 있으면 파일에 적힌 키만 부모 값을 덮어쓴다
//   (OverrideMask). 부모도 인스턴스일 수 있다(체인). 해석은 Resolve — 순환/깊이 초과/부모 없음은 오류 로그 후 그때까지의 값으로.
//   이름은 상속하지 않는다. 부모의 텍스처 경로는 해석 결과에서 이 파일 폴더 기준으로 다시 쓴다.
struct FMaterialAsset
{
	static constexpr const wchar_t* Extension = L".emat";

	// 덮어쓰기 대상 필드 (OverrideMask 비트). 텍스처 슬롯은 Field_Texture0 + 슬롯
	enum EField : uint32
	{
		Field_BaseColorFactor   = 1u << 0,
		Field_EmissiveFactor    = 1u << 1,
		Field_Metallic          = 1u << 2,
		Field_Roughness         = 1u << 3,
		Field_NormalScale       = 1u << 4,
		Field_OcclusionStrength = 1u << 5,
		Field_AlphaCutoff       = 1u << 6,
		Field_BlendMode         = 1u << 7,
		Field_TwoSided          = 1u << 8,
		Field_Texture0          = 1u << 9, // ~ 1u << 13
		Field_All               = (1u << (9 + MaterialSlot_Count)) - 1u,
	};
	static constexpr uint32 GetTextureField(uint32 Slot) { return Field_Texture0 << Slot; }
	static constexpr uint32 MaxParentDepth = 16;

	std::string        Name;
	std::string        TexturePaths[MaterialSlot_Count]; // EMaterialTextureSlot 순서
	FMaterialConstants Constants;
	EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque;
	bool               bTwoSided = false;
	std::string        Parent;                  // 비어 있으면 일반 머티리얼
	uint32             OverrideMask = Field_All; // 인스턴스가 덮어쓰는 필드 (Parent가 없으면 무시 — 모든 필드가 자기 값)

	bool IsInstance() const { return !Parent.empty(); }
	bool Overrides(uint32 Field) const { return !IsInstance() || (OverrideMask & Field) != 0; }

	static const char*        GetBlendModeName(EMaterialBlendMode Mode);
	static EMaterialBlendMode ParseBlendMode(const std::string& Name, EMaterialBlendMode Fallback = EMaterialBlendMode::Opaque);

	// 슬롯별 JSON 키 ("BaseColorTexture" 등)
	static const char* GetTextureKey(uint32 Slot);
	// 슬롯별 텍스처 용도 (쿠킹 압축 형식/색공간 결정)
	static ETextureUsage GetSlotUsage(uint32 Slot)
	{
		switch (Slot)
		{
		case MaterialSlot_BaseColor:
		case MaterialSlot_Emissive:  return ETextureUsage::Color;
		case MaterialSlot_Normal:    return ETextureUsage::Normal;
		case MaterialSlot_Occlusion: return ETextureUsage::Mask;
		default:                     return ETextureUsage::Linear;
		}
	}

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json);

	bool LoadFromFile(const std::filesystem::path& Path);
	bool SaveToFile(const std::filesystem::path& Path) const;

	// 경로의 .emat를 읽는 함수 (FResourceManager: 편집 중 내용 → 디스크, 테스트: 메모리)
	using FLoader = std::function<bool(const std::filesystem::path& Path, FMaterialAsset& OutAsset)>;
	// 인스턴스 해석: Path(이 에셋 파일 경로) 기준으로 부모 체인을 읽어 평탄한 머티리얼(Parent 없음, 텍스처는 Path 폴더 기준)을 만든다.
	// OutChain = 부모 경로 (가까운 부모부터, lexically_normal). 순환·깊이 초과·부모 읽기 실패면 오류를 남기고 false
	// (OutResolved는 읽은 데까지의 값으로 채워진다 — 끊긴 지점 위는 기본값)
	static bool Resolve(const FMaterialAsset& Asset, const std::filesystem::path& Path, const FLoader& Loader, FMaterialAsset& OutResolved,
	                    std::vector<std::filesystem::path>* OutChain = nullptr, std::string* OutError = nullptr);
	// 경로 비교 키 (정규화 + 소문자 + '/' 구분) — 순환 검출/의존 목록 비교에 쓴다
	static std::wstring MakePathKey(const std::filesystem::path& Path);
};
