#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <vector>

// 루트 시그니처 빌더 (버전 1.1).
// Add* 로 파라미터를 순서대로 추가한 뒤 Finalize. 각 Add*는 루트 파라미터 인덱스를 반환한다.
class FD3D12RootSignature
{
public:
	~FD3D12RootSignature();

	uint32 AddConstants(uint32 Num32BitValues, uint32 ShaderRegister, uint32 RegisterSpace = 0,
	                    D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL);

	uint32 AddConstantBufferView(uint32 ShaderRegister, uint32 RegisterSpace = 0,
	                             D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL);

	// 루트 SRV/UAV (버퍼 전용 — 구조화/원시 버퍼를 GPU 주소로 바로 바인딩)
	uint32 AddShaderResourceView(uint32 ShaderRegister, uint32 RegisterSpace = 0,
	                             D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL);
	uint32 AddUnorderedAccessView(uint32 ShaderRegister, uint32 RegisterSpace = 0,
	                              D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL);

	uint32 AddDescriptorTable(std::vector<D3D12_DESCRIPTOR_RANGE1> Ranges,
	                          D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL);

	void AddStaticSampler(const D3D12_STATIC_SAMPLER_DESC& Sampler);

	bool Finalize(ID3D12Device* Device, D3D12_ROOT_SIGNATURE_FLAGS Flags, const wchar_t* DebugName);
	void Shutdown();

	ID3D12RootSignature* Get() const { return RootSignature.Get(); }

	static D3D12_DESCRIPTOR_RANGE1 MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE Type, uint32 NumDescriptors, uint32 BaseShaderRegister,
	                                         uint32 RegisterSpace = 0,
	                                         D3D12_DESCRIPTOR_RANGE_FLAGS Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE);

	static D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(uint32 ShaderRegister,
	                                                   D3D12_FILTER Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
	                                                   D3D12_TEXTURE_ADDRESS_MODE AddressMode = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
	                                                   D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_PIXEL,
	                                                   uint32 RegisterSpace = 0);

private:
	std::vector<D3D12_ROOT_PARAMETER1>                Parameters;
	std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> TableRanges; // 파라미터 인덱스별 범위 (테이블이 아니면 비어 있음)
	std::vector<D3D12_STATIC_SAMPLER_DESC>            StaticSamplers;
	ComPtr<ID3D12RootSignature>                       RootSignature;
};
