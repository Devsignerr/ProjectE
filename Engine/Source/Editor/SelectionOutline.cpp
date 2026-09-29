#include "Editor/SelectionOutline.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	struct FCompositeConstants
	{
		FVector4 OutlineColor;
		FVector4 FillColor;
		int32    Thickness = 2;
		int32    Padding[3] = {};
	};
	static_assert(sizeof(FCompositeConstants) == 48);

	constexpr uint32 MaskRoot_Matrix       = 0; // b0 (루트 상수 16개)
	constexpr uint32 CompositeRoot_Consts  = 0; // b0 (루트 상수 12개)
	constexpr uint32 CompositeRoot_Mask    = 1; // t0

	FShaderCompileDesc MakeDesc(const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"Outline.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}
} // namespace

FSelectionOutline::~FSelectionOutline()
{
	Shutdown();
}

bool FSelectionOutline::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "선택 아웃라인이 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	MaskRootSignature.AddConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	if (!MaskRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"OutlineMaskRootSignature"))
	{
		return false;
	}

	CompositeRootSignature.AddConstants(sizeof(FCompositeConstants) / 4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	CompositeRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) },
	                                          D3D12_SHADER_VISIBILITY_PIXEL);
	if (!CompositeRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"OutlineCompositeRootSignature"))
	{
		return false;
	}

	CompositeFormat = FD3D12RHI::RenderTargetFormat;
	return CreatePipelines(MaskPipeline, CompositePipeline, CompositeFormat, false);
}

void FSelectionOutline::Shutdown()
{
	Mask.reset();
	MaskPipeline.Shutdown();
	CompositePipeline.Shutdown();
	MaskRootSignature.Shutdown();
	CompositeRootSignature.Shutdown();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FSelectionOutline::CreatePipelines(FD3D12PipelineState& OutMask, FD3D12PipelineState& OutComposite, DXGI_FORMAT OutputFormat,
                                        bool bForceRecompile)
{
	const FShaderCompileDesc Descs[] = {
		MakeDesc(L"MaskVS", EShaderStage::Vertex), MakeDesc(L"MaskPS", EShaderStage::Pixel),
		MakeDesc(L"CompositeVS", EShaderStage::Vertex), MakeDesc(L"CompositePS", EShaderStage::Pixel),
	};
	if (bForceRecompile)
	{
		for (const FShaderCompileDesc& Desc : Descs)
		{
			if (!ShaderLibrary->CookShader(Desc))
			{
				return false;
			}
		}
	}

	ComPtr<IDxcBlob> Blobs[4];
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Blobs[Index] = ShaderLibrary->GetShader(Descs[Index]);
		if (!Blobs[Index])
		{
			return false;
		}
	}

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	FGraphicsPipelineDesc MaskDesc;
	MaskDesc.RootSignature          = MaskRootSignature.Get();
	MaskDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(Blobs[0].Get());
	MaskDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(Blobs[1].Get());
	MaskDesc.InputLayout            = FStaticMesh::GetInputLayout(); // POSITION만 사용
	MaskDesc.RenderTargetFormats[0] = DXGI_FORMAT_R8_UNORM;
	MaskDesc.CullMode               = D3D12_CULL_MODE_NONE;
	MaskDesc.bDepthEnable           = false; // 가려진 부분까지 실루엣 전체를 표시
	if (!OutMask.InitGraphics(Device, MaskDesc, L"OutlineMaskPipeline"))
	{
		return false;
	}

	FGraphicsPipelineDesc CompositeDesc;
	CompositeDesc.RootSignature          = CompositeRootSignature.Get();
	CompositeDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(Blobs[2].Get());
	CompositeDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(Blobs[3].Get());
	CompositeDesc.RenderTargetFormats[0] = OutputFormat;
	CompositeDesc.CullMode               = D3D12_CULL_MODE_NONE;
	CompositeDesc.bDepthEnable           = false;
	CompositeDesc.BlendMode              = EBlendMode::Alpha;
	return OutComposite.InitGraphics(Device, CompositeDesc, L"OutlineCompositePipeline");
}

