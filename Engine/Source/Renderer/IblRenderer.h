#pragma once

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/IblMath.h"

#include <memory>

class FD3D12RHI;
class FShaderLibrary;
class FCamera;

// 초기화 시 환경광을 생성한다. Rhi보다 먼저 Shutdown해야 한다.
class FIblRenderer
{
public:
	FIblRenderer() = default;
	~FIblRenderer();

	FIblRenderer(const FIblRenderer&) = delete;
	FIblRenderer& operator=(const FIblRenderer&) = delete;

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown(); // GPU Flush 이후 호출
	bool ReloadShaders(bool bForceRecompile);

	// 호출자가 HDR RTV/D32 DSV와 뷰포트를 바인딩한다.
	// 깊이 1에서 그리며 깊이를 쓰지 않는다. 카메라 이동은 하늘에 영향을 주지 않는다.
	void RenderSkybox(const FCamera& Camera, float Intensity = 1.0f);

	bool IsReady() const { return bReady; }

	// 하늘 환경 (Phase 33-7): Image가 nullptr이면 절차적 하늘, 아니면 등장방형 HDR → 하늘 큐브(512, Z축 회전) → 조도/프리필터 다시 생성.
	// GPU 동기 실행 (바뀔 때만 부른다). 실패하면 false (이전 환경 유지)
	bool SetEnvironment(const struct FEnvironmentImage* Image, float RotationDegrees);

	// 연속 SRV 3칸:
	// TextureCube 확산(E/pi), TextureCube GGX 프리필터, Texture2D BRDF(A,B).
	const FD3D12DescriptorHandle& GetLightingTable() const { return LightingTable; }
	const FD3D12DescriptorHandle& GetSkySrv() const { return SkySrv; }

	static constexpr uint32 PrefilterMipCount = IblMath::PrefilterMipCount;

private:
	bool CreateTexture(uint32 Size, uint16 Slices, uint16 Mips,
	                   ComPtr<ID3D12Resource>& Texture,
	                   D3D12_CPU_DESCRIPTOR_HANDLE Srv, const wchar_t* Name,
	                   D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	// 하늘 큐브를 Size로 다시 만든다 (이전 리소스는 지연 해제, SRV는 같은 칸에 다시 기록). 새 큐브는 PIXEL_SHADER_RESOURCE
	bool RecreateSky(uint32 Size);

	std::unique_ptr<FD3D12Texture>       EnvironmentTexture; // 등장방형 HDR (없으면 절차적 하늘)
	float                               EnvironmentRotation = 0.0f; // 라디안
	uint32                              SkySize = IblMath::SkyCubeSize;
	uint32                              SkyMipCount = IblMath::GetFullMipCount(IblMath::SkyCubeSize); // 필터드 중요도 샘플링용 밉 체인
	bool Generate(FShaderLibrary& Library, bool bRebuild = false);
	bool CreateSkyPipeline(FShaderLibrary& Library, FD3D12PipelineState& OutPipeline);
	FShaderLibrary* ShaderLibrary = nullptr; // 비소유: 씬 렌더러가 소유

	FD3D12RHI* Rhi = nullptr; // 비소유: 이 객체보다 오래 유지

	ComPtr<ID3D12Resource> Sky;
	ComPtr<ID3D12Resource> Irradiance;
	ComPtr<ID3D12Resource> Prefilter;
	ComPtr<ID3D12Resource> BrdfLut;

	FD3D12DescriptorHandle SkySrv;
	FD3D12DescriptorHandle LightingTable;

	FD3D12RootSignature SkyRoot;
	FD3D12PipelineState SkyPipeline;

	bool bReady = false;
};
