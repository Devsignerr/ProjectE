#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShaderTypes.h"

#include <chrono>
#include <vector>

class FD3D12RHI;
class FD3D12RenderTarget;
class FScene;
class FShaderLibrary;
struct FFrustum;

// 물 패스 입력 (FSceneRenderer가 채운다 — 씬 타깃 참조, 카메라, 조명·안개·그림자·반사 리소스)
struct FWaterPassInputs
{
	const FD3D12RenderTarget* SceneColor    = nullptr; // 씬 컬러 + 깊이 (내부 해상도)
	const FD3D12RenderTarget* SceneVelocity = nullptr;
	FRGResourceRef            ColorRef;
	FRGResourceRef            DepthRef;
	FRGResourceRef            VelocityRef;
	FRGResourceRef            ShadowMapRef;
	FRGResourceRef            FogVolumeRef;
	FMatrix4x4                ViewProjection;           // 지터 포함
	FMatrix4x4                UnjitteredViewProjection;
	FMatrix4x4                PrevViewProjection;
	FVector3                  CameraPosition;
	FVector3                  SunDirection = FVector3(0.0f, 0.0f, 1.0f); // 태양 쪽
	FVector3                  SunColor;
	float                     AmbientIntensity       = 1.0f;
	uint32                    ReflectionCaptureCount = 0;
	D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants = 0;
	D3D12_GPU_VIRTUAL_ADDRESS FogConstants    = 0;
	D3D12_GPU_VIRTUAL_ADDRESS CaptureList     = 0;
	FD3D12DescriptorHandle    ShadowMapSrv;
	FD3D12DescriptorHandle    FogVolumeSrv;
	FD3D12DescriptorHandle    IblTable;     // 조도 / 프리필터 / BRDF (FIblRenderer::GetLightingTable — 대기 실시간 IBL 포함)
	FD3D12DescriptorHandle    CaptureAtlasSrv;
};

// 소규모 물 (Phase 49, FWaterBodyComponent — 식은 Renderer/WaterMath.h, 셰이더 Water.hlsl)
//   Prepare: 보이는 물 상자 수집(프러스텀) + 카메라가 들어간 상자
//   AddSurfacePass (안개 적용 뒤·반투명 메시 전): 씬 컬러 → 그래프 텍스처 복사(굴절 원본) → 수면 사각형(깊이는 셰이더에서 비교, 쓰지 않음)
//     — MRT 씬 컬러(알파 = TAA 반응형 0.2) + 움직임 벡터(수면 기준)
//   AddUnderwaterPass (파티클 뒤, 씬 컬러의 마지막): 카메라가 물 상자 안이면 전체 화면 물속 흡수/산란 (프리멀티플라이드)
//   잔물결 노멀은 시작 때 CPU로 만든 타일 텍스처 한 장(정수 파수 코사인 합 = 이음매 없음, B = 거품 워리 노이즈)을 두 번 패닝
class FWaterRenderer
{
public:
	~FWaterRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 보이는 물 수 (0이면 패스 없음)
	uint32 Prepare(FScene& Scene, const FFrustum& Frustum, const FVector3& CameraPosition);
	bool   IsCameraUnderwater() const { return UnderwaterIndex >= 0; }

	void AddSurfacePass(FRenderGraph& Graph, const FWaterPassInputs& Inputs, int32 Timer);
	void AddUnderwaterPass(FRenderGraph& Graph, const FWaterPassInputs& Inputs, int32 Timer);

private:
	FWaterBodyConstants UnderwaterBody; // 카메라가 들어간 물 (alignas(16) 상수를 첫 데이터 멤버로 — C4324)

	bool CreatePipelines(bool bForceRecompile);
	bool CreateWaveTexture();
	D3D12_GPU_VIRTUAL_ADDRESS UploadFrameConstants(const FWaterPassInputs& Inputs) const;
	void BindCommon(ID3D12GraphicsCommandList* List, D3D12_GPU_VIRTUAL_ADDRESS Frame, const FWaterPassInputs& Inputs,
	                const FD3D12DescriptorHandle& CopySrv) const;

	FD3D12RHI*          Rhi     = nullptr;
	FShaderLibrary*     Library = nullptr;
	FD3D12RootSignature RootSignature;
	FD3D12PipelineState SurfacePipeline;
	FD3D12PipelineState UnderwaterPipeline;
	FD3D12Texture       WaveTexture;

	std::vector<FWaterBodyConstants> Bodies;          // 이번 렌더 보이는 물
	int32                            UnderwaterIndex = -1; // 카메라가 들어간 물 (Bodies 안 번호)
	std::chrono::steady_clock::time_point StartTime;
};
