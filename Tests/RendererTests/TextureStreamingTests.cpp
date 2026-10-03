#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/AssetCache.h"
#include "Renderer/MaterialGraph.h"
#include "Renderer/MeshData.h"
#include "Renderer/TextureStreamingMath.h"

#include <cmath>
#include <vector>

// 텍스처 밉 스트리밍 순수 식 (Phase 53 — Renderer/TextureStreamingMath.h)

namespace
{
	using namespace TextureStreamingMath;

	// 밉마다 다른 바이트로 채운 전체 밉 체인 (압축하지 않고 크기만 형식에 맞춘다)
	FCompressedTexture MakeTexture(ETextureFormat Format, uint32 Width, uint32 Height, bool bSRGB = false)
	{
		FCompressedTexture Texture;
		Texture.Format = Format;
		Texture.bSRGB  = bSRGB;
		uint32 W = Width, H = Height;
		for (uint32 Mip = 0;; ++Mip)
		{
			FTextureMip& Out = Texture.Mips.emplace_back();
			Out.Width        = W;
			Out.Height       = H;
			Out.Data.resize(TextureCompression::GetMipDataSize(Format, W, H));
			for (size_t Index = 0; Index < Out.Data.size(); ++Index)
			{
				Out.Data[Index] = static_cast<uint8>(Mip * 37 + Index * 13);
			}
			if (W == 1 && H == 1)
			{
				break;
			}
			W = std::max(1u, W / 2);
			H = std::max(1u, H / 2);
		}
		return Texture;
	}

	FVertex MakeVertex(float X, float Y, float U, float V)
	{
		FVertex Vertex;
		Vertex.Position = FVector3(X, Y, 0.0f);
		Vertex.UV       = FVector2(U, V);
		return Vertex;
	}
} // namespace

E_TEST(TextureStreaming_PayloadLayoutMatchesCookedTexture)
{
	// .etex 직렬화 결과와 배치 식이 같은 위치를 가리켜야 밉 단위로 다시 읽을 수 있다
	for (const ETextureFormat Format : { ETextureFormat::BC7, ETextureFormat::BC4, ETextureFormat::RGBA8 })
	{
		const FCompressedTexture Texture = MakeTexture(Format, 512, 256, Format == ETextureFormat::BC7);
		FBinaryWriter            Writer;
		FAssetCache::WriteTexture(Writer, Texture);
		const std::vector<uint8>& File    = Writer.GetBuffer();
		const uint8*              Payload = File.data() + FAssetCache::TexturePayloadOffset;
		const size_t              Size    = File.size() - FAssetCache::TexturePayloadOffset;

		const FPayloadLayout Layout = ParsePayloadProbe(Payload, PayloadProbeSize);
		E_EXPECT_TRUE(Layout.IsValid());
		E_EXPECT_EQ(Layout.MipCount, static_cast<uint32>(Texture.Mips.size()));
		E_EXPECT_EQ(Layout.TotalSize, static_cast<uint64>(Size));
		E_EXPECT_EQ(Layout.bSRGB, Format == ETextureFormat::BC7);
		for (uint32 Mip = 0; Mip < Layout.MipCount; ++Mip)
		{
			E_EXPECT_EQ(Layout.Mips[Mip].Width, Texture.Mips[Mip].Width);
			E_EXPECT_EQ(Layout.Mips[Mip].DataSize, static_cast<uint64>(Texture.Mips[Mip].Data.size()));
		}
		// 밉 3부터 범위 읽기 → 같은 데이터
		const uint32             Top = 3;
		std::vector<FTextureMip> Mips;
		E_EXPECT_TRUE(ParseMipRange(Layout, Top, Payload + Layout.GetRangeReadOffset(Top), Layout.GetRangeReadSize(Top), Mips));
		E_EXPECT_EQ(Mips.size(), Texture.Mips.size() - Top);
		bool bSame = true;
		for (size_t Index = 0; Index < Mips.size(); ++Index)
		{
			bSame &= Mips[Index].Data == Texture.Mips[Top + Index].Data && Mips[Index].Width == Texture.Mips[Top + Index].Width;
		}
		E_EXPECT_TRUE(bSame);
		E_EXPECT_EQ(Layout.GetRangeDataBytes(Top), static_cast<uint64>(TextureCompression::GetMipDataSize(Format, 64, 32)) +
		                                               Layout.GetRangeDataBytes(Top + 1));
	}
}

