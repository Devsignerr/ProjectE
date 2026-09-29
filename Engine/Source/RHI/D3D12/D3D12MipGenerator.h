#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include <vector>

// 컴퓨트 셰이더(GenerateMips.hlsl)로 2D 텍스처 밉 체인을 생성한다.
// 디바이스당 하나를 만들어 재사용 (FD3D12Device::GetMipGenerator).
class FD3D12MipGenerator
{
public:
	~FD3D12MipGenerator();

	bool Init(ID3D12Device* InDevice);
	void Shutdown();
	bool IsInitialized() const { return PipelineState.Get() != nullptr; }

	// 밉 생성이 가능한 포맷 (TYPELESS 리소스 + UNORM UAV 조합을 만들 수 있는 8비트 RGBA 계열)
	static bool SupportsFormat(DXGI_FORMAT Format);

	// 밉 1..MipCount-1 생성 명령을 기록한다.
	// 전제: 밉 0은 업로드 완료 후 COPY_DEST, 나머지 밉도 COPY_DEST 상태. 종료 시 모든 서브리소스는 PIXEL_SHADER_RESOURCE.
	// 임시 SRV/UAV는 Allocator(셰이더 가시)에서 할당해 OutTempDescriptors로 돌려주므로, 호출자가 GPU 실행 완료 후 해제한다.
	void RecordGenerateMips(ID3D12GraphicsCommandList* CommandList, ID3D12Resource* Texture, DXGI_FORMAT ViewFormat,
	                        uint32 Width, uint32 Height, uint32 MipCount, FD3D12DescriptorAllocator& Allocator,
	                        std::vector<FD3D12DescriptorHandle>& OutTempDescriptors);

private:
	ID3D12Device*        Device = nullptr; // 소유하지 않음 (FD3D12Device가 수명 관리)
	FD3D12ShaderCompiler ShaderCompiler;
	FD3D12RootSignature  RootSignature;
	FD3D12PipelineState  PipelineState;
};
