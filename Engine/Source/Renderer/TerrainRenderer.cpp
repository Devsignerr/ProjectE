#include "Renderer/TerrainRenderer.h"

#include "Core/CommandLine.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/IblRenderer.h"
#include "Renderer/ResourceCollector.h"
#include "Renderer/LocalLightRenderer.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/ShadowRenderer.h"
#include "Scene/Scene.h"
#include "Scene/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// Terrain.hlsl 루트 시그니처 (공간 0 = Mesh.hlsl 조명 리소스, 공간 1 = 지형)
	enum ETerrainRootParameter : uint32
	{
		TerrainParam_PerFrame = 0,        // b1
		TerrainParam_Shadow,              // b3
		TerrainParam_ShadowMap,           // t8 테이블
		TerrainParam_Ibl,                 // t5~t7 테이블
		TerrainParam_Cluster,             // b5
		TerrainParam_LocalLights,         // t9
		TerrainParam_ClusterData,         // t10
		TerrainParam_LocalShadowMatrices, // t11
		TerrainParam_LocalShadowMap,      // t12 테이블
		TerrainParam_Constants,           // b0 space1
		TerrainParam_Draw,                // b1 space1 (루트 상수 2개)
		TerrainParam_LightViewProjection, // b2 space1 (루트 상수 16개)
		TerrainParam_Heights,             // t0 space1 테이블
		TerrainParam_Weights,             // t1 space1 테이블
		TerrainParam_Chunks,              // t2 space1 루트 SRV
		TerrainParam_Layer0,              // t3~t7 space1 테이블
		TerrainParam_Layer1,              // t8~t12
		TerrainParam_Layer2,              // t13~t17
		TerrainParam_Layer3,              // t18~t22
		// Phase 33 화면 효과 (공간 0, Mesh.hlsl과 같은 레지스터): SSAO, DBuffer, 반사 캡처, SSR
		TerrainParam_AmbientOcclusion,    // t16 테이블
		TerrainParam_DBufferA,            // t17~t19 테이블 3개
		TerrainParam_DBufferB,
		TerrainParam_DBufferC,
		TerrainParam_ReflectionCaptures,  // t20 루트 SRV
		TerrainParam_CaptureAtlas,        // t21 테이블
		TerrainParam_ScreenReflection,    // t22 테이블
		TerrainParam_RayTracedShadowMask, // t24 테이블 (Phase 50 RT 방향광 그림자 — PerFrame RayTracedShadows)
		TerrainParam_LightTextures,       // 공간 3 t0~ (셰이더 가시 힙 전체 — 면광원 LTC·IES·쿠키, Phase 52)
		TerrainParam_DdgiConstants,       // b9 (Phase 51 DDGI 상수 — 메시 루트 27~30와 같은 레지스터)
		TerrainParam_DdgiIrradiance,      // t40 테이블
		TerrainParam_DdgiDistance,        // t41 테이블
		TerrainParam_DdgiProbeData,       // t42 테이블
	};

	constexpr uint32 MaxChunkCells = 64;
	constexpr uint64 RecreateBytes = 2ull * 1024 * 1024; // 바뀐 영역이 이보다 크면 업로드 대신 텍스처를 다시 만든다 (동기)
	constexpr uint64 UnusedFramesBeforeRelease = 120;

	// 머티리얼이 없는 레이어의 기본 색 (선형): 풀 / 흙 / 바위 / 눈
	const FVector4 DefaultLayerColors[4] = { FVector4(0.16f, 0.26f, 0.08f, 1.0f), FVector4(0.30f, 0.22f, 0.14f, 1.0f), FVector4(0.32f, 0.31f, 0.30f, 1.0f),
		                                     FVector4(0.85f, 0.87f, 0.92f, 1.0f) };

	const std::string& GetLayerAsset(const FTerrainComponent& Terrain, uint32 Layer)
	{
		switch (Layer)
		{
		case 1:  return Terrain.Layer1Material;
		case 2:  return Terrain.Layer2Material;
		case 3:  return Terrain.Layer3Material;
		default: return Terrain.Layer0Material;
		}
	}

	float GetLayerTiling(const FTerrainComponent& Terrain, uint32 Layer)
	{
		const float Tiles[4] = { Terrain.Layer0Tiling, Terrain.Layer1Tiling, Terrain.Layer2Tiling, Terrain.Layer3Tiling };
		return 1.0f / std::max(Tiles[Layer], 1.0f);
	}

	// 상자까지 거리 (안이면 0)
	float DistanceToBox(const FBox& Box, const FVector3& Point)
	{
		const float DX = std::max({ Box.Min.X - Point.X, 0.0f, Point.X - Box.Max.X });
		const float DY = std::max({ Box.Min.Y - Point.Y, 0.0f, Point.Y - Box.Max.Y });
		const float DZ = std::max({ Box.Min.Z - Point.Z, 0.0f, Point.Z - Box.Max.Z });
		return std::sqrt(DX * DX + DY * DY + DZ * DZ);
	}
} // namespace

FTerrainRenderer::~FTerrainRenderer()
{
	Shutdown();
}

uint32 FTerrainRenderer::ComputeChunkCells(uint32 Cells)
{
	for (uint32 Size = MaxChunkCells; Size >= 2; Size /= 2)
	{
		if (Cells % Size == 0)
		{
			return Size;
		}
	}
	return 0;
}

uint32 FTerrainRenderer::SelectChunkLod(float Distance, float ChunkWorldSize, float DistanceScale, uint32 MaxLod)
{
	const float Ratio = Distance / std::max(ChunkWorldSize * std::max(DistanceScale, 0.01f), 1.0f);
	if (Ratio < 1.0f)
	{
		return 0;
	}
	const uint32 Lod = static_cast<uint32>(std::floor(std::log2(Ratio))) + 1;
	return std::min(Lod, MaxLod);
}