E_TEST(TextureStreaming_ParseMipRangeRejectsChangedFile)
{
	const FCompressedTexture Texture = MakeTexture(ETextureFormat::BC7, 256, 256);
	FBinaryWriter            Writer;
	FAssetCache::WriteTexture(Writer, Texture);
	std::vector<uint8>   File   = Writer.GetBuffer();
	uint8*               Payload = File.data() + FAssetCache::TexturePayloadOffset;
	const FPayloadLayout Layout = ParsePayloadProbe(Payload, PayloadProbeSize);
	// 다른 크기로 다시 쿠킹된 것처럼 밉 2 머리의 폭을 바꾼다 → 거부 (재임포트 뒤 낡은 오프셋)
	Payload[Layout.Mips[2].RecordOffset] ^= 0x01;
	std::vector<FTextureMip> Mips;
	E_EXPECT_FALSE(ParseMipRange(Layout, 1, Payload + Layout.GetRangeReadOffset(1), Layout.GetRangeReadSize(1), Mips));
	E_EXPECT_TRUE(Mips.empty());
	// 크기가 맞지 않는 버퍼도 거부
	E_EXPECT_FALSE(ParseMipRange(Layout, 1, Payload + Layout.GetRangeReadOffset(1), Layout.GetRangeReadSize(1) - 1, Mips));
	// 잘못된 머리
	std::vector<uint8> Bad(PayloadProbeSize, 0);
	Bad[0] = 99;
	E_EXPECT_FALSE(ParsePayloadProbe(Bad.data(), Bad.size()).IsValid());
}

E_TEST(TextureStreaming_ValidTopAndTail)
{
	// 2의 거듭제곱 BC: 큰 변 64 밉이 꼬리 시작
	E_EXPECT_EQ(ComputeTailTopMip(ETextureFormat::BC7, 1024, 1024, 11), 4u);
	E_EXPECT_EQ(ComputeTailTopMip(ETextureFormat::BC7, 2048, 1024, 12), 5u); // 64x32
	E_EXPECT_EQ(GetMaxValidTopMip(ETextureFormat::BC7, 1024, 1024, 11), 8u); // 4x4까지 (BC 블록)
	E_EXPECT_EQ(GetMaxValidTopMip(ETextureFormat::RGBA8, 1024, 1024, 11), 10u);
	// 큰 변 256 이하는 스트리밍하지 않음
	E_EXPECT_EQ(ComputeTailTopMip(ETextureFormat::BC7, 256, 256, 9), 0u);
	E_EXPECT_EQ(ComputeTailTopMip(ETextureFormat::BC7, 512, 64, 10), 3u);
	// 2의 거듭제곱이 아닌 크기: 2^k로 나누어떨어지고 블록 4의 배수인 밉까지만 (1000x600 → 밉 1 = 500x300)
	E_EXPECT_EQ(GetMaxValidTopMip(ETextureFormat::BC7, 1000, 600, 10), 1u);
	E_EXPECT_EQ(ComputeTailTopMip(ETextureFormat::BC7, 1000, 600, 10), 1u);
	E_EXPECT_EQ(GetMaxValidTopMip(ETextureFormat::BC7, 1000, 600, 1), 0u); // 밉 1개
	E_EXPECT_EQ(ComputeTailTopMip(ETextureFormat::BC7, 1000, 600, 1), 0u);
}

