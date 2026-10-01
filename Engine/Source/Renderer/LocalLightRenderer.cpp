#include "Renderer/LocalLightRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/LightMath.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <numeric>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr uint32 GCullGroupSize = 64; // ClusterCulling.hlsl numthreads

	enum EComputeRootParameter : uint32
	{
		ComputeParam_Constants = 0, // b0
		ComputeParam_Lights    = 1, // t0
		ComputeParam_Clusters  = 2, // u0
	};
} // namespace

FLocalLightRenderer::~FLocalLightRenderer()
{
	Shutdown();
}

bool FLocalLightRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "로컬 라이트 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	const uint32 ConstantsIndex = ComputeRootSignature.AddConstantBufferView(0);
	const uint32 LightsIndex    = ComputeRootSignature.AddShaderResourceView(0);
	const uint32 ClustersIndex  = ComputeRootSignature.AddUnorderedAccessView(0);
	E_CHECK(ConstantsIndex == ComputeParam_Constants && LightsIndex == ComputeParam_Lights && ClustersIndex == ComputeParam_Clusters);
	if (!ComputeRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ClusterCullingRootSignature"))
	{
		return false;
	}
	if (!CreatePipeline(CullPipeline, false))
	{
		return false;
	}

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   Desc = MakeBufferDesc(sizeof(uint32) * static_cast<uint64>(LightMath::ClusterCount) * LightMath::ClusterStride,
	                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ClusterBuffer))))
	{
		E_LOG(LogRenderer, Error, "클러스터 라이트 버퍼를 만들지 못했습니다");
		return false;
	}
	ClusterBuffer->SetName(L"ClusterLightData");
	ClusterState = D3D12_RESOURCE_STATE_COMMON;

	Lights.reserve(64);
	return true;
}

void FLocalLightRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	ClusterBuffer.Reset(); // 호출자(씬 렌더러)가 GPU Flush 이후 종료한다
	CullPipeline.Shutdown();
	ComputeRootSignature.Shutdown();
	Lights.clear();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FLocalLightRenderer::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	FShaderCompileDesc Desc;
	Desc.FileName   = L"ClusterCulling.hlsl";
	Desc.EntryPoint = L"CSMain";
	Desc.Stage      = EShaderStage::Compute;
	if (bForceRecompile && !ShaderLibrary->CookShader(Desc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> Shader = ShaderLibrary->GetShader(Desc);
	if (!Shader)
	{
		return false;
	}
	return OutPipeline.InitCompute(Rhi->GetDevice().GetDevice(), ComputeRootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()),
	                               L"ClusterCullingPipeline");
}

bool FLocalLightRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "클러스터 컬링 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	CullPipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

D3D12_GPU_VIRTUAL_ADDRESS FLocalLightRenderer::GetClusterData() const
{
	return ClusterBuffer ? ClusterBuffer->GetGPUVirtualAddress() : 0;
}

void FLocalLightRenderer::TransitionClusters(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After)
{
	if (ClusterState != After)
	{
		const D3D12_RESOURCE_BARRIER Barrier = MakeTransitionBarrier(ClusterBuffer.Get(), ClusterState, After);
		CommandList->ResourceBarrier(1, &Barrier);
		ClusterState = After;
	}
}

void FLocalLightRenderer::CollectLights(FScene& Scene, const FCamera& Camera)
{
	Lights.clear();
	LightScores.clear();

	const FFrustum Frustum        = FFrustum::FromViewProjection(Camera.GetViewProjectionMatrix());
	const FVector3 CameraPosition = Camera.GetPosition();
	FRegistry&     Registry       = Scene.GetRegistry();

	auto AddLight = [&](const FVector3& Position, const FVector3& SrgbColor, float Intensity, float Radius) -> FLocalLightGpuData* {
		if (Intensity <= 0.0f || Radius <= 0.0f)
		{
			return nullptr;
		}
		if (!Frustum.Intersects(FBox(Position - FVector3(Radius), Position + FVector3(Radius))))
		{
			return nullptr;
		}
		FLocalLightGpuData& Light = Lights.emplace_back();
		Light.Position            = Position;
		Light.Radius              = Radius;
		Light.Color               = LightMath::SrgbToLinear(SrgbColor) * Intensity;
		LightScores.push_back(FMath::Max(FVector3::Distance(Position, CameraPosition) - Radius, 0.0f));
		return &Light;
	};

	Registry.View<FTransformComponent, FPointLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FPointLightComponent& Point) {
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Point.Color, Point.Intensity, Point.Radius))
		{
			Light->Type = static_cast<uint32>(LightMath::ELocalLightType::Point);
		}
	});
	Registry.View<FTransformComponent, FSpotLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FSpotLightComponent& Spot) {
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Spot.Color, Spot.Intensity, Spot.Radius))
		{
			const LightMath::FConeParams Cone = LightMath::ComputeConeParams(Spot.InnerConeAngle, Spot.OuterConeAngle);
			Light->Type       = static_cast<uint32>(LightMath::ELocalLightType::Spot);
			Light->Direction  = Transform.GetWorldForward();
			Light->ConeScale  = Cone.Scale;
			Light->ConeOffset = Cone.Offset;
		}
	});

	// 상한을 넘으면 카메라에 가까운(영향 구 기준) 라이트만 남긴다
	if (Lights.size() > LightMath::MaxLocalLights)
	{
		std::vector<uint32> Order(Lights.size());
		std::iota(Order.begin(), Order.end(), 0u);
		std::nth_element(Order.begin(), Order.begin() + LightMath::MaxLocalLights, Order.end(),
		                 [&](uint32 A, uint32 B) { return LightScores[A] < LightScores[B]; });
		Order.resize(LightMath::MaxLocalLights);
		std::vector<FLocalLightGpuData> Kept;
		std::vector<float>              KeptScores;
		Kept.reserve(Order.size());
		KeptScores.reserve(Order.size());
		for (const uint32 Index : Order)
		{
			Kept.push_back(Lights[Index]);
			KeptScores.push_back(LightScores[Index]);
		}
		Lights.swap(Kept);
		LightScores.swap(KeptScores);
	}
}

