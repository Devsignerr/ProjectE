#include "RHI/D3D12/D3D12RootSignature.h"

#include "RHI/D3D12/D3D12PipelineCache.h"

FD3D12RootSignature::~FD3D12RootSignature()
{
	Shutdown();
}

uint32 FD3D12RootSignature::AddConstants(uint32 Num32BitValues, uint32 ShaderRegister, uint32 RegisterSpace,
                                         D3D12_SHADER_VISIBILITY Visibility)
{
	D3D12_ROOT_PARAMETER1 Parameter{};
	Parameter.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	Parameter.Constants.Num32BitValues = Num32BitValues;
	Parameter.Constants.ShaderRegister = ShaderRegister;
	Parameter.Constants.RegisterSpace  = RegisterSpace;
	Parameter.ShaderVisibility         = Visibility;

	Parameters.push_back(Parameter);
	TableRanges.emplace_back();
	return static_cast<uint32>(Parameters.size() - 1);
}

uint32 FD3D12RootSignature::AddConstantBufferView(uint32 ShaderRegister, uint32 RegisterSpace,
                                                  D3D12_SHADER_VISIBILITY Visibility)
{
	D3D12_ROOT_PARAMETER1 Parameter{};
	Parameter.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	Parameter.Descriptor.ShaderRegister = ShaderRegister;
	Parameter.Descriptor.RegisterSpace  = RegisterSpace;
	Parameter.Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
	Parameter.ShaderVisibility          = Visibility;

	Parameters.push_back(Parameter);
	TableRanges.emplace_back();
	return static_cast<uint32>(Parameters.size() - 1);
}

uint32 FD3D12RootSignature::AddShaderResourceView(uint32 ShaderRegister, uint32 RegisterSpace, D3D12_SHADER_VISIBILITY Visibility)
{
	D3D12_ROOT_PARAMETER1 Parameter{};
	Parameter.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
	Parameter.Descriptor.ShaderRegister = ShaderRegister;
	Parameter.Descriptor.RegisterSpace  = RegisterSpace;
	Parameter.Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
	Parameter.ShaderVisibility          = Visibility;

	Parameters.push_back(Parameter);
	TableRanges.emplace_back();
	return static_cast<uint32>(Parameters.size() - 1);
}

uint32 FD3D12RootSignature::AddUnorderedAccessView(uint32 ShaderRegister, uint32 RegisterSpace, D3D12_SHADER_VISIBILITY Visibility)
{
	D3D12_ROOT_PARAMETER1 Parameter{};
	Parameter.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
	Parameter.Descriptor.ShaderRegister = ShaderRegister;
	Parameter.Descriptor.RegisterSpace  = RegisterSpace;
	Parameter.Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
	Parameter.ShaderVisibility          = Visibility;

	Parameters.push_back(Parameter);
	TableRanges.emplace_back();
	return static_cast<uint32>(Parameters.size() - 1);
}

uint32 FD3D12RootSignature::AddDescriptorTable(std::vector<D3D12_DESCRIPTOR_RANGE1> Ranges,
                                               D3D12_SHADER_VISIBILITY Visibility)
{
	E_CHECKF(!Ranges.empty(), "디스크립터 테이블에는 최소 하나의 범위가 필요합니다");

	D3D12_ROOT_PARAMETER1 Parameter{};
	Parameter.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	Parameter.ShaderVisibility = Visibility;
	// 범위 포인터는 Finalize에서 연결 (벡터 재할당 대비)

	Parameters.push_back(Parameter);
	TableRanges.push_back(std::move(Ranges));
	return static_cast<uint32>(Parameters.size() - 1);
}

void FD3D12RootSignature::AddStaticSampler(const D3D12_STATIC_SAMPLER_DESC& Sampler)
{
	StaticSamplers.push_back(Sampler);
}