E_TEST(TextureStreaming_RequiredMipFromScreenSize)
{
	// 픽셀 크기: 깊이 1000cm, tan(시야/2) = 0.5, 화면 1000px → 1cm/픽셀
	E_EXPECT_NEAR(ComputePerspectiveCmPerPixel(1000.0f, 0.5f, 1000), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(ComputeOrthographicCmPerPixel(500.0f, 1000), 0.5f, 1.0e-6f);
	E_EXPECT_EQ(ComputePerspectiveCmPerPixel(0.0f, 0.5f, 1000), 0.0f);

	// 1024 텍스처, UV 1 = 100cm → 10.24 텍셀/cm, 1cm/픽셀 → LOD log2(10.24) = 3.36 → 밉 3 - 여유 1 = 2
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 1.0f, 0.0f, 1, 11), 2u);
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 1.0f, 0.0f, 0, 11), 3u);
	// TAAU 밉 바이어스(음수 = 더 세밀) 반영: 2.36 → 밉 2 - 1 = 1
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 1.0f, -1.0f, 1, 11), 1u);
	// 두 배 멀면 한 밉 덜 세밀, 두 배 가까우면 한 밉 더
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 2.0f, 0.0f, 1, 11), 3u);
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 0.5f, 0.0f, 1, 11), 1u);
	// UV 밀도가 두 배(타일링 2)면 한 밉 덜 세밀 (같은 픽셀에 텍셀이 두 배)
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.02f, 1.0f, 0.0f, 1, 11), 3u);
	// 확대(텍셀 < 픽셀)·알 수 없는 밀도·카메라 안쪽은 전체
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.0001f, 1.0f, 0.0f, 1, 11), 0u);
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.0f, 1.0f, 0.0f, 1, 11), 0u);
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 0.0f, 0.0f, 1, 11), 0u);
	// 아주 멀면 마지막 밉에서 멈춘다
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, 0.01f, 1.0e6f, 0.0f, 1, 11), 10u);
	// 텍스처 크기와 무관한 부분(머티리얼마다 최솟값)으로 나눠 계산해도 같다
	E_EXPECT_EQ(ComputeRequiredTopMip(1024, ComputeLog2UvPerPixel(0.01f, 1.0f) + std::log2(2.0f), 0.0f, 1, 11), 3u);
}

E_TEST(TextureStreaming_UvDensity)
{
	// 100cm × 100cm 사각형에 UV 0~1 → 0.01 UV/cm
	std::vector<FVertex> Vertices = { MakeVertex(0, 0, 0, 0), MakeVertex(100, 0, 1, 0), MakeVertex(100, 100, 1, 1), MakeVertex(0, 100, 0, 1) };
	std::vector<uint32>  Indices  = { 0, 2, 1, 0, 3, 2 };
	E_EXPECT_NEAR(ComputeUvDensity(Vertices, Indices), 0.01f, 1.0e-6f);
	// UV 4배 타일링 → 0.04
	for (FVertex& Vertex : Vertices)
	{
		Vertex.UV = Vertex.UV * 4.0f;
	}
	E_EXPECT_NEAR(ComputeUvDensity(Vertices, Indices), 0.04f, 1.0e-6f);
	// UV 없음/한 점으로 모임(모두 0) → 0 (UV 미분 0 → 밉 0 필요)
	for (FVertex& Vertex : Vertices)
	{
		Vertex.UV = FVector2(0.0f, 0.0f);
	}
	E_EXPECT_EQ(ComputeUvDensity(Vertices, Indices), 0.0f);

	// 면적 가중 하위 백분위: 낮은 밀도(텍스처가 늘어나 세밀한 밉 필요)가 기준. 큰 면(0.01) + 아주 작은 조각(0.0001, 면적 0.5)이면
	// 하위 1%는 작은 조각을 빼고 큰 면 값, 0%(최솟값)는 조각 값
	std::vector<FVertex> Mixed = { MakeVertex(0, 0, 0, 0),   MakeVertex(100, 0, 1, 0),      MakeVertex(100, 100, 1, 1), MakeVertex(0, 100, 0, 1),
	                               MakeVertex(200, 0, 0, 0), MakeVertex(201, 0, 0.0001f, 0), MakeVertex(201, 1, 0.0001f, 0.0001f) };
	std::vector<uint32>  MixedIndices = { 0, 2, 1, 0, 3, 2, 4, 6, 5 };
	E_EXPECT_NEAR(ComputeUvDensity(Mixed, MixedIndices, 0.01f), 0.01f, 1.0e-6f);
	E_EXPECT_NEAR(ComputeUvDensity(Mixed, MixedIndices, 0.0f), 0.0001f, 1.0e-6f);
	// 밀도가 높은(작게 축소된) 조각은 기준을 바꾸지 않는다
	Mixed[5].UV = FVector2(1.0f, 0.0f);
	Mixed[6].UV = FVector2(1.0f, 1.0f);
	E_EXPECT_NEAR(ComputeUvDensity(Mixed, MixedIndices, 0.0f), 0.01f, 1.0e-6f);
	// UV가 모인 면이 면적의 1%를 넘으면 0 (보수적: 밉 0)
	std::vector<FVertex> Collapsed = Vertices; // UV 모두 0인 100x100 면
	Collapsed.push_back(MakeVertex(300, 0, 0, 0));
	Collapsed.push_back(MakeVertex(310, 0, 1, 0));
	Collapsed.push_back(MakeVertex(310, 10, 1, 1));
	std::vector<uint32> CollapsedIndices = { 0, 2, 1, 0, 3, 2, 4, 6, 5 };
	E_EXPECT_EQ(ComputeUvDensity(Collapsed, CollapsedIndices), 0.0f);
}

