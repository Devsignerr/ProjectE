#include "Renderer/DecalRenderer.h"

#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/DecalMath.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/ScreenPass.h"
#include "Scene/Scene.h"

#include <algorithm>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// Decal.hlsl DecalConstants와 1:1
	struct alignas(16) FDecalConstants
	{
		FMatrix4x4 DecalToWorld;
		FMatrix4x4 WorldToDecal;
		FMatrix4x4 ViewProjection;
		FMatrix4x4 InvViewProjection;
		FVector4   BaseColorFactor;
		FVector3   DecalNormal;
		float      Opacity = 1.0f;
		FVector3   DecalTangent;
		float      RoughnessFactor = 1.0f;
		FVector3   DecalBitangent;
		float      MetallicFactor = 0.0f;
		FVector3   CameraPosition;
		float      NormalScale = 1.0f;
		FVector2   ScreenSize;
		float      FadeStartDistance = 0.0f;
		float      FadeEndDistance   = 0.0f;
		uint32     AffectFlags       = 7;
		float      FootprintScale    = 0.0f;
		uint32     bOrthographic     = 0;
		float      TexelsPerUnit     = 1.0f;
	};
	static_assert(sizeof(FDecalConstants) == 368);

	std::filesystem::path ResolveDecalPath(const std::string& AssetPath)
	{
		const std::filesystem::path Path = FStringConv::ToWide(AssetPath);
		if (Path.is_absolute())
		{
			return Path;
		}
		return (FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory()) / Path;
	}
} // namespace

FDecalRenderer::~FDecalRenderer()
{
	Shutdown();
}

bool FDecalRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InRoot)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	Root    = &InRoot;
	return CreatePipeline(Pipeline, false);
}

void FDecalRenderer::Shutdown()
{
	for (std::unique_ptr<FD3D12RenderTarget>& Target : Targets)
	{
		Target.reset();
	}
	Pipeline.Shutdown();
	Rhi = nullptr;
}

bool FDecalRenderer::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Decal.hlsl";
	VertexDesc.EntryPoint = L"VSMain";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"PSMain";
	PixelDesc.Stage              = EShaderStage::Pixel;
	if (bForceRecompile && (!Library->CookShader(VertexDesc) || !Library->CookShader(PixelDesc)))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader = Library->GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = Library->GetShader(PixelDesc);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}

	FGraphicsPipelineDesc Desc;
	Desc.RootSignature          = Root->Get();
	Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	Desc.NumRenderTargets       = 3;
	Desc.RenderTargetFormats[0] = FormatA;
	Desc.RenderTargetFormats[1] = FormatB;
	Desc.RenderTargetFormats[2] = FormatC;
	Desc.CullMode               = D3D12_CULL_MODE_FRONT; // 뒷면만: 카메라가 상자 안이어도 덮인다
	Desc.bDepthEnable           = false;
	Desc.BlendMode              = EBlendMode::Remaining;
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"DecalPipeline");
}

bool FDecalRenderer::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "데칼 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	Pipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

void FDecalRenderer::EnsureTargets(uint32 Width, uint32 Height)
{
	if (Targets[0] && Targets[0]->GetWidth() == Width && Targets[0]->GetHeight() == Height)
	{
		return;
	}
	const DXGI_FORMAT    Formats[3] = { FormatA, FormatB, FormatC };
	const wchar_t* const Names[3]   = { L"DBufferA", L"DBufferB", L"DBufferC" };
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		if (Targets[Index])
		{
			Targets[Index]->ShutdownDeferred(*Rhi);
		}
		FRenderTargetDesc Desc = FRenderTargetDesc::MakeColor(Formats[Index]);
		Desc.ClearColor[3]     = 1.0f; // 남은 표면 비중 1 = 데칼 없음
		Targets[Index]         = std::make_unique<FD3D12RenderTarget>();
		if (!Targets[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, Names[Index], Desc))
		{
			E_LOG(LogRenderer, Fatal, "DBuffer 생성 실패 ({}x{})", Width, Height);
		}
	}
}

