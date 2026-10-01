#include "RHI/D3D12/D3D12PipelineState.h"

FD3D12PipelineState::~FD3D12PipelineState()
{
	Shutdown();
}

bool FD3D12PipelineState::InitGraphics(ID3D12Device* Device, const FGraphicsPipelineDesc& Desc, const wchar_t* DebugName)
{
	E_CHECKF(PipelineState == nullptr, "파이프라인 스테이트가 이미 생성되어 있습니다");
	E_CHECKF(Desc.RootSignature != nullptr, "루트 시그니처가 필요합니다");
	E_CHECKF(Desc.VertexShader.pShaderBytecode != nullptr, "정점 셰이더가 필요합니다");
	E_CHECKF(Desc.NumRenderTargets <= 8, "렌더 타깃은 최대 8개입니다");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc{};
	PsoDesc.pRootSignature = Desc.RootSignature;
	PsoDesc.VS             = Desc.VertexShader;
	PsoDesc.PS             = Desc.PixelShader;

	// 블렌드
	PsoDesc.BlendState.AlphaToCoverageEnable  = FALSE;
	PsoDesc.BlendState.IndependentBlendEnable = FALSE;
	const EBlendMode BlendMode = Desc.GetEffectiveBlendMode();
	for (D3D12_RENDER_TARGET_BLEND_DESC& Target : PsoDesc.BlendState.RenderTarget)
	{
		Target.BlendEnable   = BlendMode != EBlendMode::Opaque ? TRUE : FALSE;
		Target.LogicOpEnable = FALSE;
		if (BlendMode == EBlendMode::Additive)
		{
			Target.SrcBlend       = D3D12_BLEND_ONE;
			Target.DestBlend      = D3D12_BLEND_ONE;
			Target.SrcBlendAlpha  = D3D12_BLEND_ONE;
			Target.DestBlendAlpha = D3D12_BLEND_ONE;
		}
		else if (BlendMode == EBlendMode::Remaining)
		{
			Target.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
			Target.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
			Target.SrcBlendAlpha  = D3D12_BLEND_ZERO;
			Target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		}
		else
		{
			Target.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
			Target.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
			Target.SrcBlendAlpha  = D3D12_BLEND_ONE;
			Target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		}
		Target.BlendOp               = D3D12_BLEND_OP_ADD;
		Target.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		Target.LogicOp               = D3D12_LOGIC_OP_NOOP;
		Target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	PsoDesc.SampleMask = UINT_MAX;

	// 래스터라이저
	PsoDesc.RasterizerState.FillMode              = Desc.FillMode;
	PsoDesc.RasterizerState.CullMode              = Desc.CullMode;
	PsoDesc.RasterizerState.FrontCounterClockwise = Desc.bFrontCounterClockwise ? TRUE : FALSE;
	PsoDesc.RasterizerState.DepthBias             = Desc.DepthBias;
	PsoDesc.RasterizerState.DepthBiasClamp        = Desc.DepthBiasClamp;
	PsoDesc.RasterizerState.SlopeScaledDepthBias  = Desc.SlopeScaledDepthBias;
	PsoDesc.RasterizerState.DepthClipEnable       = Desc.bDepthClip ? TRUE : FALSE;
	PsoDesc.RasterizerState.MultisampleEnable     = FALSE;
	PsoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
	PsoDesc.RasterizerState.ForcedSampleCount     = 0;
	PsoDesc.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	// 깊이/스텐실
	PsoDesc.DepthStencilState.DepthEnable      = Desc.bDepthEnable ? TRUE : FALSE;
	PsoDesc.DepthStencilState.DepthWriteMask   = Desc.bDepthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	PsoDesc.DepthStencilState.DepthFunc        = Desc.DepthFunc;
	PsoDesc.DepthStencilState.StencilEnable    = FALSE;
	PsoDesc.DepthStencilState.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
	PsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	const D3D12_DEPTH_STENCILOP_DESC DefaultStencilOp{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
	                                                   D3D12_COMPARISON_FUNC_ALWAYS };
	PsoDesc.DepthStencilState.FrontFace = DefaultStencilOp;
	PsoDesc.DepthStencilState.BackFace  = DefaultStencilOp;

	// 입력 조립
	PsoDesc.InputLayout.pInputElementDescs = Desc.InputLayout.empty() ? nullptr : Desc.InputLayout.data();
	PsoDesc.InputLayout.NumElements        = static_cast<UINT>(Desc.InputLayout.size());
	PsoDesc.IBStripCutValue                = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	PsoDesc.PrimitiveTopologyType          = Desc.PrimitiveTopologyType;

	// 출력
	PsoDesc.NumRenderTargets = Desc.NumRenderTargets;
	for (uint32 Index = 0; Index < Desc.NumRenderTargets; ++Index)
	{
		PsoDesc.RTVFormats[Index] = Desc.RenderTargetFormats[Index];
	}
	PsoDesc.DSVFormat  = Desc.DepthStencilFormat;
	PsoDesc.SampleDesc = { 1, 0 };
	PsoDesc.NodeMask   = 0;
	PsoDesc.Flags      = D3D12_PIPELINE_STATE_FLAG_NONE;

	E_D3D_VERIFY(Device->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(&PipelineState)));
	PipelineState->SetName(DebugName);
	return true;
}

bool FD3D12PipelineState::InitCompute(ID3D12Device* Device, ID3D12RootSignature* RootSignature,
                                      const D3D12_SHADER_BYTECODE& ComputeShader, const wchar_t* DebugName)
{
	E_CHECKF(PipelineState == nullptr, "파이프라인 스테이트가 이미 생성되어 있습니다");
	E_CHECKF(RootSignature != nullptr, "루트 시그니처가 필요합니다");
	E_CHECKF(ComputeShader.pShaderBytecode != nullptr, "컴퓨트 셰이더가 필요합니다");

	D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc{};
	PsoDesc.pRootSignature = RootSignature;
	PsoDesc.CS             = ComputeShader;
	PsoDesc.NodeMask       = 0;
	PsoDesc.Flags          = D3D12_PIPELINE_STATE_FLAG_NONE;

	E_D3D_VERIFY(Device->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(&PipelineState)));
	PipelineState->SetName(DebugName);
	return true;
}

void FD3D12PipelineState::Shutdown()
{
	PipelineState.Reset();
}