E_TEST(TextureStreaming_GraphUvScale)
{
	FMaterialGraph Graph;
	FMaterialGraphNode& Plain = Graph.Nodes.emplace_back();
	Plain.Id                  = "Plain";
	Plain.Type                = "TextureSample";
	Plain.Name                = "Albedo";
	E_EXPECT_NEAR(ComputeGraphTextureUvScale(Graph, "Albedo"), 1.0f, 0.0f); // UV 핀 연결 없음 = UV0
	E_EXPECT_NEAR(ComputeGraphTextureUvScale(Graph, "Unused"), 1.0f, 0.0f); // 쓰지 않는 파라미터

	FMaterialGraphNode& Tiling = Graph.Nodes.emplace_back();
	Tiling.Id                  = "Tiling";
	Tiling.Type                = "TexCoord";
	Tiling.Value               = FVector4(4.0f, 2.0f, 0.0f, 0.0f);
	FMaterialGraphNode& Tiled  = Graph.Nodes.emplace_back();
	Tiled.Id                   = "Tiled";
	Tiled.Type                 = "TextureSample";
	Tiled.Name                 = "Detail";
	Tiled.Inputs.push_back({ "UV", "Tiling", 0 });
	E_EXPECT_NEAR(ComputeGraphTextureUvScale(Graph, "Detail"), 4.0f, 0.0f); // 큰 축 타일링

	// 같은 파라미터를 다른 곳에서 월드 위치 UV로도 읽으면 알 수 없음
	FMaterialGraphNode& World = Graph.Nodes.emplace_back();
	World.Id                  = "World";
	World.Type                = "WorldPosition";
	FMaterialGraphNode& Projected = Graph.Nodes.emplace_back();
	Projected.Id                  = "Projected";
	Projected.Type                = "TextureSample";
	Projected.Name                = "Detail";
	Projected.Inputs.push_back({ "UV", "World", 0 });
	E_EXPECT_TRUE(ComputeGraphTextureUvScale(Graph, "Detail") < 0.0f);
	E_EXPECT_NEAR(ComputeGraphTextureUvScale(Graph, "Albedo"), 1.0f, 0.0f);

	// 상수 UV (연결 없는 입력 값) → 알 수 없음 (미분 0 → 하드웨어는 밉 0)
	FMaterialGraphNode& Constant = Graph.Nodes.emplace_back();
	Constant.Id                  = "Constant";
	Constant.Type                = "TextureSample";
	Constant.Name                = "Albedo";
	Constant.Inputs.push_back({ "UV", "", 0 });
	E_EXPECT_TRUE(ComputeGraphTextureUvScale(Graph, "Albedo") < 0.0f);
}

