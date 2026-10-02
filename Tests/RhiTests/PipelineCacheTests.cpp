#include "Core/Testing/TestFramework.h"
#include "RHI/D3D12/D3D12PipelineRecipe.h"

#include <vector>

// PSO 캐시 순수 규칙 (Phase 48): 레시피 직렬화·키·레시피 파일·드라이버 캐시 무효화 (GPU 없음)

namespace
{
	const uint8 VertexCode[] = { 'D', 'X', 'B', 'C', 1, 2, 3, 4, 5, 6, 7, 8 };
	const uint8 PixelCode[]  = { 'D', 'X', 'B', 'C', 9, 9, 9, 9 };
	const uint8 RootBlob[]   = { 0x10, 0x20, 0x30, 0x40 };

	const D3D12_INPUT_ELEMENT_DESC Elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC MakeDesc()
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
		Desc.pRootSignature                           = reinterpret_cast<ID3D12RootSignature*>(0x1234); // 키에 들어가지 않는다
		Desc.VS                                       = { VertexCode, sizeof(VertexCode) };
		Desc.PS                                       = { PixelCode, sizeof(PixelCode) };
		Desc.BlendState.RenderTarget[0].BlendEnable   = TRUE;
		Desc.BlendState.RenderTarget[0].SrcBlend      = D3D12_BLEND_SRC_ALPHA;
		Desc.BlendState.RenderTarget[0].DestBlend     = D3D12_BLEND_INV_SRC_ALPHA;
		Desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		Desc.SampleMask                               = UINT_MAX;
		Desc.RasterizerState.FillMode                 = D3D12_FILL_MODE_SOLID;
		Desc.RasterizerState.CullMode                 = D3D12_CULL_MODE_BACK;
		Desc.RasterizerState.DepthBias                = -3;
		Desc.RasterizerState.SlopeScaledDepthBias     = -1.5f;
		Desc.DepthStencilState.DepthEnable            = TRUE;
		Desc.DepthStencilState.DepthWriteMask         = D3D12_DEPTH_WRITE_MASK_ALL;
		Desc.DepthStencilState.DepthFunc              = D3D12_COMPARISON_FUNC_EQUAL;
		Desc.DepthStencilState.StencilReadMask        = 0xFF;
		Desc.InputLayout                              = { Elements, 2 };
		Desc.PrimitiveTopologyType                    = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		Desc.NumRenderTargets                         = 2;
		Desc.RTVFormats[0]                            = DXGI_FORMAT_R16G16B16A16_FLOAT;
		Desc.RTVFormats[1]                            = DXGI_FORMAT_R16G16_FLOAT;
		Desc.DSVFormat                                = DXGI_FORMAT_D32_FLOAT;
		Desc.SampleDesc                               = { 1, 0 };
		return Desc;
	}
} // namespace

E_TEST(PipelineCache_RecipeRoundTripAndKey)
{
	const uint64                     RootHash = PipelineCache::HashBytes(RootBlob, sizeof(RootBlob));
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = MakeDesc();
	E_EXPECT_TRUE(PipelineCache::IsCacheable(Desc));
	const PipelineCache::FRecipe Recipe = PipelineCache::FromDesc(Desc, RootHash);
	const std::vector<uint8>     Bytes  = PipelineCache::Serialize(Recipe);

	PipelineCache::FRecipe Restored;
	E_EXPECT_TRUE(PipelineCache::Deserialize(Bytes, Restored));
	E_EXPECT_TRUE(PipelineCache::Serialize(Restored) == Bytes);
	E_EXPECT_EQ(PipelineCache::ComputeKey(Restored), PipelineCache::ComputeKey(Recipe));

	// 되살린 설명이 원래와 같은 상태
	std::vector<D3D12_INPUT_ELEMENT_DESC> OutElements;
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC Rebuilt = PipelineCache::ToGraphicsDesc(Restored, Desc.pRootSignature, Desc.VS, Desc.PS, OutElements);
	E_EXPECT_EQ(Rebuilt.InputLayout.NumElements, 2u);
	E_EXPECT_TRUE(std::string(Rebuilt.InputLayout.pInputElementDescs[1].SemanticName) == "TEXCOORD");
	E_EXPECT_EQ(Rebuilt.InputLayout.pInputElementDescs[1].AlignedByteOffset, 12u);
	E_EXPECT_EQ(Rebuilt.RasterizerState.DepthBias, -3);
	E_EXPECT_NEAR(Rebuilt.RasterizerState.SlopeScaledDepthBias, -1.5f, 0.0f);
	E_EXPECT_TRUE(Rebuilt.DepthStencilState.DepthFunc == D3D12_COMPARISON_FUNC_EQUAL);
	E_EXPECT_TRUE(Rebuilt.BlendState.RenderTarget[0].DestBlend == D3D12_BLEND_INV_SRC_ALPHA);
	E_EXPECT_TRUE(Rebuilt.RTVFormats[1] == DXGI_FORMAT_R16G16_FLOAT);
	E_EXPECT_TRUE(Rebuilt.DSVFormat == DXGI_FORMAT_D32_FLOAT);

	// 잘린 바이트는 실패
	PipelineCache::FRecipe Broken;
	E_EXPECT_FALSE(PipelineCache::Deserialize(std::span<const uint8>(Bytes.data(), Bytes.size() - 1), Broken));
}