bool FDecalRenderer::Prepare(FScene& Scene, FResourceManager& Resources, const FCamera& Camera, const FFrustum& Frustum, uint32 Width, uint32 Height)
{
	DrawnCount = 0;
	Prepared.clear();
	FRegistry& Registry = Scene.GetRegistry();

	// 수집: 머티리얼 해석 (경로가 바뀌면 다시) + 상자 컬링
	struct FCandidate
	{
		const FDecalComponent* Decal = nullptr;
		FMatrix4x4             DecalToWorld;
	};
	std::vector<FCandidate> Candidates;
	Visible.clear();
	Registry.View<FTransformComponent, FDecalComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FDecalComponent& Decal) {
		if (Decal.ResolvedMaterialAsset != Decal.MaterialAsset || (!Decal.Material.IsValid() && !Decal.MaterialAsset.empty()))
		{
			Decal.Material              = Decal.MaterialAsset.empty() ? FMaterialHandle{} : Resources.LoadMaterial(ResolveDecalPath(Decal.MaterialAsset));
			Decal.ResolvedMaterialAsset = Decal.MaterialAsset;
		}
		if (Decal.Opacity <= 0.0f || (!Decal.bAffectBaseColor && !Decal.bAffectNormal && !Decal.bAffectRoughness))
		{
			return;
		}
		const FMatrix4x4 DecalToWorld = FDecalMath::MakeDecalToWorld(Transform.WorldMatrix, Decal.Size);
		const FBox       Bounds       = FBox(FVector3(-0.5f), FVector3(0.5f)).TransformBy(DecalToWorld);
		if (!Frustum.Intersects(Bounds))
		{
			return;
		}
		Visible.push_back({ Decal.SortOrder, Entity.Index, static_cast<uint32>(Candidates.size()) });
		Candidates.push_back({ &Decal, DecalToWorld });
	});
	if (Visible.empty())
	{
		return false;
	}
	std::sort(Visible.begin(), Visible.end(), [](const FVisibleDecal& A, const FVisibleDecal& B) {
		return A.SortOrder != B.SortOrder ? A.SortOrder < B.SortOrder : A.Entity < B.Entity;
	});

	EnsureTargets(Width, Height);

	const FMatrix4x4 ViewProjection    = Camera.GetViewProjectionMatrix();
	const FMatrix4x4 InvViewProjection = ViewProjection.GetInverse();
	const float      FootprintScale    = Camera.IsOrthographic()
	                                         ? Camera.GetOrthoHeight() / static_cast<float>(Height)
	                                         : 2.0f * FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f) / static_cast<float>(Height);

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	for (const FVisibleDecal& Item : Visible)
	{
		const FCandidate&      Candidate = Candidates[Item.Index];
		const FDecalComponent& Decal     = *Candidate.Decal;
		const FMaterial&       Material  = Resources.ResolveMaterial(Decal.Material);

		FDecalConstants Constants;
		Constants.DecalToWorld      = Candidate.DecalToWorld;
		Constants.WorldToDecal      = Candidate.DecalToWorld.GetInverse();
		Constants.ViewProjection    = ViewProjection;
		Constants.InvViewProjection = InvViewProjection;
		Constants.BaseColorFactor   = Material.Constants.BaseColorFactor;
		const FMatrix4x4& M         = Candidate.DecalToWorld;
		Constants.DecalNormal       = FVector3(M.M[2][0], M.M[2][1], M.M[2][2]).GetNormalized(); // 로컬 +Z
		Constants.DecalTangent      = FVector3(M.M[1][0], M.M[1][1], M.M[1][2]).GetNormalized(); // 로컬 +Y = +U
		Constants.DecalBitangent    = FVector3(M.M[0][0], M.M[0][1], M.M[0][2]).GetNormalized(); // 로컬 +X = 텍스처 위
		Constants.Opacity           = FMath::Clamp(Decal.Opacity, 0.0f, 1.0f);
		Constants.RoughnessFactor   = Material.Constants.Roughness;
		Constants.MetallicFactor    = Material.Constants.Metallic;
		Constants.NormalScale       = Material.Constants.NormalScale;
		Constants.CameraPosition    = Camera.GetPosition();
		Constants.ScreenSize        = FVector2(static_cast<float>(Width), static_cast<float>(Height));
		Constants.FadeStartDistance = Decal.FadeStartDistance;
		Constants.FadeEndDistance   = Decal.FadeEndDistance;
		Constants.AffectFlags       = (Decal.bAffectBaseColor ? 1u : 0u) | (Decal.bAffectNormal ? 2u : 0u) | (Decal.bAffectRoughness ? 4u : 0u);
		Constants.FootprintScale    = FootprintScale;
		Constants.bOrthographic     = Camera.IsOrthographic() ? 1u : 0u;
		const FD3D12Texture* Base   = Resources.GetTexture(Material.Textures[MaterialSlot_BaseColor]);
		const float TextureWidth    = Base != nullptr ? static_cast<float>(Base->GetHeight()) : 1.0f; // 텍스처 위 = 로컬 X
		Constants.TexelsPerUnit     = TextureWidth / FMath::Max(Decal.Size.X, 1.0f);

		Prepared.push_back({ DynamicBuffer.AllocateConstants(Constants).GpuAddress, Material.TextureTable.Gpu });
	}
	DrawnCount = static_cast<uint32>(Prepared.size());
	return true;
}

