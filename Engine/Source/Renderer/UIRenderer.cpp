#include "Renderer/UIRenderer.h"

#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/ShaderTypes.h"
#include "UI/UIDrawList.h"
#include "UI/UIFont.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum ERootParameter : uint32
	{
		RootParam_Frame   = 0, // b0
		RootParam_Batch   = 1, // b1
		RootParam_Quads   = 2, // t1 (루트 SRV)
		RootParam_Texture = 3, // t0
	};

	constexpr uint64 GUploadReserveBytes = 64 * 1024; // 뒤따르는 패스 상수용 여유

	FShaderCompileDesc MakeDesc(const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"UI.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}
} // namespace

FUIRenderer::~FUIRenderer()
{
	Shutdown();
}

bool FUIRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT InColorFormat)
{
	E_CHECKF(Rhi == nullptr, "UI 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	Resources     = &InResources;
	ColorFormat   = InColorFormat;
	// 리소스 수거 루트: UI 이미지 파일 텍스처 (경로별로 기억해 다시 읽지 않으므로 살려 둔다)
	ResourceRootProviderId = Resources->AddRootProvider([this](FResourceRoots& Roots) {
		for (const auto& [Key, Handle] : FileTextures)
		{
			Roots.Add(Handle);
		}
	});

	const uint32 FrameIndex   = RootSignature.AddConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 BatchIndex   = RootSignature.AddConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 QuadsIndex   = RootSignature.AddShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 TextureIndex = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) },
	                                                             D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(FrameIndex == RootParam_Frame && BatchIndex == RootParam_Batch && QuadsIndex == RootParam_Quads && TextureIndex == RootParam_Texture);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"UIRootSignature"))
	{
		Rhi = nullptr;
		return false;
	}
	if (!CreatePipeline(Pipeline, false))
	{
		RootSignature.Shutdown();
		Rhi = nullptr;
		return false;
	}
	return true;
}

void FUIRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (auto& [Font, Texture] : FontTextures)
	{
		if (Texture.Handle.IsValid())
		{
			Resources->DestroyTexture(Texture.Handle);
		}
	}
	FontTextures.clear();
	FileTextures.clear(); // 파일 텍스처는 리소스 관리자 캐시가 소유
	Resources->RemoveRootProvider(ResourceRootProviderId);
	ResourceRootProviderId = 0;
	Pipeline.Shutdown();
	RootSignature.Shutdown();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
	Resources     = nullptr;
}

bool FUIRenderer::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	const FShaderCompileDesc Descs[] = { MakeDesc(L"VSMain", EShaderStage::Vertex), MakeDesc(L"PSMain", EShaderStage::Pixel) };
	ComPtr<IDxcBlob>         Blobs[2];
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (bForceRecompile && !ShaderLibrary->CookShader(Descs[Index]))
		{
			return false;
		}
		Blobs[Index] = ShaderLibrary->GetShader(Descs[Index]);
		if (!Blobs[Index])
		{
			return false;
		}
	}

	FGraphicsPipelineDesc Desc;
	Desc.RootSignature          = RootSignature.Get();
	Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(Blobs[0].Get());
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(Blobs[1].Get());
	Desc.RenderTargetFormats[0] = ColorFormat;
	Desc.DepthStencilFormat     = DXGI_FORMAT_UNKNOWN;
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	Desc.bDepthEnable           = false;
	Desc.bDepthWrite            = false;
	Desc.BlendMode              = EBlendMode::Alpha;
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"UIPipeline");
}

bool FUIRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "UI 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	Pipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

FTextureHandle FUIRenderer::GetFontTexture(const FUIFont& Font)
{
	FFontTexture& Entry = FontTextures[&Font];
	if (Entry.Handle.IsValid() && Entry.Version == Font.GetAtlasVersion())
	{
		return Entry.Handle;
	}
	// 새 글자가 구워졌다: 아틀라스 전체를 다시 올리고 이전 텍스처는 지연 해제
	const FTextureHandle NewHandle = Resources->CreateTexture(Font.GetAtlasSize(), Font.GetAtlasSize(), DXGI_FORMAT_R8_UNORM,
	                                                          Font.GetAtlasPixels().data(), 1, L"UIFontAtlas");
	if (!NewHandle.IsValid())
	{
		return Entry.Handle; // 실패하면 이전 아틀라스라도
	}
	if (Entry.Handle.IsValid())
	{
		Resources->DestroyTexture(Entry.Handle);
	}
	Entry.Handle  = NewHandle;
	Entry.Version = Font.GetAtlasVersion();
	Entry.Size    = Font.GetAtlasSize();
	return Entry.Handle;
}