bool FTerrainRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT InColorFormat,
                            DXGI_FORMAT InDepthFormat)
{
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	Resources     = &InResources;
	ColorFormat   = InColorFormat;
	DepthFormat   = InDepthFormat;

	using FRange = FD3D12RootSignature;
	const D3D12_SHADER_VISIBILITY Pixel  = D3D12_SHADER_VISIBILITY_PIXEL;
	const D3D12_SHADER_VISIBILITY Vertex = D3D12_SHADER_VISIBILITY_VERTEX;
	uint32 Index = 0;
	Index        = RootSignature.AddConstantBufferView(1);
	E_CHECK(Index == TerrainParam_PerFrame);
	RootSignature.AddConstantBufferView(3, 0, Pixel);
	RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8) }, Pixel);
	RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 5) }, Pixel);
	RootSignature.AddConstantBufferView(5, 0, Pixel);
	RootSignature.AddShaderResourceView(9, 0, Pixel);
	RootSignature.AddShaderResourceView(10, 0, Pixel);
	RootSignature.AddShaderResourceView(11, 0, Pixel);
	Index = RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 12) }, Pixel);
	E_CHECK(Index == TerrainParam_LocalShadowMap);
	RootSignature.AddConstantBufferView(0, 1);
	RootSignature.AddConstants(2, 1, 1, Vertex);
	RootSignature.AddConstants(16, 2, 1, Vertex);
	RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1) });
	RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 1) }, Pixel);
	Index = RootSignature.AddShaderResourceView(2, 1, Vertex);
	E_CHECK(Index == TerrainParam_Chunks);
	for (uint32 Layer = 0; Layer < 4; ++Layer)
	{
		Index = RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MaterialSlot_Count, 3 + Layer * MaterialSlot_Count, 1) }, Pixel);
	}
	E_CHECK(Index == TerrainParam_Layer3);
	// 화면 효과 (FSceneRenderer 메시 루트 파라미터 15~21과 같은 레지스터, 공간 0)
	const auto VolatileTable = [&](uint32 Register) {
		return RootSignature.AddDescriptorTable(
			{ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, Register, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, Pixel);
	};
	Index = VolatileTable(16);
	E_CHECK(Index == TerrainParam_AmbientOcclusion);
	VolatileTable(17);
	VolatileTable(18);
	Index = VolatileTable(19);
	E_CHECK(Index == TerrainParam_DBufferC);
	Index = RootSignature.AddShaderResourceView(20, 0, Pixel);
	E_CHECK(Index == TerrainParam_ReflectionCaptures);
	VolatileTable(21);
	Index = VolatileTable(22);
	E_CHECK(Index == TerrainParam_ScreenReflection);
	Index = VolatileTable(24);
	E_CHECK(Index == TerrainParam_RayTracedShadowMask);
	Index = RootSignature.AddDescriptorTable(
		{ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) }, Pixel);
	E_CHECK(Index == TerrainParam_LightTextures);
	Index = RootSignature.AddConstantBufferView(9, 0, Pixel);
	E_CHECK(Index == TerrainParam_DdgiConstants);
	VolatileTable(40);
	VolatileTable(41);
	Index = VolatileTable(42);
	E_CHECK(Index == TerrainParam_DdgiProbeData);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_ANISOTROPIC));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	D3D12_STATIC_SAMPLER_DESC ShadowSampler =
		FD3D12RootSignature::MakeStaticSampler(2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
	ShadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	ShadowSampler.BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	ShadowSampler.MaxAnisotropy  = 1;
	RootSignature.AddStaticSampler(ShadowSampler);
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"TerrainRootSignature"))
	{
		return false;
	}
	if (!CreatePipelines(MainPipeline, ShadowPipeline, LocalShadowPipeline, false) || !CreatePassPipelines(MainEqualPipeline, PrepassPipeline, false))
	{
		return false;
	}
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	bDebugLod                      = CommandLine.HasFlag(L"--terrain-lod-colors");
	if (const std::wstring Lod = CommandLine.GetValue(L"--terrain-force-lod"); !Lod.empty())
	{
		ForcedLod = std::stoi(Lod);
	}
	return true;
}

bool FTerrainRenderer::CreatePipelines(FD3D12PipelineState& OutMain, FD3D12PipelineState& OutShadow, FD3D12PipelineState& OutLocalShadow, bool bForceRecompile)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Terrain.hlsl";
	VertexDesc.EntryPoint = L"TerrainVS";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"TerrainPS";
	PixelDesc.Stage              = EShaderStage::Pixel;
	FShaderCompileDesc ShadowDesc = VertexDesc;
	ShadowDesc.EntryPoint         = L"TerrainShadowVS";
	if (bForceRecompile && (!ShaderLibrary->CookShader(VertexDesc) || !ShaderLibrary->CookShader(PixelDesc) || !ShaderLibrary->CookShader(ShadowDesc)))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary->GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = ShaderLibrary->GetShader(PixelDesc);
	const ComPtr<IDxcBlob> ShadowShader = ShaderLibrary->GetShader(ShadowDesc);
	if (!VertexShader || !PixelShader || !ShadowShader)
	{
		return false;
	}
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	FGraphicsPipelineDesc Main;
	Main.RootSignature          = RootSignature.Get();
	Main.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Main.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	Main.RenderTargetFormats[0] = ColorFormat;
	Main.DepthStencilFormat     = DepthFormat;
	Main.bDepthEnable           = true;
	Main.CullMode               = D3D12_CULL_MODE_NONE; // 스커트는 양면, 지형 아래는 보통 보이지 않는다
	if (!OutMain.InitGraphics(Device, Main, L"TerrainPipeline"))
	{
		return false;
	}

	// 그림자: ShadowRenderer/LocalLightRenderer 기본 바이어스와 같은 값 (설정 창에서 바꾼 값은 따라가지 않는다)
	FGraphicsPipelineDesc Shadow;
	Shadow.RootSignature        = RootSignature.Get();
	Shadow.VertexShader         = FD3D12ShaderCompiler::ToBytecode(ShadowShader.Get());
	Shadow.NumRenderTargets     = 0;
	Shadow.DepthStencilFormat   = DXGI_FORMAT_D32_FLOAT;
	Shadow.bDepthEnable         = true;
	Shadow.CullMode             = D3D12_CULL_MODE_NONE;
	Shadow.bDepthClip           = false;
	Shadow.DepthBias            = FShadowSettings{}.DepthBias;
	Shadow.SlopeScaledDepthBias = FShadowSettings{}.SlopeBias;
	if (!OutShadow.InitGraphics(Device, Shadow, L"TerrainShadowPipeline"))
	{
		return false;
	}
	Shadow.bDepthClip           = true;
	Shadow.DepthBias            = FLocalShadowSettings{}.DepthBias;
	Shadow.SlopeScaledDepthBias = FLocalShadowSettings{}.SlopeBias;
	return OutLocalShadow.InitGraphics(Device, Shadow, L"TerrainLocalShadowPipeline");
}