std::array<FRGResourceRef, 3> FDecalRenderer::ImportTargets(FRenderGraph& Graph) const
{
	static constexpr const char* Names[3] = { "DBufferA", "DBufferB", "DBufferC" };
	std::array<FRGResourceRef, 3> Refs;
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		Refs[Index] = Graph.ImportColor(Names[Index], *Targets[Index]);
	}
	return Refs;
}

void FDecalRenderer::AddPass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneDepth, const FD3D12RenderTarget& SceneNormal, FRGResourceRef Depth,
                             FRGResourceRef Normal, int32 Timer, std::array<FRGResourceRef, 3>& OutTargets)
{
	OutTargets = ImportTargets(Graph);
	const uint32                 Width     = SceneDepth.GetWidth();
	const uint32                 Height    = SceneDepth.GetHeight();
	const FD3D12DescriptorHandle DepthSrv  = SceneDepth.GetDepthSrv();
	const FD3D12DescriptorHandle NormalSrv = SceneNormal.GetSrv();
	FRenderGraph::FPassBuilder   Pass      = Graph.AddPass("데칼 DBuffer");
	Pass.Read(Depth, ERGAccess::SrvPixel).Read(Normal, ERGAccess::SrvPixel).Timer(Timer);
	for (const FRGResourceRef& Target : OutTargets)
	{
		Pass.Write(Target, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true); // 지우고 그린다
	}
	Pass.Execute([this, Width, Height, DepthSrv, NormalSrv](FRGContext& Context) {
		ID3D12GraphicsCommandList* CommandList = Context.CommandList;
		const D3D12_CPU_DESCRIPTOR_HANDLE Rtvs[3] = { Targets[0]->GetRtv(), Targets[1]->GetRtv(), Targets[2]->GetRtv() };
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			CommandList->ClearRenderTargetView(Rtvs[Index], Targets[Index]->GetDesc().ClearColor, 0, nullptr);
		}
		CommandList->OMSetRenderTargets(3, Rtvs, FALSE, nullptr);
		SetScreenPassViewport(CommandList, Width, Height);
		CommandList->SetGraphicsRootSignature(Root->Get());
		CommandList->SetPipelineState(Pipeline.Get());
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		CommandList->SetGraphicsRootDescriptorTable(FScreenPassRootSignature::Root_Srv0 + 0, DepthSrv.Gpu);
		CommandList->SetGraphicsRootDescriptorTable(FScreenPassRootSignature::Root_Srv0 + 1, NormalSrv.Gpu);
		const uint32 Increment = Rhi->GetSrvAllocator().GetIncrementSize();
		for (const FPreparedDecal& Decal : Prepared)
		{
			CommandList->SetGraphicsRootConstantBufferView(FScreenPassRootSignature::Root_Constants, Decal.Constants);
			// 머티리얼 테이블(연속 5칸)의 베이스/금속거칠기/노멀 칸을 t2~t4로
			for (uint32 Slot = 0; Slot < 3; ++Slot)
			{
				D3D12_GPU_DESCRIPTOR_HANDLE Handle = Decal.TextureTable;
				Handle.ptr += static_cast<UINT64>(Slot) * Increment;
				CommandList->SetGraphicsRootDescriptorTable(FScreenPassRootSignature::Root_Srv0 + 2 + Slot, Handle);
			}
			CommandList->DrawInstanced(36, 1, 0, 0);
		}
	});
}
