#include "Renderer/SpriteShadowRenderer.h"

#include "Core/Jobs/ParallelFor.h"
#include "Core/Log.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/LocalLightRenderer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/ShadowCacheMath.h"
#include "Renderer/ShadowRenderer.h"
#include "Renderer/SpriteTiles.h"

#include <cstring>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum ESpriteShadowParam : uint32
	{
		SpriteShadowParam_Constants = 0, // b0: 광원 뷰-투영 16 + RunInfo 1
		SpriteShadowParam_Instances,     // t0
		SpriteShadowParam_Header,        // t1
		SpriteShadowParam_Textures,      // 공간 1 t0~ (셰이더 가시 힙 전체)
	};
	constexpr uint32 SpriteChunkRunBit  = 0x80000000u; // SpriteCommon.hlsli E_SPRITE_CHUNK_BIT
	constexpr uint32 SpriteDitherRunBit = 0x40000000u; // SpriteCommon.hlsli E_SPRITE_RUN_DITHER_BIT (청크 구간 전체 디더)

	// 반투명 그림자 디더 대상 (r.Sprite.TranslucentShadows): 알파·프리멀티플라이드 — Masked는 컷오프가 뜻, 가산은 빛이라 컷오프 그대로
	bool IsTranslucent(ESpriteBlendMode Blend)
	{
		return Blend == ESpriteBlendMode::Alpha || Blend == ESpriteBlendMode::Premultiplied;
	}

	// 정적 해시용: 색 RGB는 그림자에 영향이 없으므로 뺀다 (색만 바뀌는 정적 캐스터가 캐시를 매 프레임 다시 그리지 않게)
	uint64 HashInstance(uint64 Hash, FSpriteInstanceGpu Instance)
	{
		Instance.Color.X = Instance.Color.Y = Instance.Color.Z = 0.0f;
		return ShadowCacheMath::HashValue(Hash, Instance);
	}

	FBox ComputeInstanceBounds(const FSpriteInstanceGpu& Instance)
	{
		FBox Box;
		Box.AddPoint(Instance.Origin);
		Box.AddPoint(Instance.Origin + Instance.AxisX);
		Box.AddPoint(Instance.Origin + Instance.AxisZ);
		Box.AddPoint(Instance.Origin + Instance.AxisX + Instance.AxisZ);
		return Box;
	}
} // namespace

FSpriteShadowRenderer::~FSpriteShadowRenderer()
{
	Shutdown();
}

bool FSpriteShadowRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources)
{
	E_CHECKF(Rhi == nullptr, "스프라이트 그림자 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	Resources     = &InResources;
	DirectionalDepthBias = FShadowSettings{}.DepthBias;
	DirectionalSlopeBias = FShadowSettings{}.SlopeBias;
	LocalDepthBias       = FLocalShadowSettings{}.DepthBias;
	LocalSlopeBias       = FLocalShadowSettings{}.SlopeBias;

	using FRange = FD3D12RootSignature;
	E_CHECK(RootSignature.AddConstants(17, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX) == SpriteShadowParam_Constants);
	E_CHECK(RootSignature.AddShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX) == SpriteShadowParam_Instances);
	E_CHECK(RootSignature.AddShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_VERTEX) == SpriteShadowParam_Header);
	E_CHECK(RootSignature.AddDescriptorTable(
	            { FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) },
	            D3D12_SHADER_VISIBILITY_PIXEL) == SpriteShadowParam_Textures);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"SpriteShadowRootSignature"))
	{
		return false;
	}
	return CreatePipelines(DirectionalPipeline, LocalPipeline, false);
}

void FSpriteShadowRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	DirectionalPipeline.Shutdown();
	LocalPipeline.Shutdown();
	RootSignature.Shutdown();
	Runs.clear();
	StaticCasters.clear();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
	Resources     = nullptr;
}