const FD3D12Texture& FUIRenderer::ResolveTexture(const FUITextureRef& Texture, const std::filesystem::path& ContentDirectory)
{
	if (Texture.Font != nullptr)
	{
		return Resources->ResolveTexture(GetFontTexture(*Texture.Font));
	}
	if (Texture.Path.empty())
	{
		return Resources->ResolveTexture(FTextureHandle{});
	}
	const std::filesystem::path Relative = FStringConv::ToWide(Texture.Path);
	const std::filesystem::path Absolute = (Relative.is_absolute() ? Relative : ContentDirectory / Relative).lexically_normal();
	std::wstring                Key      = Absolute.generic_wstring();
	std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
	auto It = FileTextures.find(Key);
	if (It == FileTextures.end())
	{
		It = FileTextures.emplace(Key, Resources->LoadTexture(Absolute, ETextureUsage::Color)).first;
	}
	return Resources->ResolveTexture(It->second);
}

void FUIRenderer::Render(const FUIDrawList& DrawList, const FRenderOutput& Output, const std::filesystem::path& ContentDirectory)
{
	if (Rhi == nullptr || DrawList.IsEmpty() || !Output.IsValid())
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	// 사각형 전체를 한 번에 올린다 (프레임 업로드 버퍼 확장 상한을 넘으면 앞쪽만)
	const uint64 MaxBytes  = DynamicBuffer.GetMaxAllocation();
	const uint64 Available = MaxBytes > GUploadReserveBytes + 256 ? MaxBytes - GUploadReserveBytes - 256 : 0;
	const uint64 MaxQuads  = Available / sizeof(FUIDrawQuad);
	const uint64 QuadCount = std::min<uint64>(DrawList.Quads.size(), MaxQuads);
	if (QuadCount < DrawList.Quads.size() && !bWarnedBufferFull)
	{
		E_LOG(LogRenderer, Warning, "UI 사각형이 너무 많아 일부를 그리지 않습니다 ({}개 중 {}개)", DrawList.Quads.size(), QuadCount);
		bWarnedBufferFull = true;
	}
	if (QuadCount == 0)
	{
		return;
	}
	// 텍스처(특히 글꼴 아틀라스)를 먼저 준비한다 — 생성은 동기 업로드라 기록 중간에 끼우지 않는다
	std::vector<const FD3D12Texture*> BatchTextures;
	BatchTextures.reserve(DrawList.Batches.size());
	for (const FUIDrawBatch& Batch : DrawList.Batches)
	{
		BatchTextures.push_back(&ResolveTexture(Batch.Texture, ContentDirectory));
	}

	const uint64                  Bytes = QuadCount * sizeof(FUIDrawQuad);
	const FD3D12DynamicAllocation Quads = DynamicBuffer.Allocate(Bytes, 16);
	std::memcpy(Quads.CpuAddress, DrawList.Quads.data(), Bytes);

	FUIConstants Constants;
	Constants.ViewportSize = FVector2(static_cast<float>(Output.Width), static_cast<float>(Output.Height));

	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, nullptr);
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Output.Width), static_cast<float>(Output.Height), 0.0f, 1.0f };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipeline.Get());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Frame, DynamicBuffer.AllocateConstants(Constants).GpuAddress);
	CommandList->SetGraphicsRootShaderResourceView(RootParam_Quads, Quads.GpuAddress);
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (size_t Index = 0; Index < DrawList.Batches.size(); ++Index)
	{
		const FUIDrawBatch& Batch = DrawList.Batches[Index];
		if (Batch.FirstQuad >= QuadCount)
		{
			break;
		}
		const uint32 Count = static_cast<uint32>(std::min<uint64>(Batch.QuadCount, QuadCount - Batch.FirstQuad));
		// 시저 (출력 안으로 자르고 비면 건너뜀)
		const LONG Left   = static_cast<LONG>(std::clamp(Batch.Clip.Min.X, 0.0f, static_cast<float>(Output.Width)));
		const LONG Top    = static_cast<LONG>(std::clamp(Batch.Clip.Min.Y, 0.0f, static_cast<float>(Output.Height)));
		const LONG Right  = static_cast<LONG>(std::clamp(Batch.Clip.Max.X + 0.999f, 0.0f, static_cast<float>(Output.Width)));
		const LONG Bottom = static_cast<LONG>(std::clamp(Batch.Clip.Max.Y + 0.999f, 0.0f, static_cast<float>(Output.Height)));
		if (Right <= Left || Bottom <= Top || Count == 0)
		{
			continue;
		}
		const D3D12_RECT Scissor{ Left, Top, Right, Bottom };
		CommandList->RSSetScissorRects(1, &Scissor);

		FUIBatchConstants BatchConstants;
		BatchConstants.QuadOffset = Batch.FirstQuad;
		CommandList->SetGraphicsRootConstantBufferView(RootParam_Batch, DynamicBuffer.AllocateConstants(BatchConstants).GpuAddress);
		CommandList->SetGraphicsRootDescriptorTable(RootParam_Texture, BatchTextures[Index]->GetSrv().Gpu);
		CommandList->DrawInstanced(6, Count, 0, 0);
	}
	// 뒤따르는 패스를 위해 시저를 출력 전체로 되돌린다
	const D3D12_RECT FullScissor{ 0, 0, static_cast<LONG>(Output.Width), static_cast<LONG>(Output.Height) };
	CommandList->RSSetScissorRects(1, &FullScissor);
}