E_TEST(TextureStreaming_HysteresisDropsLate)
{
	FHysteresisState State;
	E_EXPECT_EQ(UpdateHysteresis(State, 3, 0.016f, 2.0f), 3u); // 처음: 그대로
	E_EXPECT_EQ(UpdateHysteresis(State, 1, 0.016f, 2.0f), 1u); // 더 세밀: 바로
	// 덜 세밀해도 되면 2초 동안 유지
	uint32 Held = 1;
	for (int32 Frame = 0; Frame < 100; ++Frame)
	{
		Held = UpdateHysteresis(State, 4, 0.016f, 2.0f);
	}
	E_EXPECT_EQ(Held, 1u); // 1.6초
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Held = UpdateHysteresis(State, 4, 0.016f, 2.0f);
	}
	E_EXPECT_EQ(Held, 4u); // 2초 넘음
	// 다시 세밀해지면 바로, 잠깐 내렸다 올라오면 타이머가 처음부터
	E_EXPECT_EQ(UpdateHysteresis(State, 2, 0.016f, 2.0f), 2u);
	E_EXPECT_EQ(UpdateHysteresis(State, 5, 1.5f, 2.0f), 2u);
	E_EXPECT_EQ(UpdateHysteresis(State, 2, 0.016f, 2.0f), 2u);
	E_EXPECT_EQ(UpdateHysteresis(State, 5, 1.5f, 2.0f), 2u);
	E_EXPECT_EQ(UpdateHysteresis(State, 5, 0.6f, 2.0f), 5u);
	// 지연 0 = 바로 내림
	E_EXPECT_EQ(UpdateHysteresis(State, 7, 0.0f, 0.0f), 7u);
}

E_TEST(TextureStreaming_BudgetDropsLowPriorityFirst)
{
	// 텍스처 두 장: 1024² BC7 (밉별 [k..) 바이트). 꼬리 = 밉 4
	const FPayloadLayout Layout = ComputePayloadLayout(ETextureFormat::BC7, false, 1024, 1024, 11);
	std::vector<uint64>  Range(Layout.MipCount + 1, 0);
	for (uint32 Mip = Layout.MipCount; Mip-- > 0;)
	{
		Range[Mip] = Range[Mip + 1] + Layout.Mips[Mip].DataSize;
	}
	const FBudgetItem Important{ 0, 4, 10.0f, Range.data() };
	const FBudgetItem Minor{ 0, 4, 1.0f, Range.data() };

	// 넉넉하면 원하는 그대로
	FBudgetResult Result = FitToBudget({ Important, Minor }, Range[0] * 2);
	E_EXPECT_EQ(Result.Tops[0], 0u);
	E_EXPECT_EQ(Result.Tops[1], 0u);
	E_EXPECT_FALSE(Result.bOverBudget);

	// 한 장 반: 우선순위가 낮은 쪽이 먼저 줄어든다
	Result = FitToBudget({ Important, Minor }, Range[0] + Range[1]);
	E_EXPECT_EQ(Result.Tops[0], 0u);
	E_EXPECT_EQ(Result.Tops[1], 1u);
	E_EXPECT_TRUE(Result.TotalBytes <= Range[0] + Range[1]);

	// 더 빠듯하면 낮은 쪽이 여러 단계 (키 = 우선순위 × (줄인 단계 + 1)) — 1×(1..)이 10보다 작은 동안 낮은 쪽만
	Result = FitToBudget({ Important, Minor }, Range[0] + Range[3]);
	E_EXPECT_EQ(Result.Tops[0], 0u);
	E_EXPECT_EQ(Result.Tops[1], 3u);

	// 둘 다 꼬리까지 줄여도 넘치면 초과 표시
	Result = FitToBudget({ Important, Minor }, Range[4]);
	E_EXPECT_EQ(Result.Tops[0], 4u);
	E_EXPECT_EQ(Result.Tops[1], 4u);
	E_EXPECT_TRUE(Result.bOverBudget);

	// 같은 우선순위면 고르게, 같은 값은 번호가 작은 쪽부터 (결정적)
	Result = FitToBudget({ Minor, Minor }, Range[0] + Range[1]);
	E_EXPECT_EQ(Result.Tops[0], 1u);
	E_EXPECT_EQ(Result.Tops[1], 0u);
	Result = FitToBudget({ Minor, Minor }, Range[1] * 2);
	E_EXPECT_EQ(Result.Tops[0], 1u);
	E_EXPECT_EQ(Result.Tops[1], 1u);

	// 원하는 밉이 이미 덜 세밀하면 그보다 더 세밀하게 올리지 않는다
	Result = FitToBudget({ FBudgetItem{ 2, 4, 1.0f, Range.data() } }, Range[0]);
	E_EXPECT_EQ(Result.Tops[0], 2u);
}