bool FTerrainRenderer::CreatePassPipelines(FD3D12PipelineState& OutMainEqual, FD3D12PipelineState& OutPrepass, bool bForceRecompile)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Terrain.hlsl";
	VertexDesc.EntryPoint = L"TerrainVS";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc MainDesc = VertexDesc;
	MainDesc.EntryPoint         = L"TerrainPS";
	MainDesc.Stage              = EShaderStage::Pixel;
	FShaderCompileDesc PrepassDesc = MainDesc;
	PrepassDesc.EntryPoint         = L"TerrainPrepassPS";
	if (bForceRecompile && !ShaderLibrary->CookShader(PrepassDesc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader  = ShaderLibrary->GetShader(VertexDesc); // 메인 패스와 같은 바이트코드 (깊이 EQUAL)
	const ComPtr<IDxcBlob> MainShader    = ShaderLibrary->GetShader(MainDesc);
	const ComPtr<IDxcBlob> PrepassShader = ShaderLibrary->GetShader(PrepassDesc);
	if (!VertexShader || !MainShader || !PrepassShader)
	{
		return false;
	}
	ID3D12Device*         Device = Rhi->GetDevice().GetDevice();
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature          = RootSignature.Get();
	Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(MainShader.Get());
	Desc.RenderTargetFormats[0] = ColorFormat;
	Desc.DepthStencilFormat     = DepthFormat;
	Desc.bDepthEnable           = true;
	Desc.bDepthWrite            = false;
	Desc.DepthFunc              = D3D12_COMPARISON_FUNC_EQUAL;
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	if (!OutMainEqual.InitGraphics(Device, Desc, L"TerrainDepthEqualPipeline"))
	{
		return false;
	}
	// 사전 패스: FSceneRenderer SceneNormal(R10G10B10A2) + SceneVelocity(R16G16_FLOAT) 포맷과 같아야 한다
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PrepassShader.Get());
	Desc.NumRenderTargets       = 2;
	Desc.RenderTargetFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
	Desc.RenderTargetFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
	Desc.bDepthWrite            = true;
	Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS;
	return OutPrepass.InitGraphics(Device, Desc, L"TerrainPrepassPipeline");
}

bool FTerrainRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewMain;
	FD3D12PipelineState NewShadow;
	FD3D12PipelineState NewLocalShadow;
	if (!CreatePipelines(NewMain, NewShadow, NewLocalShadow, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "지형 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	FD3D12PipelineState NewMainEqual;
	FD3D12PipelineState NewPrepass;
	if (CreatePassPipelines(NewMainEqual, NewPrepass, bForceRecompile))
	{
		MainEqualPipeline.Swap(NewMainEqual);
		PrepassPipeline.Swap(NewPrepass);
		Rhi->DeferRelease(NewMainEqual.Detach());
		Rhi->DeferRelease(NewPrepass.Detach());
	}
	MainPipeline.Swap(NewMain);
	ShadowPipeline.Swap(NewShadow);
	LocalShadowPipeline.Swap(NewLocalShadow);
	Rhi->DeferRelease(NewMain.Detach());
	Rhi->DeferRelease(NewShadow.Detach());
	Rhi->DeferRelease(NewLocalShadow.Detach());
	if (MaskPipeline.Get() != nullptr)
	{
		Rhi->DeferRelease(MaskPipeline.Detach()); // 다음 RenderMask에서 새 셰이더로
	}
	return true;
}

void FTerrainRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (auto& [Data, Gpu] : GpuData)
	{
		if (Gpu.Heights)
		{
			Gpu.Heights->ShutdownDeferred(*Rhi);
		}
		if (Gpu.Weights)
		{
			Gpu.Weights->ShutdownDeferred(*Rhi);
		}
	}
	GpuData.clear();
	for (auto& [Quads, Patch] : Patches)
	{
		Patch->IndexBuffer.ShutdownDeferred(*Rhi);
	}
	Patches.clear();
	LayerMaterials.clear();
	Frame.clear();
	MainPipeline.Shutdown();
	MainEqualPipeline.Shutdown();
	PrepassPipeline.Shutdown();
	ShadowPipeline.Shutdown();
	LocalShadowPipeline.Shutdown();
	MaskPipeline.Shutdown();
	RootSignature.Shutdown();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
	Resources     = nullptr;
}