void FLocalLightRenderer::Prepare(FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height)
{
	E_CHECKF(Rhi != nullptr, "로컬 라이트 렌더러가 초기화되지 않았습니다");

	CollectLights(Scene, Camera);

	// ---- 상수
	const float NearZ = Camera.GetNearZ();
	const float FarZ  = FMath::Max(Camera.GetFarZ(), NearZ * 2.0f);
	const LightMath::FSliceParams Slices = LightMath::ComputeSliceParams(NearZ, FarZ, LightMath::ClusterGridZ);

	Constants               = FClusterConstants{};
	Constants.View          = Camera.GetViewMatrix();
	Constants.GridX         = LightMath::ClusterGridX;
	Constants.GridY         = LightMath::ClusterGridY;
	Constants.GridZ         = LightMath::ClusterGridZ;
	Constants.LightCount    = static_cast<uint32>(Lights.size());
	Constants.ScreenSize    = FVector2(static_cast<float>(FMath::Max<uint32>(Width, 1)), static_cast<float>(FMath::Max<uint32>(Height, 1)));
	Constants.SliceScale    = Slices.Scale;
	Constants.SliceBias     = Slices.Bias;
	Constants.NearZ         = NearZ;
	Constants.FarZ          = FarZ;
	Constants.bOrthographic = Camera.IsOrthographic() ? 1u : 0u;
	if (Camera.IsOrthographic())
	{
		Constants.ProjScaleY = Camera.GetOrthoHeight() * 0.5f;
		Constants.ProjScaleX = Constants.ProjScaleY * Camera.GetAspectRatio();
	}
	else
	{
		Constants.ProjScaleY = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
		Constants.ProjScaleX = Constants.ProjScaleY * Camera.GetAspectRatio();
	}

	// ---- 업로드 (빈 목록이어도 루트 SRV가 유효한 주소를 가리키게 한 칸은 잡는다)
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	const uint64               ListBytes     = sizeof(FLocalLightGpuData) * FMath::Max<size_t>(Lights.size(), 1);
	const FD3D12DynamicAllocation List       = DynamicBuffer.Allocate(ListBytes, 16);
	if (!Lights.empty())
	{
		std::memcpy(List.CpuAddress, Lights.data(), sizeof(FLocalLightGpuData) * Lights.size());
	}
	else
	{
		std::memset(List.CpuAddress, 0, ListBytes);
	}
	LightListAddress = List.GpuAddress;
	ConstantsAddress = DynamicBuffer.AllocateConstants(Constants).GpuAddress;

	// ---- 클러스터 컬링
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	TransitionClusters(CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	CommandList->SetComputeRootSignature(ComputeRootSignature.Get());
	CommandList->SetPipelineState(CullPipeline.Get());
	CommandList->SetComputeRootConstantBufferView(ComputeParam_Constants, ConstantsAddress);
	CommandList->SetComputeRootShaderResourceView(ComputeParam_Lights, LightListAddress);
	CommandList->SetComputeRootUnorderedAccessView(ComputeParam_Clusters, ClusterBuffer->GetGPUVirtualAddress());
	CommandList->Dispatch((LightMath::ClusterCount + GCullGroupSize - 1) / GCullGroupSize, 1, 1);
	TransitionClusters(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
