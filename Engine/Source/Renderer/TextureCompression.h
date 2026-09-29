#pragma once

#include "Core/CoreTypes.h"
#include "Renderer/Image.h"

#include <vector>

// 쿠킹된 텍스처 저장 형식
enum class ETextureFormat : uint8
{
	RGBA8, // 비압축 (블록 크기 불일치 등 압축 불가 시)
	BC7,   // RGBA 8bpp (색상/패킹 데이터)
	BC5,   // RG 8bpp (탄젠트 공간 노멀 XY — 셰이더가 Z 재구성)
	BC4,   // R 4bpp (단일 채널 마스크: AO 등)
};

// 텍스처 용도: 밉 필터링 방식과 압축 형식을 결정
enum class ETextureUsage : uint8
{
	Color,  // sRGB 색상 (베이스/발광): 선형 공간 평균, BC7 지각 가중치
	Linear, // 선형 데이터 (금속/거칠기 등 채널 패킹): BC7 선형 가중치
	Normal, // 탄젠트 공간 노멀: 벡터 평균 후 재정규화, BC5
	Mask,   // 단일 채널 (R): BC4
};

struct FTextureMip
{
	uint32             Width  = 0;
	uint32             Height = 0;
	std::vector<uint8> Data; // 블록 형식이면 블록 행 순서로 빈틈없이, RGBA8이면 행 순서
};

// CPU 측 쿠킹 텍스처 (전체 밉 체인 포함)
struct FCompressedTexture
{
	ETextureFormat           Format = ETextureFormat::RGBA8;
	bool                     bSRGB  = false;
	std::vector<FTextureMip> Mips;

	bool   IsValid() const { return !Mips.empty() && Mips[0].Width > 0 && Mips[0].Height > 0; }
	uint32 GetWidth() const { return Mips.empty() ? 0 : Mips[0].Width; }
	uint32 GetHeight() const { return Mips.empty() ? 0 : Mips[0].Height; }
	size_t GetTotalBytes() const;
};

namespace TextureCompression
{
	// 블록 형식이면 블록(4x4)당 바이트, RGBA8이면 0
	uint32 GetBlockBytes(ETextureFormat Format);
	// 한 밉의 데이터 크기와 행(블록 행) 수/행 바이트
	size_t GetMipDataSize(ETextureFormat Format, uint32 Width, uint32 Height);
	uint32 GetRowCount(ETextureFormat Format, uint32 Height);
	uint32 GetRowBytes(ETextureFormat Format, uint32 Width);

	// 용도에 맞는 필터로 전체 밉 체인 생성 (밉 0 = 원본 복사). 크기는 max(1, 절반)
	std::vector<FImage> GenerateMips(const FImage& Base, ETextureUsage Usage);

	// 밉 생성 + 압축. 밉 0 크기가 4의 배수가 아니면 RGBA8(비압축)로 대체한다 (D3D12 BC 제약)
	//   bSRGB는 Color 용도에서만 true (색상은 sRGB 뷰로 샘플링)
	FCompressedTexture Compress(const FImage& Base, ETextureUsage Usage);

	// 검증/미리보기용 CPU 디코딩 (RGBA8). GPU 샘플링과 같게 BC5는 (R, G, 0, 255), BC4는 (R, 0, 0, 255)
	bool Decompress(const FCompressedTexture& Texture, uint32 MipIndex, FImage& OutImage);
}