bool FSpriteShadowRenderer::CreatePipelines(FD3D12PipelineState& OutDirectional, FD3D12PipelineState& OutLocal, bool bForceRecompile)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"SpriteShadow.hlsl";
	VertexDesc.EntryPoint = L"SpriteShadowVS";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"SpriteShadowPS";
	PixelDesc.Stage              = EShaderStage::Pixel;
	if (bForceRecompile && (!ShaderLibrary->CookShader(VertexDesc) || !ShaderLibrary->CookShader(PixelDesc)))
	{
		return false;
	}
	const ComPtr<IDxcBlob> Vertex = ShaderLibrary->GetShader(VertexDesc);
	const ComPtr<IDxcBlob> Pixel  = ShaderLibrary->GetShader(PixelDesc);
	if (!Vertex || !Pixel)
	{
		return false;
	}
	// 바이어스: 그림자 설정 값 (SetBias — 메시 그림자 PSO와 같은 값)
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature        = RootSignature.Get();
	Desc.VertexShader         = FD3D12ShaderCompiler::ToBytecode(Vertex.Get());
	Desc.PixelShader          = FD3D12ShaderCompiler::ToBytecode(Pixel.Get());
	Desc.NumRenderTargets     = 0;
	Desc.DepthStencilFormat   = DXGI_FORMAT_D32_FLOAT;
	Desc.bDepthEnable         = true;
	Desc.CullMode             = D3D12_CULL_MODE_NONE; // 양면
	Desc.bDepthClip           = false;                // 방향광: 캐스케이드 앞 캐스터를 근평면에 눌러 그린다
	Desc.DepthBias            = DirectionalDepthBias;
	Desc.SlopeScaledDepthBias = DirectionalSlopeBias;
	ID3D12Device* Device      = Rhi->GetDevice().GetDevice();
	if (!OutDirectional.InitGraphics(Device, Desc, L"SpriteShadowPipeline"))
	{
		return false;
	}
	Desc.bDepthClip           = true;
	Desc.DepthBias            = LocalDepthBias;
	Desc.SlopeScaledDepthBias = LocalSlopeBias;
	return OutLocal.InitGraphics(Device, Desc, L"SpriteLocalShadowPipeline");
}

