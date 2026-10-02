#include "Renderer/ScreenPass.h"

#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"

namespace
{
	ComPtr<IDxcBlob> LoadScreenShader(FShaderLibrary& Library, const wchar_t* File, const wchar_t* Entry, EShaderStage Stage, bool bForceRecompile)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = File;
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		if (bForceRecompile && !Library.CookShader(Desc))
		{
			return nullptr;
		}
		return Library.GetShader(Desc);
	}
} // namespace

bool FScreenPassRootSignature::Init(ID3D12Device* Device)
{
	const uint32 Constants = RootSignature.AddConstantBufferView(0);
	E_CHECK(Constants == Root_Constants);
	for (uint32 Index = 0; Index < SrvCount; ++Index)
	{
		const uint32 Table = RootSignature.AddDescriptorTable(
			{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, Index, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
		E_CHECK(Table == Root_Srv0 + Index);
	}
	for (uint32 Index = 0; Index < UavCount; ++Index)
	{
		const uint32 Table = RootSignature.AddDescriptorTable(
			{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, Index, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
		E_CHECK(Table == Root_Uav0 + Index);
	}
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	D3D12_STATIC_SAMPLER_DESC Compare = FD3D12RootSignature::MakeStaticSampler(2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
	                                                                            D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_SHADER_VISIBILITY_ALL);
	Compare.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	Compare.BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	Compare.MaxAnisotropy  = 1;
	RootSignature.AddStaticSampler(Compare);
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_ALL));
	return RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ScreenPassRootSignature");
}

bool FScreenPassRootSignature::CreateGraphicsPipeline(FD3D12PipelineState& OutPipeline, ID3D12Device* Device, FShaderLibrary& Library,
                                                      const wchar_t* File, const wchar_t* PixelEntry, std::initializer_list<DXGI_FORMAT> Formats,
                                                      EBlendMode BlendMode, bool bForceRecompile, const wchar_t* DebugName) const
{
	const ComPtr<IDxcBlob> VertexShader = LoadScreenShader(Library, File, L"VSMain", EShaderStage::Vertex, bForceRecompile);
	const ComPtr<IDxcBlob> PixelShader  = LoadScreenShader(Library, File, PixelEntry, EShaderStage::Pixel, bForceRecompile);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature    = RootSignature.Get();
	Desc.VertexShader     = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Desc.PixelShader      = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	Desc.CullMode         = D3D12_CULL_MODE_NONE;
	Desc.bDepthEnable     = false;
	Desc.BlendMode        = BlendMode;
	Desc.NumRenderTargets = static_cast<uint32>(Formats.size());
	uint32 Index          = 0;
	for (const DXGI_FORMAT Format : Formats)
	{
		Desc.RenderTargetFormats[Index++] = Format;
	}
	return OutPipeline.InitGraphics(Device, Desc, DebugName);
}

bool FScreenPassRootSignature::CreateDepthOutputPipeline(FD3D12PipelineState& OutPipeline, ID3D12Device* Device, FShaderLibrary& Library,
                                                         const wchar_t* File, const wchar_t* PixelEntry, DXGI_FORMAT DepthFormat, bool bForceRecompile,
                                                         const wchar_t* DebugName) const
{
	const ComPtr<IDxcBlob> VertexShader = LoadScreenShader(Library, File, L"VSMain", EShaderStage::Vertex, bForceRecompile);
	const ComPtr<IDxcBlob> PixelShader  = LoadScreenShader(Library, File, PixelEntry, EShaderStage::Pixel, bForceRecompile);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature      = RootSignature.Get();
	Desc.VertexShader       = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Desc.PixelShader        = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	Desc.CullMode           = D3D12_CULL_MODE_NONE;
	Desc.NumRenderTargets   = 0;
	Desc.DepthStencilFormat = DepthFormat;
	Desc.bDepthEnable       = true;
	Desc.bDepthWrite        = true;
	Desc.DepthFunc          = D3D12_COMPARISON_FUNC_ALWAYS; // SV_Depth를 그대로 쓴다
	return OutPipeline.InitGraphics(Device, Desc, DebugName);
}

bool FScreenPassRootSignature::CreateComputePipeline(FD3D12PipelineState& OutPipeline, ID3D12Device* Device, FShaderLibrary& Library,
                                                     const wchar_t* File, const wchar_t* Entry, bool bForceRecompile, const wchar_t* DebugName) const
{
	const ComPtr<IDxcBlob> Shader = LoadScreenShader(Library, File, Entry, EShaderStage::Compute, bForceRecompile);
	if (!Shader)
	{
		return false;
	}
	return OutPipeline.InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), DebugName);
}

void SetScreenPassViewport(ID3D12GraphicsCommandList* CommandList, uint32 Width, uint32 Height)
{
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
}

void DrawScreenPass(ID3D12GraphicsCommandList* CommandList, const FScreenPassRootSignature& Root, const FD3D12PipelineState& Pipeline,
                    D3D12_GPU_VIRTUAL_ADDRESS Constants, std::initializer_list<FD3D12DescriptorHandle> Srvs, uint32 Width, uint32 Height)
{
	SetScreenPassViewport(CommandList, Width, Height);
	CommandList->SetGraphicsRootSignature(Root.Get());
	CommandList->SetPipelineState(Pipeline.Get());
	if (Constants != 0)
	{
		CommandList->SetGraphicsRootConstantBufferView(FScreenPassRootSignature::Root_Constants, Constants);
	}
	uint32 Index = 0;
	for (const FD3D12DescriptorHandle& Srv : Srvs)
	{
		if (Srv.IsValid() && Index < FScreenPassRootSignature::SrvCount)
		{
			CommandList->SetGraphicsRootDescriptorTable(FScreenPassRootSignature::Root_Srv0 + Index, Srv.Gpu);
		}
		++Index;
	}
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->DrawInstanced(3, 1, 0, 0);
}