E_TEST(PipelineCache_KeyChangesWithBytecodeAndState)
{
	const uint64 RootHash = PipelineCache::HashBytes(RootBlob, sizeof(RootBlob));
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC Base = MakeDesc();
	const uint64 BaseKey = PipelineCache::ComputeKey(PipelineCache::FromDesc(Base, RootHash));

	// 루트 시그니처 포인터는 키와 무관 (같은 블롭이면 같은 키)
	D3D12_GRAPHICS_PIPELINE_STATE_DESC OtherPointer = Base;
	OtherPointer.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(0x9999);
	E_EXPECT_EQ(PipelineCache::ComputeKey(PipelineCache::FromDesc(OtherPointer, RootHash)), BaseKey);

	// 셰이더 바이트코드가 바뀌면 (핫 리로드·머티리얼 변형) 새 키
	const uint8 NewPixel[] = { 'D', 'X', 'B', 'C', 9, 9, 9, 8 };
	D3D12_GRAPHICS_PIPELINE_STATE_DESC NewShader = Base;
	NewShader.PS = { NewPixel, sizeof(NewPixel) };
	E_EXPECT_TRUE(PipelineCache::ComputeKey(PipelineCache::FromDesc(NewShader, RootHash)) != BaseKey);

	// 루트 시그니처 블롭 / 상태 하나 / 렌더 타깃 형식이 바뀌어도 새 키
	E_EXPECT_TRUE(PipelineCache::ComputeKey(PipelineCache::FromDesc(Base, RootHash + 1)) != BaseKey);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC NewCull = Base;
	NewCull.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	E_EXPECT_TRUE(PipelineCache::ComputeKey(PipelineCache::FromDesc(NewCull, RootHash)) != BaseKey);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC NewFormat = Base;
	NewFormat.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	E_EXPECT_TRUE(PipelineCache::ComputeKey(PipelineCache::FromDesc(NewFormat, RootHash)) != BaseKey);
	// 쓰지 않는 렌더 타깃 칸의 쓰레기 값은 키에 들어가지 않는다
	D3D12_GRAPHICS_PIPELINE_STATE_DESC UnusedSlot = Base;
	UnusedSlot.RTVFormats[5] = DXGI_FORMAT_R8_UNORM;
	E_EXPECT_EQ(PipelineCache::ComputeKey(PipelineCache::FromDesc(UnusedSlot, RootHash)), BaseKey);

	// 지원하지 않는 설명은 캐시 안 함
	D3D12_GRAPHICS_PIPELINE_STATE_DESC WithGs = Base;
	WithGs.GS = { PixelCode, sizeof(PixelCode) };
	E_EXPECT_FALSE(PipelineCache::IsCacheable(WithGs));

	// 계산 레시피
	D3D12_COMPUTE_PIPELINE_STATE_DESC Compute{};
	Compute.pRootSignature = Base.pRootSignature;
	Compute.CS             = { VertexCode, sizeof(VertexCode) };
	const PipelineCache::FRecipe ComputeRecipe = PipelineCache::FromDesc(Compute, RootHash);
	PipelineCache::FRecipe       ComputeRestored;
	E_EXPECT_TRUE(PipelineCache::Deserialize(PipelineCache::Serialize(ComputeRecipe), ComputeRestored));
	E_EXPECT_TRUE(ComputeRestored.Type == PipelineCache::EPipelineType::Compute);
	E_EXPECT_TRUE(PipelineCache::ComputeKey(ComputeRecipe) != BaseKey);
	E_EXPECT_TRUE(PipelineCache::MakeLibraryName(BaseKey).size() == 20);
}

