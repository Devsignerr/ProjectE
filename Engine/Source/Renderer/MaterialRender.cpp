#include "Renderer/MaterialRender.h"

#include "Core/FrameTime.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/StaticMesh.h"

#include <cmath>
#include <cstring>
#include <format>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace MaterialRender
{
	float GetMaterialTime()
	{
		const double Seconds = FFrameTime::GetTotalSeconds(); // 앱 프레임 시간 (실제 시각이 아님 — --fixed-delta 재현성)
		return static_cast<float>(std::fmod(Seconds, 3600.0)); // float 정밀도 유지
	}

	D3D12_GPU_VIRTUAL_ADDRESS UploadMaterialConstants(FD3D12DynamicUploadBuffer& DynamicBuffer, const FMaterial& Material)
	{
		if (Material.Shader == nullptr)
		{
			return DynamicBuffer.AllocateConstants(Material.Constants).GpuAddress;
		}
		FMaterialGraphHeader Header;
		Header.Time        = GetMaterialTime();
		Header.AlphaCutoff = Material.Constants.AlphaCutoff;
		const size_t Bytes = sizeof(Header) + sizeof(FVector4) * Material.GraphConstants.size();
		const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(AlignUp<uint64>(Bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
		uint8* Destination = static_cast<uint8*>(Allocation.CpuAddress);
		std::memcpy(Destination, &Header, sizeof(Header)); // 업로드 힙(쓰기 결합)은 순차 쓰기만
		if (!Material.GraphConstants.empty())
		{
			std::memcpy(Destination + sizeof(Header), Material.GraphConstants.data(), sizeof(FVector4) * Material.GraphConstants.size());
		}
		return Allocation.GpuAddress;
	}

	FShaderCompileDesc MakeGraphShaderDesc(const wchar_t* FileName, const wchar_t* EntryPoint, EShaderStage Stage, const FMaterialShader& Shader)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = FileName;
		Desc.EntryPoint = EntryPoint;
		Desc.Stage      = Stage;
		Desc.Defines.push_back(L"E_MATERIAL_GRAPH=1");
		Desc.VirtualFiles.push_back({ FStringConv::ToWide(FMaterialGraphCompiler::GeneratedFileName), Shader.Hlsl });
		return Desc;
	}

	std::vector<FShaderCompileDesc> GetGraphShaderDescs(const FMaterialShader& Shader)
	{
		std::vector<FShaderCompileDesc> Descs;
		for (const wchar_t* Entry : { L"PSMain", L"PSMainMasked", L"PSPrepass", L"PSPrepassMasked", L"PSTranslucent", L"PSAdditive" })
		{
			Descs.push_back(MakeGraphShaderDesc(L"Mesh.hlsl", Entry, EShaderStage::Pixel, Shader));
		}
		Descs.push_back(MakeGraphShaderDesc(L"Shadow.hlsl", L"ShadowMaterialPS", EShaderStage::Pixel, Shader));
		return Descs;
	}
} // namespace MaterialRender

FMaterialDepthPipelines::~FMaterialDepthPipelines()
{
	Shutdown();
}

void FMaterialDepthPipelines::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, const wchar_t* InDebugName)
{
	Rhi                  = &InRhi;
	State                = std::make_unique<FState>();
	State->ShaderLibrary = &InShaderLibrary;
	State->DebugName     = InDebugName;
}

void FMaterialDepthPipelines::SetBaseDesc(const FGraphicsPipelineDesc& Desc)
{
	if (!State)
	{
		return;
	}
	const FGraphicsPipelineDesc& BaseDesc = State->BaseDesc;
	const bool bSame = BaseDesc.RootSignature == Desc.RootSignature && BaseDesc.DepthBias == Desc.DepthBias &&
	                   BaseDesc.SlopeScaledDepthBias == Desc.SlopeScaledDepthBias && BaseDesc.bDepthClip == Desc.bDepthClip &&
	                   BaseDesc.DepthStencilFormat == Desc.DepthStencilFormat && BaseDesc.CullMode == Desc.CullMode;
	State->BaseDesc = Desc;
	if (!bSame)
	{
		Reset();
	}
}

ID3D12PipelineState* FMaterialDepthPipelines::Get(const FMaterialShader& Shader, bool bSkinned)
{
	if (Rhi == nullptr || !State || State->BaseDesc.RootSignature == nullptr)
	{
		return nullptr;
	}
	std::unique_ptr<FEntry>& Entry = State->Entries[Shader.Hash];
	if (!Entry)
	{
		Entry = std::make_unique<FEntry>();
	}
	const uint32 Index = bSkinned ? 1 : 0;
	if (!Entry->bTried[Index])
	{
		Entry->bTried[Index] = true;
		FShaderCompileDesc VertexDesc;
		VertexDesc.FileName                 = L"Shadow.hlsl";
		VertexDesc.EntryPoint               = bSkinned ? L"ShadowMaterialSkinnedVS" : L"ShadowMaterialVS";
		VertexDesc.Stage                    = EShaderStage::Vertex;
		const FShaderCompileDesc PixelDesc  = MaterialRender::MakeGraphShaderDesc(L"Shadow.hlsl", L"ShadowMaterialPS", EShaderStage::Pixel, Shader);
		const ComPtr<IDxcBlob>   VertexBlob = State->ShaderLibrary->GetShader(VertexDesc);
		const ComPtr<IDxcBlob>   PixelBlob  = State->ShaderLibrary->GetShader(PixelDesc);
		bool                     bOk        = VertexBlob && PixelBlob;
		if (bOk)
		{
			FGraphicsPipelineDesc Desc = State->BaseDesc;
			Desc.VertexShader          = FD3D12ShaderCompiler::ToBytecode(VertexBlob.Get());
			Desc.PixelShader           = FD3D12ShaderCompiler::ToBytecode(PixelBlob.Get());
			Desc.InputLayout           = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout();
			const std::wstring Name    = std::format(L"{}{}_{:016x}", State->DebugName, bSkinned ? L"Skinned" : L"", Shader.Hash);
			bOk                        = Entry->Pipelines[Index].InitGraphics(Rhi->GetDevice().GetDevice(), Desc, Name.c_str());
		}
		if (!bOk)
		{
			E_LOG(LogRenderer, Error, "그래프 머티리얼 그림자 PSO 생성 실패 ({:016x}) — 알파 테스트 없이 그립니다", Shader.Hash);
			Entry->bFailed[Index] = true;
		}
	}
	return Entry->bFailed[Index] ? nullptr : Entry->Pipelines[Index].Get();
}

void FMaterialDepthPipelines::Reset()
{
	if (!State)
	{
		return;
	}
	for (auto& [Hash, Entry] : State->Entries)
	{
		for (FD3D12PipelineState& Pipeline : Entry->Pipelines)
		{
			if (Pipeline.Get() != nullptr && Rhi != nullptr)
			{
				Rhi->DeferRelease(Pipeline.Detach()); // 진행 중인 프레임이 참조할 수 있다
			}
		}
	}
	State->Entries.clear();
}

void FMaterialDepthPipelines::Shutdown()
{
	State.reset(); // 호출자가 GPU Flush 이후 종료
	Rhi = nullptr;
}
