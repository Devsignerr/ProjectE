#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/ShaderTypes.h"

class FCamera;
class FD3D12RHI;
class FD3D12RenderTarget;
class FScene;
class FScreenPassRootSignature;
class FShaderLibrary;
struct FHeightFogComponent;

// 볼류메트릭 안개 계산에 필요한 프레임 입력 (FSceneRenderer가 채운다)
struct FVolumetricFogInputs
{
	D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants  = 0; // 방향광 캐스케이드 상수 (b1)
	FD3D12DescriptorHandle    ShadowMapSrv;         // (t0)
	ID3D12Resource*           ShadowMap        = nullptr; // 읽는 동안 PIXEL | NON_PIXEL로 전이
	D3D12_GPU_VIRTUAL_ADDRESS ClusterConstants = 0; // 로컬 라이트 개수 (b2)
	D3D12_GPU_VIRTUAL_ADDRESS LocalLights      = 0; // 라이트 목록 (t1, 루트 SRV — 업로드 힙)
	FVector3                  LightDirection   = FVector3(0.0f, 0.0f, -1.0f);
	FVector3                  LightColor;           // 색 × 강도
	FMatrix4x4                PrevViewProjection;   // 지터 없음
	bool                      bHistoryValid = false;
	uint64                    FrameIndex    = 0;
};

// 높이 지수 안개 + 볼류메트릭 안개 (FHeightFogComponent, 식은 Renderer/FogMath.h, 셰이더 Fog.hlsli/FogApply.hlsl/VolumetricFog.hlsl)
//   Prepare: 씬의 첫 안개 컴포넌트 → FFogConstants (꺼져 있으면 bEnabled 0)
//   RenderVolumetric (메인 패스 전): 주입(시간 누적, 이력 2장 번갈아) → 적분 → 결과 볼륨 (ALL_SHADER_RESOURCE)
//   Apply (메인 패스·하늘 뒤, 파티클 전): 전체 화면, 씬 깊이로 월드 위치 → 색 = 원래 × 투과율 + 산란 (알파 = TAA 마스크 유지)
//   파티클은 GetConstantsAddress/GetVolumeSrv로 정점에서 같은 식을 계산한다 (ParticleRenderer)
class FFogRenderer
{
public:
	static constexpr DXGI_FORMAT VolumeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	~FFogRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 씬 안개 설정 + 카메라(Camera = 깊이를 그린 지터 카메라, UnjitteredViewProjection = 볼륨 좌표) → 상수. 안개가 있으면 true
	bool Prepare(FScene& Scene, const FCamera& Camera, const FMatrix4x4& UnjitteredViewProjection, uint32 Width, uint32 Height);
	bool IsEnabled() const { return Constants.bEnabled != 0; }
	bool IsVolumetric() const { return Constants.bVolumetric != 0; }

	void RenderVolumetric(const FVolumetricFogInputs& Inputs);
	// SceneColor RTV가 바인딩된 상태에서 호출 (깊이는 DEPTH_WRITE로 받아 읽는 동안만 전이), 끝나면 SceneColor RTV + DSV 다시 바인딩
	void Apply(const FD3D12RenderTarget& SceneColor);

	// 이번 프레임 상수 (동적 업로드 버퍼, Prepare 이후 유효) / 결과 볼륨 (볼류메트릭이 꺼져 있어도 유효한 1칸 볼륨)
	D3D12_GPU_VIRTUAL_ADDRESS     GetConstantsAddress() const { return ConstantsAddress; }
	const FD3D12DescriptorHandle& GetVolumeSrv() const { return Volumes[IntegratedVolume].Srv; }
	const FFogConstants&          GetConstants() const { return Constants; }

private:
	// alignas(16) 상수를 첫 멤버로 (구조체 패딩 경고 C4324 방지)
	FFogConstants           Constants;
	FVolumetricFogConstants VolumeConstants;

	struct FVolume
	{
		ComPtr<ID3D12Resource> Resource;
		FD3D12DescriptorHandle Srv;
		FD3D12DescriptorHandle Uav;
		D3D12_RESOURCE_STATES  State = D3D12_RESOURCE_STATE_COMMON;
	};
	enum : uint32
	{
		HistoryVolume0   = 0,
		HistoryVolume1   = 1,
		IntegratedVolume = 2,
		VolumeCount      = 3,
	};

	bool CreatePipelines(FD3D12PipelineState& OutApply, FD3D12PipelineState& OutInject, FD3D12PipelineState& OutIntegrate, bool bForceRecompile);
	void EnsureVolumes(uint32 GridX, uint32 GridY, uint32 GridZ);
	void ReleaseVolumes();
	void Transition(FVolume& Volume, D3D12_RESOURCE_STATES After);

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12RootSignature             VolumeRoot;        // 볼류메트릭 계산 전용 (CBV 3개 + 그림자/라이트/볼륨)
	FD3D12PipelineState             ApplyPipeline;
	FD3D12PipelineState             InjectPipeline;
	FD3D12PipelineState             IntegratePipeline;

	FVolume Volumes[VolumeCount];
	uint32  GridSize[3]  = {};
	uint32  HistoryIndex = 0;     // 이번 프레임 주입을 쓸 이력 칸
	bool    bHasHistory  = false;

	D3D12_GPU_VIRTUAL_ADDRESS   ConstantsAddress = 0;
	uint32                      TargetWidth  = 0;
	uint32                      TargetHeight = 0;
};