bool FSelectionOutline::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewMask;
	FD3D12PipelineState NewComposite;
	if (!CreatePipelines(NewMask, NewComposite, CompositeFormat, bForceRecompile))
	{
		E_LOG(LogEditor, Error, "아웃라인 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	MaskPipeline.Swap(NewMask);
	CompositePipeline.Swap(NewComposite);
	Rhi->DeferRelease(NewMask.Detach());
	Rhi->DeferRelease(NewComposite.Detach());
	return true;
}

void FSelectionOutline::EnsureMask(uint32 Width, uint32 Height)
{
	if (Mask && Mask->GetWidth() == Width && Mask->GetHeight() == Height)
	{
		return;
	}
	if (Mask)
	{
		Mask->ShutdownDeferred(*Rhi);
	}
	Mask = std::make_unique<FD3D12RenderTarget>();
	if (!Mask->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, L"SelectionMask", FRenderTargetDesc::MakeMask(false)))
	{
		Mask.reset();
	}
}

void FSelectionOutline::CollectOutlinedEntities(FScene& Scene, FEntity Root, std::vector<FEntity>& OutEntities)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Root))
	{
		return;
	}
	if (const FStaticMeshComponent* Mesh = Registry.TryGet<FStaticMeshComponent>(Root); Mesh != nullptr && Mesh->bVisible)
	{
		OutEntities.push_back(Root);
	}
	for (FEntity Child : Scene.GetChildren(Root))
	{
		CollectOutlinedEntities(Scene, Child, OutEntities);
	}
}

void FSelectionOutline::Render(FScene& Scene, FResourceManager& Resources, const FCamera& Camera, FEntity Selected, const FRenderOutput& Output)
{
	if (Rhi == nullptr || !Output.IsValid() || Output.Format != CompositeFormat)
	{
		return;
	}

	Entities.clear();
	CollectOutlinedEntities(Scene, Selected, Entities);
	if (Entities.empty())
	{
		return;
	}

	EnsureMask(Output.Width, Output.Height);
	if (!Mask)
	{
		return;
	}

	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();

	// 1) 마스크
	const float Clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	Mask->Begin(CommandList, Clear);
	CommandList->SetGraphicsRootSignature(MaskRootSignature.Get());
	CommandList->SetPipelineState(MaskPipeline.Get());

	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();
	FRegistry&       Registry       = Scene.GetRegistry();
	for (FEntity Entity : Entities)
	{
		const FStaticMesh* Mesh = Resources.GetMesh(Registry.Get<FStaticMeshComponent>(Entity).Mesh);
		if (Mesh == nullptr)
		{
			continue;
		}
		const FMatrix4x4 WorldViewProjection = Registry.Get<FTransformComponent>(Entity).WorldMatrix * ViewProjection;
		CommandList->SetGraphicsRoot32BitConstants(MaskRoot_Matrix, 16, &WorldViewProjection.M[0][0], 0);
		Mesh->Draw(CommandList);
	}
	Mask->End(CommandList);

	// 2) 합성 (출력 위에 알파 블렌드)
	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, nullptr);
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Output.Width), static_cast<float>(Output.Height), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Output.Width), static_cast<LONG>(Output.Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);

	FCompositeConstants Constants;
	Constants.OutlineColor = OutlineColor;
	Constants.FillColor    = FillColor;
	Constants.Thickness    = FMath::Clamp(Thickness, 1, 8);

	CommandList->SetGraphicsRootSignature(CompositeRootSignature.Get());
	CommandList->SetPipelineState(CompositePipeline.Get());
	CommandList->SetGraphicsRoot32BitConstants(CompositeRoot_Consts, sizeof(Constants) / 4, &Constants, 0);
	CommandList->SetGraphicsRootDescriptorTable(CompositeRoot_Mask, Mask->GetSrv().Gpu);
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->DrawInstanced(3, 1, 0, 0);
}