E_TEST(TextureStreaming_CookedModelImageOffsets)
{
	// 쿠킹 모델: 기록할 때의 이미지 본문 위치 == 읽을 때 기록한 위치, 메시 UV 밀도 보존 (ModelVersion 9)
	FModelData Model;
	Model.Name = "Streaming";
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FModelImage& Image = Model.Images.emplace_back();
		Image.Name         = Index == 0 ? "Base" : "Normal";
		Image.Texture      = MakeTexture(Index == 0 ? ETextureFormat::BC7 : ETextureFormat::BC5, 512, 512);
	}
	FModelMesh& Mesh   = Model.Meshes.emplace_back();
	Mesh.Name          = "Quad";
	Mesh.Data.Vertices = { MakeVertex(0, 0, 0, 0), MakeVertex(100, 0, 1, 0), MakeVertex(100, 100, 1, 1) };
	Mesh.Data.Indices  = { 0, 2, 1 };
	FAssetCache::ComputeModelUvDensities(Model);
	E_EXPECT_NEAR(Mesh.Data.UvDensity, 0.01f, 1.0e-6f);
	FModelNode& Node = Model.Nodes.emplace_back();
	Node.Name        = "Root";
	Node.Meshes      = { 0 };
	Model.RootNodes  = { 0 };

	FBinaryWriter       Writer;
	std::vector<uint64> Offsets;
	FAssetCache::WriteModel(Writer, Model, &Offsets);
	E_EXPECT_EQ(Offsets.size(), static_cast<size_t>(2));

	FModelData   Read;
	FBinaryReader Reader(Writer.GetBuffer().data(), Writer.GetBuffer().size());
	E_EXPECT_TRUE(FAssetCache::ReadModel(Reader, Read));
	E_EXPECT_EQ(Read.Images.size(), static_cast<size_t>(2));
	E_EXPECT_NEAR(Read.Meshes[0].Data.UvDensity, 0.01f, 1.0e-6f);
	for (size_t Index = 0; Index < Read.Images.size(); ++Index)
	{
		E_EXPECT_EQ(Read.Images[Index].CookedPayloadOffset, Offsets[Index]);
		// 그 위치에서 배치를 읽어 밉 2부터 다시 읽으면 원본과 같다
		const uint8*         Payload = Writer.GetBuffer().data() + Offsets[Index];
		const FPayloadLayout Layout  = ParsePayloadProbe(Payload, PayloadProbeSize);
		std::vector<FTextureMip> Mips;
		E_EXPECT_TRUE(ParseMipRange(Layout, 2, Payload + Layout.GetRangeReadOffset(2), Layout.GetRangeReadSize(2), Mips));
		E_EXPECT_TRUE(!Mips.empty() && Mips[0].Data == Model.Images[Index].Texture.Mips[2].Data);
	}
}