FTerrainRenderer::FPatch* FTerrainRenderer::GetPatch(uint32 Quads)
{
	if (const auto Found = Patches.find(Quads); Found != Patches.end())
	{
		return Found->second.get();
	}
	// 정점 번호: 격자 (Q+1)², 스커트 4변 × (Q+1) (Terrain.hlsl TerrainVertexPosition과 같은 순서)
	const uint32         Row = Quads + 1;
	std::vector<uint16>  Indices;
	Indices.reserve(static_cast<size_t>(Quads) * Quads * 6 + static_cast<size_t>(Quads) * 24);
	auto Grid = [Row](uint32 X, uint32 Y) { return static_cast<uint16>(Y * Row + X); };
	for (uint32 Y = 0; Y < Quads; ++Y)
	{
		for (uint32 X = 0; X < Quads; ++X)
		{
			// 대각선 (0,0)-(1,1): TerrainMath::SampleHeight / Jolt 충돌과 같은 분할
			const uint16 A = Grid(X, Y), B = Grid(X + 1, Y), C = Grid(X, Y + 1), D = Grid(X + 1, Y + 1);
			Indices.insert(Indices.end(), { A, D, B, A, C, D });
		}
	}
	for (uint32 Edge = 0; Edge < 4; ++Edge)
	{
		for (uint32 T = 0; T < Quads; ++T)
		{
			auto Border = [&](uint32 Index) {
				switch (Edge)
				{
				case 0:  return Grid(Index, 0);
				case 1:  return Grid(Quads, Index);
				case 2:  return Grid(Index, Quads);
				default: return Grid(0, Index);
				}
			};
			const uint16 G0 = Border(T), G1 = Border(T + 1);
			const uint16 S0 = static_cast<uint16>(Row * Row + Edge * Row + T), S1 = static_cast<uint16>(S0 + 1);
			Indices.insert(Indices.end(), { G0, G1, S1, G0, S1, S0 });
		}
	}
	auto Patch        = std::make_unique<FPatch>();
	Patch->IndexCount = static_cast<uint32>(Indices.size());
	if (!Patch->IndexBuffer.InitStatic(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Indices.data(), Indices.size() * sizeof(uint16), L"TerrainPatchIndices"))
	{
		return nullptr;
	}
	FPatch* Result = Patch.get();
	Patches.emplace(Quads, std::move(Patch));
	return Result;
}

FTerrainRenderer::FTerrainGpu* FTerrainRenderer::EnsureGpu(const FTerrainData& Data)
{
	FTerrainGpu& Gpu = GpuData[&Data];
	Gpu.LastUsedFrame = FrameCounter;
	FTerrainRect Changed;
	const bool   bResized = Gpu.Resolution != Data.Resolution;
	const bool   bChanged = Data.GetChangesSince(Gpu.SeenCounter, Changed);
	if (!bResized && !bChanged && Gpu.Heights)
	{
		return &Gpu;
	}
	const uint64 ChangedBytes = static_cast<uint64>(Changed.GetWidth()) * Changed.GetHeight() * (sizeof(uint16) + sizeof(uint32));
	if (bResized || !Gpu.Heights || ChangedBytes > RecreateBytes)
	{
		// 전체 (다시) 만들기: 동기 업로드
		if (Gpu.Heights)
		{
			Gpu.Heights->ShutdownDeferred(*Rhi);
			Gpu.Weights->ShutdownDeferred(*Rhi);
		}
		Gpu.Heights = std::make_unique<FD3D12Texture>();
		Gpu.Weights = std::make_unique<FD3D12Texture>();
		const bool bOk =
			Gpu.Heights->Init2D(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Data.Resolution, Data.Resolution, DXGI_FORMAT_R16_UNORM,
			                    Data.Heights.data(), sizeof(uint16), L"TerrainHeights", false) &&
			Gpu.Weights->Init2D(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Data.Resolution, Data.Resolution,
			                    DXGI_FORMAT_R8G8B8A8_UNORM, Data.Weights.data(), sizeof(uint32), L"TerrainWeights", false);
		if (!bOk)
		{
			E_LOG(LogRenderer, Error, "지형 텍스처 생성 실패 ({}x{})", Data.Resolution, Data.Resolution);
			Gpu.Heights.reset();
			Gpu.Weights.reset();
			return nullptr;
		}
		Gpu.bShaderReadable = false;
		Gpu.Resolution      = Data.Resolution;
		Gpu.ChunkCells      = ComputeChunkCells(Data.Resolution - 1);
		Gpu.ChunksPerSide   = Gpu.ChunkCells > 0 ? (Data.Resolution - 1) / Gpu.ChunkCells : 0;
		Gpu.MaxLod          = 0;
		while (Gpu.ChunkCells > 0 && (Gpu.ChunkCells >> (Gpu.MaxLod + 1)) >= 2)
		{
			++Gpu.MaxLod;
		}
		Gpu.ChunkMinHeight.assign(static_cast<size_t>(Gpu.ChunksPerSide) * Gpu.ChunksPerSide, 0);
		Gpu.ChunkMaxHeight.assign(Gpu.ChunkMinHeight.size(), 0);
		UpdateChunkHeights(Gpu, Data, 0, 0, static_cast<int32>(Data.Resolution) - 1, static_cast<int32>(Data.Resolution) - 1);
	}
	else
	{
		UploadRegion(Gpu, Data, Changed.MinX, Changed.MinY, Changed.MaxX, Changed.MaxY);
		UpdateChunkHeights(Gpu, Data, Changed.MinX, Changed.MinY, Changed.MaxX, Changed.MaxY);
	}
	Gpu.SeenCounter = Data.ChangeCounter;
	return &Gpu;
}