bool FSpriteShadowRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewDirectional;
	FD3D12PipelineState NewLocal;
	if (!CreatePipelines(NewDirectional, NewLocal, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "스프라이트 그림자 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	DirectionalPipeline.Swap(NewDirectional);
	LocalPipeline.Swap(NewLocal);
	Rhi->DeferRelease(NewDirectional.Detach());
	Rhi->DeferRelease(NewLocal.Detach());
	return true;
}

void FSpriteShadowRenderer::SetBias(int32 InDirectionalDepthBias, float InDirectionalSlopeBias, int32 InLocalDepthBias, float InLocalSlopeBias)
{
	if (InDirectionalDepthBias == DirectionalDepthBias && InDirectionalSlopeBias == DirectionalSlopeBias && InLocalDepthBias == LocalDepthBias &&
	    InLocalSlopeBias == LocalSlopeBias)
	{
		return;
	}
	DirectionalDepthBias = InDirectionalDepthBias;
	DirectionalSlopeBias = InDirectionalSlopeBias;
	LocalDepthBias       = InLocalDepthBias;
	LocalSlopeBias       = InLocalSlopeBias;
	ReloadShaders(false); // 실패하면 기존 PSO 유지 (오류 로그)
}

void FSpriteShadowRenderer::Prepare(std::span<const FSpriteDrawItem> Items, std::span<const FSpriteChunkDraw> Chunks, bool bTranslucentDither)
{
	Runs.clear();
	StaticCasters.clear();
	StaticItemSum    = 0;
	StaticItemCount  = 0;
	StaticItemBounds = FBox();
	CasterCount = 0;
	StaticCount = 0;
	if (Rhi == nullptr || (Items.empty() && Chunks.empty()))
	{
		return;
	}
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	// 항목: [정적 | 동적] 순서로 인스턴스 (월드 공간 — SpriteRenderer::Prepare와 같은 식).
	// 1) 병렬 (자기 칸만): 제출 순서 칸에 인스턴스 + 정적이면 해시. ResolveTexture는 읽기만
	const uint64 MaxCount = DynamicBuffer.GetMaxAllocation() / sizeof(FSpriteInstanceGpu);
	const size_t Count    = std::min(Items.size(), static_cast<size_t>(MaxCount));
	Scratch.resize(Count);
	ScratchHashes.resize(Count);
	FParallel::ParallelFor(static_cast<uint32>(Count), 1024, [&](uint32 Begin, uint32 End) {
		FTextureHandle LastTexture;
		uint32         LastTextureIndex = Resources->ResolveTexture(LastTexture).GetSrv().Index;
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			const FSpriteDrawItem& Item = Items[Index];
			if (Item.Texture != LastTexture)
			{
				LastTexture      = Item.Texture;
				LastTextureIndex = Resources->ResolveTexture(Item.Texture).GetSrv().Index;
			}
			const SpriteMath::FQuad Quad     = SpriteMath::ComputeQuad(Item);
			FSpriteInstanceGpu&     Instance = Scratch[Index];
			Instance.Origin       = Quad.Origin;
			Instance.TextureIndex = LastTextureIndex;
			Instance.AxisX        = Quad.AxisX;
			Instance.Flags        = (Item.Filter == ESpriteFilter::Point ? SpriteTiles::FlagPoint : 0u) |
			                 (bTranslucentDither && IsTranslucent(Item.Blend) ? SpriteTiles::FlagShadowDither : 0u);
			Instance.AxisZ        = Quad.AxisZ;
			Instance.AlphaCutoff  = Item.AlphaCutoff;
			Instance.UVRect       = FVector4(Item.UVMin.X, Item.UVMin.Y, Item.UVMax.X, Item.UVMax.Y);
			Instance.Color        = Item.Color;
			ScratchHashes[Index]  = Item.bShadowStatic ? ShadowCacheMath::Finalize(HashInstance(ShadowCacheMath::HashSeed, Instance)) : 0ull;
		}
	});
	// 2) 순차: 업로드 버퍼(쓰기 결합)에 정적 → 동적 순서로 + 구간 경계·정적 해시 합 (합은 순서 무관)
	size_t StaticItems = 0;
	if (Count > 0)
	{
		const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FSpriteInstanceGpu) * Count, 16);
		FSpriteInstanceGpu*           Gpu        = static_cast<FSpriteInstanceGpu*>(Allocation.CpuAddress);
		size_t                        Written    = 0;
		for (uint32 Pass = 0; Pass < 2; ++Pass)
		{
			const bool   bStaticPass = Pass == 0;
			const size_t First       = Written;
			FBox         Bounds;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				if (Items[Index].bShadowStatic != bStaticPass)
				{
					continue;
				}
				const FSpriteInstanceGpu& Instance = Scratch[Index];
				std::memcpy(&Gpu[Written++], &Instance, sizeof(FSpriteInstanceGpu));
				Bounds.AddBox(ComputeInstanceBounds(Instance));
				StaticItemSum += ScratchHashes[Index];
			}
			if (bStaticPass)
			{
				StaticItems      = Written;
				StaticItemCount  = static_cast<uint32>(Written);
				StaticItemBounds = Bounds;
			}
			if (Written > First)
			{
				// 구간 주소 = 구간 시작 (구조화 버퍼 루트 SRV — 셰이더는 SV_InstanceID만)
				FRun Run;
				Run.Instances = Allocation.GpuAddress + sizeof(FSpriteInstanceGpu) * First;
				Run.Header    = Run.Instances;
				Run.Count     = static_cast<uint32>(Written - First);
				Run.Bounds    = Bounds;
				Run.bStatic   = bStaticPass;
				Runs.push_back(Run);
			}
		}
	}
	CasterCount = static_cast<uint32>(Count);
	StaticCount = static_cast<uint32>(StaticItems);

	// 청크: 청크마다 구간 하나 (머리 = 월드 행렬 0/2/3행 + 텍스처 칸 + 컷오프 + 색)
	for (const FSpriteChunkDraw& Chunk : Chunks)
	{
		FSpriteChunkGpu Header;
		Header.AxisX        = FVector3(Chunk.World.M[0][0], Chunk.World.M[0][1], Chunk.World.M[0][2]);
		Header.TextureIndex = Resources->ResolveTexture(Chunk.Texture).GetSrv().Index;
		Header.AxisZ        = FVector3(Chunk.World.M[2][0], Chunk.World.M[2][1], Chunk.World.M[2][2]);
		Header.AlphaCutoff  = Chunk.AlphaCutoff;
		Header.Translation  = FVector3(Chunk.World.M[3][0], Chunk.World.M[3][1], Chunk.World.M[3][2]);
		Header.Color        = Chunk.Color;
		const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FSpriteChunkGpu), 16);
		std::memcpy(Allocation.CpuAddress, &Header, sizeof(Header));
		FRun Run;
		Run.Instances = Chunk.Instances;
		Run.Header    = Allocation.GpuAddress;
		Run.Count     = Chunk.Count;
		Run.RunInfo   = SpriteChunkRunBit | (bTranslucentDither && IsTranslucent(Chunk.Blend) ? SpriteDitherRunBit : 0u);
		Run.Bounds    = Chunk.Bounds;
		Run.bStatic   = Chunk.bShadowStatic;
		Runs.push_back(Run);
		CasterCount += Chunk.Count;
		if (Chunk.bShadowStatic)
		{
			// 청크 내용은 정적 버퍼 주소·개수(다시 만들면 새 버퍼)로, 나머지는 머리 (색 RGB 제외)
			using namespace ShadowCacheMath;
			Header.Color.X = Header.Color.Y = Header.Color.Z = 0.0f;
			uint64 Hash = HashSeed;
			Hash        = HashValue(Hash, Chunk.Instances);
			Hash        = HashValue(Hash, Chunk.Count);
			Hash        = HashValue(Hash, Header);
			Hash        = HashValue(Hash, Run.RunInfo); // 디더 (r.Sprite.TranslucentShadows를 바꾸면 캐시를 다시 그린다)
			StaticCasters.push_back({ Chunk.Bounds, Finalize(Hash) });
			StaticCount += Chunk.Count;
		}
	}
}