E_TEST(PipelineCache_RecipeFileWritePruneMerge)
{
	const uint64 RootHash = PipelineCache::HashBytes(RootBlob, sizeof(RootBlob));
	const PipelineCache::FRecipe Recipe = PipelineCache::FromDesc(MakeDesc(), RootHash);
	const uint64                 Key    = PipelineCache::ComputeKey(Recipe);

	PipelineCache::FRecipeFile File;
	File.RunCounter = 5;
	File.Recipes[Key] = { Recipe, 5 };
	E_EXPECT_FALSE(File.HasBlobs(Recipe)); // 블롭이 없으면 워밍 불가
	File.Blobs[RootHash] = std::vector<uint8>(std::begin(RootBlob), std::end(RootBlob));
	File.Blobs[Recipe.Shaders[0].Hash] = std::vector<uint8>(std::begin(VertexCode), std::end(VertexCode));
	File.Blobs[Recipe.Shaders[1].Hash] = std::vector<uint8>(std::begin(PixelCode), std::end(PixelCode));
	File.Blobs[12345] = { 1, 2, 3 }; // 아무도 안 쓰는 블롭 (해시 불일치 — 읽기에서 거부되므로 Prune으로 먼저 제거)
	E_EXPECT_TRUE(File.HasBlobs(Recipe));
	E_EXPECT_EQ(File.Prune(5, 8), 0u);
	E_EXPECT_EQ(File.Blobs.size(), size_t(3));

	PipelineCache::FRecipeFile Read;
	const std::vector<uint8>   Bytes = File.Write();
	E_EXPECT_TRUE(Read.Read(Bytes));
	E_EXPECT_EQ(Read.RunCounter, 5u);
	E_EXPECT_EQ(Read.Recipes.size(), size_t(1));
	E_EXPECT_TRUE(Read.Recipes.contains(Key)); // 키 = 레시피 바이트 해시 (읽기 쪽도 같은 키)
	E_EXPECT_TRUE(Read.HasBlobs(Read.Recipes.at(Key).Recipe));

	// 손상(블롭 바이트 변조)·다른 형식은 거부하고 비운다
	std::vector<uint8> Corrupt = Bytes;
	Corrupt[Corrupt.size() / 2] ^= 0xFF;
	PipelineCache::FRecipeFile Bad;
	E_EXPECT_FALSE(Bad.Read(Corrupt) && Bad.Recipes.size() == 1 && Bad.HasBlobs(Bad.Recipes.begin()->second.Recipe) &&
	               Bad.Write() == Bytes);
	std::vector<uint8> WrongMagic = Bytes;
	WrongMagic[0] ^= 0xFF;
	E_EXPECT_FALSE(Bad.Read(WrongMagic));
	E_EXPECT_TRUE(Bad.Recipes.empty() && Bad.Blobs.empty());

	// 나이: 8번 실행 넘게 안 쓰면 제거 + 블롭도 정리
	PipelineCache::FRecipeFile Old = Read;
	E_EXPECT_EQ(Old.Prune(13, 8), 0u); // 13 - 5 = 8 → 유지
	E_EXPECT_EQ(Old.Prune(14, 8), 1u);
	E_EXPECT_TRUE(Old.Recipes.empty() && Old.Blobs.empty());

	// 합치기: 같은 키는 최근 사용 번호
	PipelineCache::FRecipeFile Newer;
	Newer.Recipes[Key] = { Recipe, 9 };
	Read.Merge(Newer);
	E_EXPECT_EQ(Read.Recipes.at(Key).LastUsedRun, 9u);
	E_EXPECT_EQ(Read.Recipes.size(), size_t(1));
}

E_TEST(PipelineCache_LibraryHeaderInvalidation)
{
	PipelineCache::FLibraryHeader Current;
	Current.VendorId      = 0x10DE;
	Current.DeviceId      = 0x2520;
	Current.DriverVersion = 0x001F00000E0A1234ull;
	PipelineCache::FLibraryHeader Stored = Current;
	Stored.DataSize                      = 1024;
	E_EXPECT_TRUE(PipelineCache::IsLibraryCompatible(Stored, Current));

	PipelineCache::FLibraryHeader NewDriver = Stored;
	NewDriver.DriverVersion += 1;
	E_EXPECT_FALSE(PipelineCache::IsLibraryCompatible(NewDriver, Current));
	PipelineCache::FLibraryHeader OtherGpu = Stored;
	OtherGpu.DeviceId = 0x1234;
	E_EXPECT_FALSE(PipelineCache::IsLibraryCompatible(OtherGpu, Current));
	PipelineCache::FLibraryHeader OldFormat = Stored;
	OldFormat.FileVersion = PipelineCache::FLibraryHeader::Version + 1;
	E_EXPECT_FALSE(PipelineCache::IsLibraryCompatible(OldFormat, Current));
	PipelineCache::FLibraryHeader Empty = Stored;
	Empty.DataSize = 0;
	E_EXPECT_FALSE(PipelineCache::IsLibraryCompatible(Empty, Current));
}