void FTerrainRenderer::UploadRegion(FTerrainGpu& Gpu, const FTerrainData& Data, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
{
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	const uint32               Width         = static_cast<uint32>(MaxX - MinX + 1);
	const uint32               Height        = static_cast<uint32>(MaxY - MinY + 1);
	const D3D12_RESOURCE_STATES ReadState =
		Gpu.bShaderReadable ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	auto Upload = [&](FD3D12Texture& Texture, DXGI_FORMAT Format, uint32 BytesPerPixel, const uint8* Source) {
		const uint32                  RowBytes = Width * BytesPerPixel;
		const uint32                  Pitch    = AlignUp<uint32>(RowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		const FD3D12DynamicAllocation Staging  = DynamicBuffer.Allocate(static_cast<uint64>(Pitch) * Height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		uint8*                        Dest     = static_cast<uint8*>(Staging.CpuAddress);
		for (uint32 Row = 0; Row < Height; ++Row)
		{
			const size_t SourceOffset = (static_cast<size_t>(MinY + Row) * Data.Resolution + MinX) * BytesPerPixel;
			std::memcpy(Dest + static_cast<size_t>(Row) * Pitch, Source + SourceOffset, RowBytes);
		}
		const D3D12_RESOURCE_BARRIER ToCopy = MakeTransitionBarrier(Texture.GetResource(), ReadState, D3D12_RESOURCE_STATE_COPY_DEST);
		CommandList->ResourceBarrier(1, &ToCopy);

		D3D12_TEXTURE_COPY_LOCATION Target{};
		Target.pResource        = Texture.GetResource();
		Target.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		Target.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION SourceLocation{};
		SourceLocation.pResource                          = Staging.Resource;
		SourceLocation.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		SourceLocation.PlacedFootprint.Offset             = Staging.ResourceOffset;
		SourceLocation.PlacedFootprint.Footprint.Format   = Format;
		SourceLocation.PlacedFootprint.Footprint.Width    = Width;
		SourceLocation.PlacedFootprint.Footprint.Height   = Height;
		SourceLocation.PlacedFootprint.Footprint.Depth    = 1;
		SourceLocation.PlacedFootprint.Footprint.RowPitch = Pitch;
		CommandList->CopyTextureRegion(&Target, static_cast<UINT>(MinX), static_cast<UINT>(MinY), 0, &SourceLocation, nullptr);

		const D3D12_RESOURCE_BARRIER ToRead =
			MakeTransitionBarrier(Texture.GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		CommandList->ResourceBarrier(1, &ToRead);
	};
	Upload(*Gpu.Heights, DXGI_FORMAT_R16_UNORM, sizeof(uint16), reinterpret_cast<const uint8*>(Data.Heights.data()));
	Upload(*Gpu.Weights, DXGI_FORMAT_R8G8B8A8_UNORM, sizeof(uint32), reinterpret_cast<const uint8*>(Data.Weights.data()));
	Gpu.bShaderReadable = true;
}

void FTerrainRenderer::UpdateChunkHeights(FTerrainGpu& Gpu, const FTerrainData& Data, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
{
	if (Gpu.ChunkCells == 0)
	{
		return;
	}
	const int32 Cells = static_cast<int32>(Gpu.ChunkCells);
	// 경계 정점은 이웃 청크와 공유하므로 한 칸 넓혀 다시 계산
	const int32 FirstX = std::max(0, (MinX - 1) / Cells);
	const int32 FirstY = std::max(0, (MinY - 1) / Cells);
	const int32 LastX  = std::min(static_cast<int32>(Gpu.ChunksPerSide) - 1, MaxX / Cells);
	const int32 LastY  = std::min(static_cast<int32>(Gpu.ChunksPerSide) - 1, MaxY / Cells);
	for (int32 CY = FirstY; CY <= LastY; ++CY)
	{
		for (int32 CX = FirstX; CX <= LastX; ++CX)
		{
			uint16 Low  = 65535;
			uint16 High = 0;
			for (int32 Y = CY * Cells; Y <= (CY + 1) * Cells; ++Y)
			{
				for (int32 X = CX * Cells; X <= (CX + 1) * Cells; ++X)
				{
					const uint16 Value = Data.GetHeight(X, Y);
					Low                = std::min(Low, Value);
					High               = std::max(High, Value);
				}
			}
			const size_t Index        = static_cast<size_t>(CY) * Gpu.ChunksPerSide + CX;
			Gpu.ChunkMinHeight[Index] = Low;
			Gpu.ChunkMaxHeight[Index] = High;
		}
	}
}

void FTerrainRenderer::CollectResourceRoots(FResourceRoots& Roots) const
{
	for (const FMaterialHandle Handle : FrameLayerMaterials)
	{
		Roots.Add(Handle);
	}
}

FMaterialHandle FTerrainRenderer::ResolveLayerMaterial(const std::string& Asset)
{
	if (Asset.empty())
	{
		return FMaterialHandle{};
	}
	if (const auto Found = LayerMaterials.find(Asset); Found != LayerMaterials.end() && Resources->GetMaterial(Found->second) != nullptr)
	{
		return Found->second;
	}
	const FMaterialHandle Handle = Resources->LoadMaterial(FTerrainLibrary::Get().ResolveAssetPath(Asset));
	LayerMaterials[Asset]        = Handle; // 실패(무효 핸들)도 기억해 매 프레임 다시 읽지 않는다
	return Handle;
}

void FTerrainRenderer::Prepare(FScene& Scene, const FCamera& Camera, const FFrustum& Frustum)
{
	++FrameCounter;
	Frame.clear();
	FrameLayerMaterials.clear();
	DrawCalls = 0;
	Triangles = 0;
	VisibleChunks = 0;
	TotalChunks   = 0;
	ShadowDrawCalls = 0;
	ShadowTriangles = 0;
	if (!bEnabled || Rhi == nullptr)
	{
		return;
	}

	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(Scene, Terrains);
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	const FVector3             CameraPosition = Camera.GetPosition();
	for (const FTerrainInstance& Instance : Terrains)
	{
		FTerrainGpu* Gpu = EnsureGpu(*Instance.Data);
		if (Gpu == nullptr || Gpu->ChunkCells == 0)
		{
			continue;
		}
		if (!Gpu->bShaderReadable)
		{
			// 생성 직후(PIXEL_SHADER_RESOURCE) → 정점 셰이더도 읽게
			const D3D12_RESOURCE_BARRIER Barriers[2] = {
				MakeTransitionBarrier(Gpu->Heights->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				MakeTransitionBarrier(Gpu->Weights->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
			};
			CommandList->ResourceBarrier(2, Barriers);
			Gpu->bShaderReadable = true;
		}

		FFrameTerrain& Terrain = Frame.emplace_back();
		Terrain.Gpu            = Gpu;
		Terrain.Data           = Instance.Data;
		Terrain.Entity         = Instance.Entity;
		Terrain.Origin         = Instance.Frame.Origin;
		Terrain.CellSize       = Instance.Frame.CellSize;
		Terrain.HeightScale    = Instance.Frame.HeightScale;
		Terrain.bCastShadows   = Instance.Component->bCastShadows;

		FTerrainConstants Constants;
		Constants.Origin      = Instance.Frame.Origin;
		Constants.HeightScale = Instance.Frame.HeightScale;
		Constants.CellSize    = Instance.Frame.CellSize;
		Constants.Resolution  = static_cast<float>(Gpu->Resolution);
		Constants.SkirtDepth  = 1.5f * std::max(Instance.Frame.CellSize.X, Instance.Frame.CellSize.Y);
		Constants.DebugLod    = bDebugLod ? 1u : 0u;
		for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
		{
			const FMaterialHandle Handle   = ResolveLayerMaterial(GetLayerAsset(*Instance.Component, Layer));
			FrameLayerMaterials.push_back(Handle);
			const FMaterial&      Material = Resources->ResolveMaterial(Handle);
			Terrain.LayerTables[Layer]     = Material.TextureTable.Gpu;
			if (Layer == 0)
			{
				Terrain.Layer0Material = &Material;
				Terrain.Layer0Tiling   = GetLayerTiling(*Instance.Component, Layer);
				Terrain.Layer0Color    = Handle.IsValid() ? FVector4::OneVector : DefaultLayerColors[0];
			}
			Constants.LayerTiling[Layer]   = GetLayerTiling(*Instance.Component, Layer);
			if (Handle.IsValid())
			{
				Constants.LayerBaseColor[Layer] = Material.Constants.BaseColorFactor;
				Constants.LayerParams[Layer]    = FVector4(Material.Constants.Metallic, Material.Constants.Roughness, Material.Constants.NormalScale,
				                                           Material.Constants.OcclusionStrength);
			}
			else
			{
				Constants.LayerBaseColor[Layer] = DefaultLayerColors[Layer];
				Constants.LayerParams[Layer]    = FVector4(0.0f, 0.9f, 1.0f, 1.0f);
			}
		}
		Terrain.Constants = DynamicBuffer.AllocateConstants(Constants).GpuAddress;

		// 청크 경계 + LOD (메인 카메라 기준 — 그림자 패스도 같은 LOD를 써서 그림자와 본체가 어긋나지 않게)
		const uint32 ChunkCount = Gpu->ChunksPerSide * Gpu->ChunksPerSide;
		const float  ChunkWorld = static_cast<float>(Gpu->ChunkCells) * std::max(Instance.Frame.CellSize.X, Instance.Frame.CellSize.Y);
		Terrain.ChunkBounds.resize(ChunkCount);
		Terrain.ChunkLods.resize(ChunkCount);
		for (uint32 CY = 0; CY < Gpu->ChunksPerSide; ++CY)
		{
			for (uint32 CX = 0; CX < Gpu->ChunksPerSide; ++CX)
			{
				const uint32 Index = CY * Gpu->ChunksPerSide + CX;
				const float  X0    = static_cast<float>(CX * Gpu->ChunkCells);
				const float  Y0    = static_cast<float>(CY * Gpu->ChunkCells);
				const float  X1    = X0 + static_cast<float>(Gpu->ChunkCells);
				const float  Y1    = Y0 + static_cast<float>(Gpu->ChunkCells);
				Terrain.ChunkBounds[Index] = FBox(Instance.Frame.GridToWorld(X0, Y0, Gpu->ChunkMinHeight[Index]),
				                                  Instance.Frame.GridToWorld(X1, Y1, Gpu->ChunkMaxHeight[Index]));
				const uint32 Lod = ForcedLod >= 0 ? std::min(static_cast<uint32>(ForcedLod), Gpu->MaxLod)
				                                  : SelectChunkLod(DistanceToBox(Terrain.ChunkBounds[Index], CameraPosition), ChunkWorld, LodDistanceScale, Gpu->MaxLod);
				Terrain.ChunkLods[Index] = static_cast<uint8>(Lod);
			}
		}
		TotalChunks += ChunkCount;
		Terrain.MainChunks = BuildDraws(Terrain, Frustum, Terrain.MainDraws);
		for (const FFrameTerrain::FDraw& Draw : Terrain.MainDraws)
		{
			VisibleChunks += Draw.Count;
		}
	}

	// 오래 쓰지 않은 데이터의 GPU 텍스처 해제 (지형 삭제/다시 로드)
	for (auto It = GpuData.begin(); It != GpuData.end();)
	{
		if (FrameCounter - It->second.LastUsedFrame > UnusedFramesBeforeRelease)
		{
			if (It->second.Heights)
			{
				It->second.Heights->ShutdownDeferred(*Rhi);
				It->second.Weights->ShutdownDeferred(*Rhi);
			}
			It = GpuData.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FTerrainRenderer::GetRayTracingInputs(std::vector<FTerrainRayTracingInput>& OutInputs) const
{
	for (const FFrameTerrain& Terrain : Frame)
	{
		if (Terrain.Data == nullptr || Terrain.Gpu == nullptr)
		{
			continue;
		}
		FTerrainRayTracingInput& Input = OutInputs.emplace_back();
		Input.Data         = Terrain.Data;
		Input.Entity       = Terrain.Entity;
		Input.Origin       = Terrain.Origin;
		Input.CellSize     = Terrain.CellSize;
		Input.HeightScale  = Terrain.HeightScale;
		Input.bCastShadows = Terrain.bCastShadows;
		Input.Material     = Terrain.Layer0Material;
		Input.Tiling       = Terrain.Layer0Tiling;
		Input.Color        = Terrain.Layer0Color;
		Input.ChunkCells   = Terrain.Gpu->ChunkCells;
	}
}

D3D12_GPU_VIRTUAL_ADDRESS FTerrainRenderer::BuildDraws(const FFrameTerrain& Terrain, const FFrustum& Frustum, std::vector<FFrameTerrain::FDraw>& OutDraws)
{
	OutDraws.clear();
	ChunkScratch.clear();
	const FTerrainGpu& Gpu = *Terrain.Gpu;
	// LOD(패치 크기)별로 모은다
	for (uint32 Lod = 0; Lod <= Gpu.MaxLod; ++Lod)
	{
		const uint32 First = static_cast<uint32>(ChunkScratch.size());
		for (uint32 Index = 0; Index < static_cast<uint32>(Terrain.ChunkLods.size()); ++Index)
		{
			if (Terrain.ChunkLods[Index] != Lod || !Frustum.Intersects(Terrain.ChunkBounds[Index]))
			{
				continue;
			}
			FTerrainChunkGpu& Chunk = ChunkScratch.emplace_back();
			Chunk.X                 = (Index % Gpu.ChunksPerSide) * Gpu.ChunkCells;
			Chunk.Y                 = (Index / Gpu.ChunksPerSide) * Gpu.ChunkCells;
			Chunk.Step              = 1u << Lod;
			Chunk.Quads             = Gpu.ChunkCells >> Lod;
		}
		const uint32 Count = static_cast<uint32>(ChunkScratch.size()) - First;
		if (Count > 0)
		{
			OutDraws.push_back({ Gpu.ChunkCells >> Lod, First, Count });
		}
	}
	if (ChunkScratch.empty())
	{
		return 0;
	}
	const FD3D12DynamicAllocation Allocation =
		Rhi->GetDynamicBuffer().Allocate(sizeof(FTerrainChunkGpu) * ChunkScratch.size(), sizeof(FTerrainChunkGpu));
	std::memcpy(Allocation.CpuAddress, ChunkScratch.data(), sizeof(FTerrainChunkGpu) * ChunkScratch.size());
	return Allocation.GpuAddress;
}

void FTerrainRenderer::DrawPatches(ID3D12GraphicsCommandList* CommandList, const std::vector<FFrameTerrain::FDraw>& Draws, uint32& InOutDrawCalls,
                                   uint64& InOutTriangles)
{
	for (const FFrameTerrain::FDraw& Draw : Draws)
	{
		FPatch* Patch = GetPatch(Draw.Quads);
		if (Patch == nullptr)
		{
			continue;
		}
		const uint32 DrawConstants[2] = { Draw.First, Draw.Quads };
		CommandList->SetGraphicsRoot32BitConstants(TerrainParam_Draw, 2, DrawConstants, 0);
		const D3D12_INDEX_BUFFER_VIEW IndexView = Patch->IndexBuffer.GetIndexBufferView(DXGI_FORMAT_R16_UINT);
		CommandList->IASetIndexBuffer(&IndexView);
		CommandList->DrawIndexedInstanced(Patch->IndexCount, Draw.Count, 0, 0, 0);
		++InOutDrawCalls;
		InOutTriangles += static_cast<uint64>(Patch->IndexCount / 3) * Draw.Count;
	}
}

void FTerrainRenderer::RenderMain(ETerrainPass Pass, D3D12_GPU_VIRTUAL_ADDRESS PerFrame, D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants,
                                  const FShadowRenderer& Shadow, const FIblRenderer& Ibl, const FLocalLightRenderer& LocalLights,
                                  const FTerrainScreenInputs& Screen)
{
	DrawCalls = 0;
	Triangles = 0;
	if (Frame.empty())
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pass == ETerrainPass::Prepass ? PrepassPipeline.Get()
	                              : Pass == ETerrainPass::MainDepthEqual ? MainEqualPipeline.Get()
	                                                                     : MainPipeline.Get());
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_AmbientOcclusion, Screen.AmbientOcclusion.Gpu);
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		CommandList->SetGraphicsRootDescriptorTable(TerrainParam_DBufferA + Index, Screen.DBuffer[Index].Gpu);
	}
	CommandList->SetGraphicsRootShaderResourceView(TerrainParam_ReflectionCaptures, Screen.ReflectionCaptures);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_CaptureAtlas, Screen.CaptureAtlas.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_ScreenReflection, Screen.ScreenReflection.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_RayTracedShadowMask, Screen.RayTracedShadowMask.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_LightTextures, Rhi->GetSrvAllocator().GetHeap()->GetGPUDescriptorHandleForHeapStart());
	CommandList->SetGraphicsRootConstantBufferView(TerrainParam_DdgiConstants, Screen.DdgiConstants);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_DdgiIrradiance, Screen.DdgiIrradiance.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_DdgiDistance, Screen.DdgiDistance.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_DdgiProbeData, Screen.DdgiProbeData.Gpu);
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->SetGraphicsRootConstantBufferView(TerrainParam_PerFrame, PerFrame);
	CommandList->SetGraphicsRootConstantBufferView(TerrainParam_Shadow, ShadowConstants);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_ShadowMap, Shadow.GetShadowMapSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_Ibl, Ibl.GetLightingTable().Gpu);
	CommandList->SetGraphicsRootConstantBufferView(TerrainParam_Cluster, LocalLights.GetConstants());
	CommandList->SetGraphicsRootShaderResourceView(TerrainParam_LocalLights, LocalLights.GetLightList());
	CommandList->SetGraphicsRootShaderResourceView(TerrainParam_ClusterData, LocalLights.GetClusterData());
	CommandList->SetGraphicsRootShaderResourceView(TerrainParam_LocalShadowMatrices, LocalLights.GetShadowMatrices());
	CommandList->SetGraphicsRootDescriptorTable(TerrainParam_LocalShadowMap, LocalLights.GetShadowMapSrv().Gpu);
	for (const FFrameTerrain& Terrain : Frame)
	{
		if (Terrain.MainDraws.empty())
		{
			continue;
		}
		CommandList->SetGraphicsRootConstantBufferView(TerrainParam_Constants, Terrain.Constants);
		CommandList->SetGraphicsRootDescriptorTable(TerrainParam_Heights, Terrain.Gpu->Heights->GetSrv().Gpu);
		CommandList->SetGraphicsRootDescriptorTable(TerrainParam_Weights, Terrain.Gpu->Weights->GetSrv().Gpu);
		CommandList->SetGraphicsRootShaderResourceView(TerrainParam_Chunks, Terrain.MainChunks);
		for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
		{
			CommandList->SetGraphicsRootDescriptorTable(TerrainParam_Layer0 + Layer, Terrain.LayerTables[Layer]);
		}
		DrawPatches(CommandList, Terrain.MainDraws, DrawCalls, Triangles);
	}
}

void FTerrainRenderer::RenderShadow(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const FFrustum& Frustum, bool bLocalLight)
{
	bool bBound = false;
	std::vector<FFrameTerrain::FDraw> Draws;
	for (const FFrameTerrain& Terrain : Frame)
	{
		if (!Terrain.bCastShadows)
		{
			continue;
		}
		const D3D12_GPU_VIRTUAL_ADDRESS Chunks = BuildDraws(Terrain, Frustum, Draws);
		if (Draws.empty())
		{
			continue;
		}
		if (!bBound)
		{
			CommandList->SetGraphicsRootSignature(RootSignature.Get());
			CommandList->SetPipelineState(bLocalLight ? LocalShadowPipeline.Get() : ShadowPipeline.Get());
			CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CommandList->SetGraphicsRoot32BitConstants(TerrainParam_LightViewProjection, 16, &ViewProjection.M[0][0], 0);
			bBound = true;
		}
		CommandList->SetGraphicsRootConstantBufferView(TerrainParam_Constants, Terrain.Constants);
		CommandList->SetGraphicsRootDescriptorTable(TerrainParam_Heights, Terrain.Gpu->Heights->GetSrv().Gpu);
		CommandList->SetGraphicsRootShaderResourceView(TerrainParam_Chunks, Chunks);
		DrawPatches(CommandList, Draws, ShadowDrawCalls, ShadowTriangles);
	}
}

bool FTerrainRenderer::HasTerrain(const std::vector<FEntity>& Entities) const
{
	for (const FFrameTerrain& Terrain : Frame)
	{
		if (std::find(Entities.begin(), Entities.end(), Terrain.Entity) != Entities.end())
		{
			return true;
		}
	}
	return false;
}

void FTerrainRenderer::RenderMask(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const std::vector<FEntity>& Entities)
{
	if (!HasTerrain(Entities))
	{
		return;
	}
	if (MaskPipeline.Get() == nullptr)
	{
		FShaderCompileDesc VertexDesc;
		VertexDesc.FileName   = L"Terrain.hlsl";
		VertexDesc.EntryPoint = L"TerrainShadowVS";
		VertexDesc.Stage      = EShaderStage::Vertex;
		FShaderCompileDesc PixelDesc = VertexDesc;
		PixelDesc.EntryPoint         = L"TerrainMaskPS";
		PixelDesc.Stage              = EShaderStage::Pixel;
		const ComPtr<IDxcBlob> VertexShader = ShaderLibrary->GetShader(VertexDesc);
		const ComPtr<IDxcBlob> PixelShader  = ShaderLibrary->GetShader(PixelDesc);
		if (!VertexShader || !PixelShader)
		{
			return;
		}
		FGraphicsPipelineDesc Desc;
		Desc.RootSignature          = RootSignature.Get();
		Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
		Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
		Desc.RenderTargetFormats[0] = DXGI_FORMAT_R8_UNORM;
		Desc.CullMode               = D3D12_CULL_MODE_NONE;
		if (!MaskPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"TerrainMaskPipeline"))
		{
			return;
		}
	}
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(MaskPipeline.Get());
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->SetGraphicsRoot32BitConstants(TerrainParam_LightViewProjection, 16, &ViewProjection.M[0][0], 0);
	uint32 IgnoredDraws     = 0;
	uint64 IgnoredTriangles = 0;
	for (const FFrameTerrain& Terrain : Frame)
	{
		if (Terrain.MainDraws.empty() || std::find(Entities.begin(), Entities.end(), Terrain.Entity) == Entities.end())
		{
			continue;
		}
		CommandList->SetGraphicsRootConstantBufferView(TerrainParam_Constants, Terrain.Constants);
		CommandList->SetGraphicsRootDescriptorTable(TerrainParam_Heights, Terrain.Gpu->Heights->GetSrv().Gpu);
		CommandList->SetGraphicsRootShaderResourceView(TerrainParam_Chunks, Terrain.MainChunks);
		DrawPatches(CommandList, Terrain.MainDraws, IgnoredDraws, IgnoredTriangles);
	}
}