bool FSpriteShadowRenderer::GetDynamicCasterBounds(const FFrustum& Frustum, FBox& OutBounds) const
{
	bool bAny = false;
	OutBounds = FBox();
	for (const FRun& Run : Runs)
	{
		if (!Run.bStatic && Frustum.Intersects(Run.Bounds))
		{
			OutBounds.AddBox(Run.Bounds);
			bAny = true;
		}
	}
	return bAny;
}

uint64 FSpriteShadowRenderer::GetStaticStateHash(const FFrustum& Frustum) const
{
	using namespace ShadowCacheMath;
	// 순서 무관 (합) — 수집 순서가 바뀌어도 같은 집합이면 같은 값 (메시 정적 집합 해시와 같은 방식)
	uint64 Sum   = 0;
	uint32 Count = 0;
	if (StaticItemCount > 0 && Frustum.Intersects(StaticItemBounds))
	{
		Sum += StaticItemSum;
		Count += StaticItemCount;
	}
	for (const FStaticCaster& Caster : StaticCasters)
	{
		if (Frustum.Intersects(Caster.Bounds))
		{
			Sum += Caster.Hash;
			++Count;
		}
	}
	return Count == 0 ? 0ull : Finalize(HashValue(HashValue(HashSeed, Sum), Count)) | 1ull;
}

void FSpriteShadowRenderer::RenderShadow(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const FFrustum& Frustum, bool bLocalLight,
                                         ESet Set) const
{
	bool bBound = false;
	for (const FRun& Run : Runs)
	{
		if ((Set == ESet::Static && !Run.bStatic) || (Set == ESet::Dynamic && Run.bStatic) || !Frustum.Intersects(Run.Bounds))
		{
			continue;
		}
		if (!bBound)
		{
			CommandList->SetGraphicsRootSignature(RootSignature.Get());
			CommandList->SetPipelineState(bLocalLight ? LocalPipeline.Get() : DirectionalPipeline.Get());
			CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CommandList->SetGraphicsRoot32BitConstants(SpriteShadowParam_Constants, 16, &ViewProjection.M[0][0], 0);
			CommandList->SetGraphicsRootDescriptorTable(SpriteShadowParam_Textures, Rhi->GetSrvAllocator().GetHeap()->GetGPUDescriptorHandleForHeapStart());
			bBound = true;
		}
		CommandList->SetGraphicsRoot32BitConstant(SpriteShadowParam_Constants, Run.RunInfo, 16);
		CommandList->SetGraphicsRootShaderResourceView(SpriteShadowParam_Instances, Run.Instances);
		CommandList->SetGraphicsRootShaderResourceView(SpriteShadowParam_Header, Run.Header);
		CommandList->DrawInstanced(6, Run.Count, 0, 0);
	}
}