bool FD3D12RootSignature::Finalize(ID3D12Device* Device, D3D12_ROOT_SIGNATURE_FLAGS Flags, const wchar_t* DebugName)
{
	E_CHECKF(RootSignature == nullptr, "루트 시그니처가 이미 생성되어 있습니다");

	D3D12_FEATURE_DATA_ROOT_SIGNATURE FeatureData{};
	FeatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
	if (FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &FeatureData, sizeof(FeatureData))) ||
	    FeatureData.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1)
	{
		E_LOG(LogD3D12, Error, "루트 시그니처 버전 1.1을 지원하지 않는 디바이스입니다");
		return false;
	}

	for (size_t Index = 0; Index < Parameters.size(); ++Index)
	{
		if (Parameters[Index].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
		{
			Parameters[Index].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(TableRanges[Index].size());
			Parameters[Index].DescriptorTable.pDescriptorRanges   = TableRanges[Index].data();
		}
	}

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC VersionedDesc{};
	VersionedDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
	VersionedDesc.Desc_1_1.NumParameters     = static_cast<UINT>(Parameters.size());
	VersionedDesc.Desc_1_1.pParameters       = Parameters.data();
	VersionedDesc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(StaticSamplers.size());
	VersionedDesc.Desc_1_1.pStaticSamplers   = StaticSamplers.data();
	VersionedDesc.Desc_1_1.Flags             = Flags;

	ComPtr<ID3DBlob> SerializedBlob;
	ComPtr<ID3DBlob> ErrorBlob;
	const HRESULT SerializeHr = D3D12SerializeVersionedRootSignature(&VersionedDesc, &SerializedBlob, &ErrorBlob);
	if (FAILED(SerializeHr))
	{
		const char* ErrorText = ErrorBlob ? static_cast<const char*>(ErrorBlob->GetBufferPointer()) : "(메시지 없음)";
		E_LOG(LogD3D12, Error, "루트 시그니처 직렬화 실패 ({}): {}", HResultToString(SerializeHr), ErrorText);
		return false;
	}

	E_D3D_VERIFY(Device->CreateRootSignature(0, SerializedBlob->GetBufferPointer(), SerializedBlob->GetBufferSize(),
	                                         IID_PPV_ARGS(&RootSignature)));
	RootSignature->SetName(DebugName);
	FD3D12PipelineCache::Get().RegisterRootSignature(RootSignature.Get(), SerializedBlob->GetBufferPointer(), SerializedBlob->GetBufferSize()); // PSO 캐시 키
	return true;
}

void FD3D12RootSignature::Shutdown()
{
	if (RootSignature)
	{
		FD3D12PipelineCache::Get().UnregisterRootSignature(RootSignature.Get());
	}
	RootSignature.Reset();
	Parameters.clear();
	TableRanges.clear();
	StaticSamplers.clear();
}

D3D12_DESCRIPTOR_RANGE1 FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE Type, uint32 NumDescriptors,
                                                       uint32 BaseShaderRegister, uint32 RegisterSpace,
                                                       D3D12_DESCRIPTOR_RANGE_FLAGS Flags)
{
	D3D12_DESCRIPTOR_RANGE1 Range{};
	Range.RangeType                         = Type;
	Range.NumDescriptors                    = NumDescriptors;
	Range.BaseShaderRegister                = BaseShaderRegister;
	Range.RegisterSpace                     = RegisterSpace;
	Range.Flags                             = Flags;
	Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	return Range;
}

D3D12_STATIC_SAMPLER_DESC FD3D12RootSignature::MakeStaticSampler(uint32 ShaderRegister, D3D12_FILTER Filter,
                                                                 D3D12_TEXTURE_ADDRESS_MODE AddressMode,
                                                                 D3D12_SHADER_VISIBILITY Visibility, uint32 RegisterSpace)
{
	D3D12_STATIC_SAMPLER_DESC Sampler{};
	Sampler.Filter           = Filter;
	Sampler.AddressU         = AddressMode;
	Sampler.AddressV         = AddressMode;
	Sampler.AddressW         = AddressMode;
	Sampler.MipLODBias       = 0.0f;
	Sampler.MaxAnisotropy    = (Filter == D3D12_FILTER_ANISOTROPIC) ? 16 : 1;
	Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
	Sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	Sampler.MinLOD           = 0.0f;
	Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
	Sampler.ShaderRegister   = ShaderRegister;
	Sampler.RegisterSpace    = RegisterSpace;
	Sampler.ShaderVisibility = Visibility;
	return Sampler;
}
