#include "Renderer/SkyAtmosphereRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/IblMath.h"
#include "Renderer/IblRenderer.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SkyAtmosphere.h"

#include <cstring>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum EAtmosphereRoot : uint32
	{
		AtmoRoot_Constants     = 0, // b0
		AtmoRoot_Pass          = 1, // b1 (상수 4개)
		AtmoRoot_Transmittance = 2, // t0
		AtmoRoot_MultiScatter  = 3, // t1
		AtmoRoot_SkyView       = 4, // t2
		AtmoRoot_CloudCube     = 5, // t3
		AtmoRoot_Output        = 6, // u0
		AtmoRoot_CubeOutput    = 7, // u1
	};
	enum EIblRoot : uint32
	{
		IblRoot_Constants = 0, // b0 (상수 4개: Size, SampleCount, Roughness, Rotation)
		IblRoot_Source    = 1, // t0 (하늘 큐브)
		IblRoot_Output    = 2, // u0
		IblRoot_MipSource = 3, // u2
	};

	struct FPassConstants
	{
		uint32 Size        = 0;
		uint32 Height      = 0;
		uint32 SampleCount = 0;
		float  CloudWeight = 0.0f;
	};
	static_assert(sizeof(FPassConstants) == 16);

	struct FIblBakeConstants // Ibl.hlsl BakeConstants
	{
		uint32 Size        = 0;
		uint32 SampleCount = 0;
		float  Roughness   = 0.0f;
		float  Rotation    = 0.0f;
	};
	static_assert(sizeof(FIblBakeConstants) == 16);

	constexpr DXGI_FORMAT LutFormat       = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr uint32      SkyViewSamples  = 30;
	constexpr float       SunDiskBaseScale = 0.002f; // 물리 원반 휘도(조도 / 입체각)에 곱하는 기본값 — 블룸이 화면을 덮지 않게
	constexpr float       SunMoveThresholdCos = 0.99999962f; // cos(0.05도)
	constexpr float       HeightThresholdKm   = 0.05f;

	FShaderCompileDesc MakeDesc(const wchar_t* File, const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = File;
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}

	bool SameParams(const FAtmosphereMath::FParams& A, const FAtmosphereMath::FParams& B)
	{
		return std::memcmp(&A, &B, sizeof(FAtmosphereMath::FParams)) == 0;
	}

	FAtmosphereMath::FParams MakeParams(const FSkyAtmosphereComponent& Component)
	{
		FAtmosphereMath::FParams Params;
		Params.BottomRadius        = FMath::Max(Component.PlanetRadius, 1.0f);
		Params.TopRadius           = Params.BottomRadius + FMath::Max(Component.AtmosphereHeight, 1.0f);
		Params.RayleighScattering  = Component.RayleighScatteringColor * FMath::Max(Component.RayleighScatteringScale, 0.0f);
		Params.RayleighScaleHeight = FMath::Max(Component.RayleighScaleHeight, 0.01f);
		Params.MieScattering       = Component.MieScatteringColor * FMath::Max(Component.MieScatteringScale, 0.0f);
		Params.MieAbsorption       = Component.MieAbsorptionColor * FMath::Max(Component.MieAbsorptionScale, 0.0f);
		Params.MieScaleHeight      = FMath::Max(Component.MieScaleHeight, 0.01f);
		Params.MieAnisotropy       = FMath::Clamp(Component.MieAnisotropy, 0.0f, 0.999f);
		Params.OzoneAbsorption     = Component.OzoneAbsorptionColor * FMath::Max(Component.OzoneAbsorptionScale, 0.0f);
		Params.GroundAlbedo        = Component.GroundAlbedo;
		return Params;
	}
} // namespace

FSkyAtmosphereRenderer::~FSkyAtmosphereRenderer()
{
	Shutdown();
}

bool FSkyAtmosphereRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	const auto Table = [](D3D12_DESCRIPTOR_RANGE_TYPE Type, uint32 Register) {
		return std::vector<D3D12_DESCRIPTOR_RANGE1>{ FD3D12RootSignature::MakeRange(Type, 1, Register, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) };
	};
	const uint32 ConstantsIndex = RootSignature.AddConstantBufferView(0);
	const uint32 PassIndex      = RootSignature.AddConstants(4, 1);
	const uint32 T0             = RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0));
	const uint32 T1             = RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1));
	const uint32 T2             = RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2));
	const uint32 T3             = RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3));
	const uint32 U0             = RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0));
	const uint32 U1             = RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1));
	E_CHECK(ConstantsIndex == AtmoRoot_Constants && PassIndex == AtmoRoot_Pass && T0 == AtmoRoot_Transmittance && T1 == AtmoRoot_MultiScatter &&
	        T2 == AtmoRoot_SkyView && T3 == AtmoRoot_CloudCube && U0 == AtmoRoot_Output && U1 == AtmoRoot_CubeOutput);
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	if (!RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"SkyAtmosphereRoot"))
	{
		return false;
	}

	const uint32 IblConstants = IblRoot.AddConstants(4, 0);
	const uint32 IblSource    = IblRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0));
	const uint32 IblOutput    = IblRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0));
	const uint32 IblMip       = IblRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2));
	E_CHECK(IblConstants == IblRoot_Constants && IblSource == IblRoot_Source && IblOutput == IblRoot_Output && IblMip == IblRoot_MipSource);
	IblRoot.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	IblRoot.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_ALL));
	if (!IblRoot.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"SkyAtmosphereIblRoot"))
	{
		return false;
	}
	return CreatePipelines(false);
}

bool FSkyAtmosphereRenderer::CreatePipelines(bool bForceRecompile)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	const auto    Load   = [&](const wchar_t* File, const wchar_t* Entry, EShaderStage Stage) {
        const FShaderCompileDesc Desc = MakeDesc(File, Entry, Stage);
        if (bForceRecompile && !Library->CookShader(Desc))
        {
            return ComPtr<IDxcBlob>();
        }
        return Library->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> Transmittance = Load(L"SkyAtmosphere.hlsl", L"TransmittanceLutCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> Multi         = Load(L"SkyAtmosphere.hlsl", L"MultiScatteringLutCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> SkyView       = Load(L"SkyAtmosphere.hlsl", L"SkyViewLutCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> SkyCubeCs     = Load(L"SkyAtmosphere.hlsl", L"SkyCubeCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> SkyVs         = Load(L"SkyAtmosphere.hlsl", L"VSSky", EShaderStage::Vertex);
	const ComPtr<IDxcBlob> SkyPs         = Load(L"SkyAtmosphere.hlsl", L"PSSky", EShaderStage::Pixel);
	const ComPtr<IDxcBlob> Downsample    = Load(L"Ibl.hlsl", L"DownsampleCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> IrradianceCs  = Load(L"Ibl.hlsl", L"IrradianceCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> PrefilterCs   = Load(L"Ibl.hlsl", L"PrefilterCS", EShaderStage::Compute);
	if (!Transmittance || !Multi || !SkyView || !SkyCubeCs || !SkyVs || !SkyPs || !Downsample || !IrradianceCs || !PrefilterCs)
	{
		return false;
	}
	FD3D12PipelineState NewPipelines[8];
	FGraphicsPipelineDesc SkyDesc;
	SkyDesc.RootSignature          = RootSignature.Get();
	SkyDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(SkyVs.Get());
	SkyDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(SkyPs.Get());
	SkyDesc.RenderTargetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	SkyDesc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
	SkyDesc.bDepthEnable           = true;
	SkyDesc.bDepthWrite            = false;
	SkyDesc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	SkyDesc.CullMode               = D3D12_CULL_MODE_NONE;
	const bool bOk =
		NewPipelines[0].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Transmittance.Get()), L"AtmosphereTransmittanceLut") &&
		NewPipelines[1].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Multi.Get()), L"AtmosphereMultiScatteringLut") &&
		NewPipelines[2].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(SkyView.Get()), L"AtmosphereSkyViewLut") &&
		NewPipelines[3].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(SkyCubeCs.Get()), L"AtmosphereSkyCube") &&
		NewPipelines[4].InitGraphics(Device, SkyDesc, L"AtmosphereSkyPipeline") &&
		NewPipelines[5].InitCompute(Device, IblRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Downsample.Get()), L"AtmosphereIblDownsample") &&
		NewPipelines[6].InitCompute(Device, IblRoot.Get(), FD3D12ShaderCompiler::ToBytecode(IrradianceCs.Get()), L"AtmosphereIblIrradiance") &&
		NewPipelines[7].InitCompute(Device, IblRoot.Get(), FD3D12ShaderCompiler::ToBytecode(PrefilterCs.Get()), L"AtmosphereIblPrefilter");
	if (!bOk)
	{
		return false;
	}
	FD3D12PipelineState* Targets[8] = { &TransmittancePipeline, &MultiScatteringPipeline, &SkyViewPipeline, &SkyCubePipeline,
		                                 &SkyPipeline, &DownsamplePipeline, &IrradiancePipeline, &PrefilterPipeline };
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		Targets[Index]->Swap(NewPipelines[Index]);
		if (NewPipelines[Index].IsInitialized())
		{
			Rhi->DeferRelease(NewPipelines[Index].Detach());
		}
	}
	return true;
}

bool FSkyAtmosphereRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return false;
	}
	if (!CreatePipelines(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "대기 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	bLutsValid        = false;
	bEnvironmentDirty = true;
	return true;
}

void FSkyAtmosphereRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (FPersistentTexture* Texture : { &TransmittanceLut, &MultiScatteringLut, &SkyViewLut, &SkyCube, &Irradiance[0], &Irradiance[1], &Prefilter[0],
	                                     &Prefilter[1], &DummyCloudCube })
	{
		Texture->Release(*Rhi);
	}
	for (FD3D12DescriptorHandle& Handle : LightingTables)
	{
		if (Handle.IsValid())
		{
			Rhi->DeferFreeDescriptor(Handle);
			Handle = FD3D12DescriptorHandle{};
		}
	}
	TransmittancePipeline.Shutdown();
	MultiScatteringPipeline.Shutdown();
	SkyViewPipeline.Shutdown();
	SkyCubePipeline.Shutdown();
	SkyPipeline.Shutdown();
	DownsamplePipeline.Shutdown();
	IrradiancePipeline.Shutdown();
	PrefilterPipeline.Shutdown();
	RootSignature.Shutdown();
	IblRoot.Shutdown();
	Rhi     = nullptr;
	Library = nullptr;
}

void FSkyAtmosphereRenderer::EnsureResources()
{
	FPersistentTextureDesc Desc;
	Desc.Format = LutFormat;
	Desc.Width  = FAtmosphereMath::TransmittanceLutWidth;
	Desc.Height = FAtmosphereMath::TransmittanceLutHeight;
	if (!TransmittanceLut.Matches(Desc) || !TransmittanceLut.IsValid())
	{
		bLutsValid = false;
	}
	TransmittanceLut.Ensure(*Rhi, Desc, L"AtmosphereTransmittanceLut");
	Desc.Width = Desc.Height = FAtmosphereMath::MultiScatteringLutSize;
	MultiScatteringLut.Ensure(*Rhi, Desc, L"AtmosphereMultiScatteringLut");
	Desc.Width  = FAtmosphereMath::SkyViewLutWidth;
	Desc.Height = FAtmosphereMath::SkyViewLutHeight;
	SkyViewLut.Ensure(*Rhi, Desc, L"AtmosphereSkyViewLut");

	FPersistentTextureDesc CubeDesc;
	CubeDesc.Format   = LutFormat;
	CubeDesc.bCube    = true;
	CubeDesc.Depth    = 6;
	CubeDesc.Width    = CubeDesc.Height = IblMath::SkyCubeSize;
	CubeDesc.MipCount = IblMath::GetFullMipCount(IblMath::SkyCubeSize);
	SkyCube.Ensure(*Rhi, CubeDesc, L"AtmosphereSkyCube");
	CubeDesc.Width = CubeDesc.Height = 1;
	CubeDesc.MipCount = 1;
	CubeDesc.bUnorderedAccess = false;
	DummyCloudCube.Ensure(*Rhi, CubeDesc, L"AtmosphereDummyCloudCube");
	CubeDesc.bUnorderedAccess = true;
	const bool bHadTables = Irradiance[0].IsValid();
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		CubeDesc.Width = CubeDesc.Height = IblMath::IrradianceSize;
		CubeDesc.MipCount = 1;
		Irradiance[Index].Ensure(*Rhi, CubeDesc, Index == 0 ? L"AtmosphereIrradiance0" : L"AtmosphereIrradiance1");
		CubeDesc.Width = CubeDesc.Height = IblMath::PrefilterSize;
		CubeDesc.MipCount = IblMath::PrefilterMipCount;
		Prefilter[Index].Ensure(*Rhi, CubeDesc, Index == 0 ? L"AtmospherePrefilter0" : L"AtmospherePrefilter1");
	}
	if (!bHadTables)
	{
		AllocateTables();
	}
}

void FSkyAtmosphereRenderer::AllocateTables()
{
	ID3D12Device*              Device    = Rhi->GetDevice().GetDevice();
	FD3D12DescriptorAllocator& Allocator = Rhi->GetSrvAllocator();
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		if (!LightingTables[Index].IsValid())
		{
			LightingTables[Index] = Allocator.AllocateRange(3);
		}
		D3D12_SHADER_RESOURCE_VIEW_DESC View{};
		View.Format                  = LutFormat;
		View.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		View.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
		View.TextureCube.MipLevels   = 1;
		Device->CreateShaderResourceView(Irradiance[Index].Resource.Get(), &View, Allocator.GetCpuHandle(LightingTables[Index].Index));
		View.TextureCube.MipLevels = IblMath::PrefilterMipCount;
		Device->CreateShaderResourceView(Prefilter[Index].Resource.Get(), &View, Allocator.GetCpuHandle(LightingTables[Index].Index + 1));
	}
	TableBrdfSource = nullptr; // BRDF 칸은 ApplyEnvironmentOverride가 채운다
}

bool FSkyAtmosphereRenderer::Prepare(FScene& Scene, const FCamera& Camera, bool bEnabled)
{
	const FSkyAtmosphereComponent* Component = nullptr;
	Scene.GetRegistry().View<FSkyAtmosphereComponent>().Each([&](FEntity, FSkyAtmosphereComponent& Atmosphere) {
		if (Component == nullptr)
		{
			Component = &Atmosphere;
		}
	});
	ConstantsAddress = 0;
	if (!bEnabled || Component == nullptr)
	{
		bActive          = false;
		EnvironmentStage = -1;
		return false;
	}
	bActive = true;
	EnsureResources();

	// 태양 = 씬의 첫 방향광 (FSceneRenderer::BuildPerFrameConstants와 같은 규칙, 없으면 같은 기본값)
	FVector3 LightDirection = FVector3(1.0f, 0.5f, -1.0f).GetNormalized();
	FVector3 Illuminance    = FVector3::OneVector * 3.0f;
	bool     bFoundLight    = false;
	Scene.GetRegistry().View<FTransformComponent, FDirectionalLightComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FDirectionalLightComponent& Light) {
			if (!bFoundLight)
			{
				LightDirection = Transform.GetWorldForward();
				Illuminance    = Light.Color * Light.Intensity;
				bFoundLight    = true;
			}
		});
	const FVector3 SunDirection = (-LightDirection).GetNormalized();

	// 끝난 IBL 갱신은 이번 렌더부터 앞쪽으로
	if (bPublishPending)
	{
		BackIndex       = 1 - BackIndex;
		bHasFront       = true;
		bPublishPending = false;
	}

	Params               = MakeParams(*Component);
	bRealtimeEnvironment = Component->bRealtimeEnvironmentLighting;
	BuildConstants(*Component, Camera, SunDirection, Illuminance);
	ConstantsAddress = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
	return true;
}

void FSkyAtmosphereRenderer::BuildConstants(const FSkyAtmosphereComponent& Component, const FCamera& Camera, const FVector3& SunDirection,
                                            const FVector3& SunIlluminance)
{
	const float SkyScale = FMath::Max(Component.SkyLuminanceScale, 0.0f);
	Constants                         = FAtmosphereConstants{};
	Constants.RayleighScattering      = Params.RayleighScattering;
	Constants.BottomRadius            = Params.BottomRadius;
	Constants.MieScattering           = Params.MieScattering;
	Constants.TopRadius               = Params.TopRadius;
	Constants.MieExtinction           = Params.GetMieExtinction();
	Constants.RayleighDensityExpScale = -1.0f / Params.RayleighScaleHeight;
	Constants.MieAbsorption           = Params.MieAbsorption;
	Constants.MieDensityExpScale      = -1.0f / Params.MieScaleHeight;
	Constants.OzoneAbsorption         = Params.OzoneAbsorption;
	Constants.MiePhaseG               = Params.MieAnisotropy;
	Constants.GroundAlbedo            = Params.GroundAlbedo;
	Constants.OzoneCenterHeight       = Params.OzoneCenterHeight;
	Constants.SunDirection            = SunDirection;
	Constants.OzoneHalfWidth          = Params.OzoneHalfWidth;
	Constants.SunIlluminance          = SunIlluminance * SkyScale;
	const float HalfAngle             = FMath::DegreesToRadians(FMath::Max(Component.SunDiskSize, 0.0f) * 0.5f);
	Constants.SunDiskCosHalfAngle     = FMath::Cos(HalfAngle);
	Constants.CameraPosition          = FAtmosphereMath::GetCameraAtmospherePosition(Params, Camera.GetPosition());
	const float SolidAngle            = 2.0f * FMath::Pi * (1.0f - Constants.SunDiskCosHalfAngle);
	// 원반 휘도 = 조도 / 입체각 × 기본 배율 × 사용자 배율 (하늘 밝기 배율과 무관하게 — SunIlluminance에 들어간 배율을 나눈다)
	Constants.SunDiskLuminance        = (HalfAngle > 0.0f && SolidAngle > 1.0e-12f && SkyScale > 0.0f)
	                                        ? FMath::Max(Component.SunDiskIntensity, 0.0f) * SunDiskBaseScale / SolidAngle / SkyScale
	                                        : 0.0f;
	Constants.NightSkyLuminance       = Component.NightSkyColor * SkyScale;
	Constants.StarIntensity           = FMath::Max(Component.StarIntensity, 0.0f);
	Constants.SkyCameraForward        = Camera.GetForwardVector();
	Constants.SkyTanHalfFov           = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	Constants.SkyCameraRight          = Camera.GetRightVector();
	Constants.SkyAspect               = Camera.GetAspectRatio();
	Constants.SkyCameraUp             = Camera.GetUpVector();
	Constants.bOrthographic           = Camera.IsOrthographic() ? 1u : 0u;

	// 태양 투과율 (카메라 위치 → 태양, CPU 수치 적분 = 투과율 LUT 한 칸과 같은 식). 지평선에 원반이 걸치면 보이는 비율만큼
	const float    CameraRadius = Constants.CameraPosition.Length();
	const FVector3 Up           = Constants.CameraPosition / CameraRadius;
	const float    SunCos       = FVector3::Dot(SunDirection, Up);
	const FVector3 Transmittance = FAtmosphereMath::ComputeTransmittanceToTop(Params, CameraRadius, SunCos);
	const float    HorizonCos   = -FMath::Sqrt(FMath::Max(1.0f - FMath::Square(Params.BottomRadius / CameraRadius), 0.0f));
	const float    DiskSin      = FMath::Max(FMath::Sin(FMath::Max(HalfAngle, FMath::DegreesToRadians(0.1f))), 1.0e-4f);
	const float    Visible      = FMath::Clamp((SunCos - HorizonCos) / (2.0f * DiskSin) + 0.5f, 0.0f, 1.0f);
	const FVector3 SunTransmittance = Transmittance * Visible;
	SunLightTransmittance = Component.bAffectSunLight ? SunTransmittance : FVector3::OneVector;

	// 달 (태양 반대편): 태양이 지평선 아래 2도에서 8도로 내려가는 동안 달빛이 켜진다. 태양 빛이 완전히 0일 때만 방향광을 바꾼다 (튐 없음)
	const FVector3 MoonDirection = -SunDirection;
	const float    MoonSunSin    = FMath::Clamp((SunCos - FMath::Sin(FMath::DegreesToRadians(-2.0f))) /
	                                                (FMath::Sin(FMath::DegreesToRadians(-8.0f)) - FMath::Sin(FMath::DegreesToRadians(-2.0f))),
	                                            0.0f, 1.0f);
	const float    MoonFactor    = MoonSunSin * MoonSunSin * (3.0f - 2.0f * MoonSunSin);
	const float    MoonCos       = FVector3::Dot(MoonDirection, Up);
	const FVector3 MoonTransmittance = FAtmosphereMath::ComputeTransmittanceToTop(Params, CameraRadius, MoonCos) *
	                                   FMath::Clamp((MoonCos - HorizonCos) / (2.0f * DiskSin) + 0.5f, 0.0f, 1.0f);
	Constants.MoonDirection   = MoonDirection;
	Constants.MoonIlluminance = Component.MoonColor * (FMath::Max(Component.MoonIntensity, 0.0f) * MoonFactor) * MoonTransmittance;
	Constants.MoonDiskLuminance = (SolidAngle > 1.0e-12f && HalfAngle > 0.0f) ? SunDiskBaseScale / SolidAngle : 0.0f;
	bUseMoonLight = Component.bAffectSunLight && Visible <= 0.0f && MoonFactor > 0.0f && Component.MoonIntensity > 0.0f;

	// 공중 원근 (평평한 지면 지수 대기, cm 단위) + 다중 산란 (CPU, 태양 각·고도가 바뀔 때만)
	const float Height = CameraRadius - Params.BottomRadius;
	if (FMath::Abs(SunCos - CachedMsSunCos) > 0.002f || FMath::Abs(Height - CachedMsHeight) > 0.05f || !SameParams(Params, CachedMsParams))
	{
		CachedMultiScattering = FAtmosphereMath::ComputeMultipleScattering(Params, Height, SunCos);
		CachedMsSunCos        = SunCos;
		CachedMsHeight        = Height;
		CachedMsParams        = Params;
	}
	constexpr float PerKmToPerCm      = FAtmosphereMath::CentimetersToKilometers;
	AerialParams                      = FAtmosphereMath::FAerialParams{};
	AerialParams.RayleighScattering   = Params.RayleighScattering * PerKmToPerCm;
	AerialParams.RayleighScaleHeight  = Params.RayleighScaleHeight * FAtmosphereMath::KilometersToCentimeters;
	AerialParams.MieScattering        = Params.MieScattering * PerKmToPerCm;
	AerialParams.MieScaleHeight       = Params.MieScaleHeight * FAtmosphereMath::KilometersToCentimeters;
	AerialParams.MieExtinction        = Params.GetMieExtinction() * PerKmToPerCm;
	AerialParams.MieAnisotropy        = Params.MieAnisotropy;
	AerialParams.SunIlluminance       = SunIlluminance * SunTransmittance * SkyScale;
	AerialParams.SunDirection         = SunDirection;
	AerialParams.MultiScattering      = CachedMultiScattering * SunIlluminance * SkyScale;
	AerialParams.GroundHeight         = 0.0f;
	AerialParams.DistanceScale        = FMath::Max(Component.AerialPerspectiveScale, 0.0f);
	bAerialEnabled                    = AerialParams.DistanceScale > 0.0f;
}

void FSkyAtmosphereRenderer::AddLutPasses(FRenderGraph& Graph, ERGQueue Queue, int32 Timer)
{
	TransmittanceRef   = FRGResourceRef{};
	MultiScatteringRef = FRGResourceRef{};
	SkyViewRef         = FRGResourceRef{};
	if (!bActive)
	{
		return;
	}
	TransmittanceRef   = TransmittanceLut.Import(Graph, "AtmosphereTransmittanceLut");
	MultiScatteringRef = MultiScatteringLut.Import(Graph, "AtmosphereMultiScatteringLut");
	SkyViewRef         = SkyViewLut.Import(Graph, "AtmosphereSkyViewLut");
	const D3D12_GPU_VIRTUAL_ADDRESS Address = ConstantsAddress;

	if (!bLutsValid || !SameParams(Params, LutParams))
	{
		bLutsValid = true;
		LutParams  = Params;
		bEnvironmentDirty = true;
		Graph.AddPass("대기 투과율 LUT", Queue)
			.Write(TransmittanceRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, Address](FRGContext& Context) {
				ID3D12GraphicsCommandList* List = Context.CommandList;
				const FPassConstants       Pass{ FAtmosphereMath::TransmittanceLutWidth, FAtmosphereMath::TransmittanceLutHeight, 0, 0.0f };
				List->SetComputeRootSignature(RootSignature.Get());
				List->SetPipelineState(TransmittancePipeline.Get());
				List->SetComputeRootConstantBufferView(AtmoRoot_Constants, Address);
				List->SetComputeRoot32BitConstants(AtmoRoot_Pass, 4, &Pass, 0);
				List->SetComputeRootDescriptorTable(AtmoRoot_Output, TransmittanceLut.Uavs[0].Gpu);
				List->Dispatch((Pass.Size + 7) / 8, (Pass.Height + 7) / 8, 1);
			});
		Graph.AddPass("대기 다중 산란 LUT", Queue)
			.Read(TransmittanceRef, ERGAccess::SrvNonPixel)
			.Write(MultiScatteringRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, Address](FRGContext& Context) {
				ID3D12GraphicsCommandList* List = Context.CommandList;
				const FPassConstants       Pass{ FAtmosphereMath::MultiScatteringLutSize, FAtmosphereMath::MultiScatteringLutSize, 0, 0.0f };
				List->SetComputeRootSignature(RootSignature.Get());
				List->SetPipelineState(MultiScatteringPipeline.Get());
				List->SetComputeRootConstantBufferView(AtmoRoot_Constants, Address);
				List->SetComputeRoot32BitConstants(AtmoRoot_Pass, 4, &Pass, 0);
				List->SetComputeRootDescriptorTable(AtmoRoot_Transmittance, TransmittanceLut.Srv.Gpu);
				List->SetComputeRootDescriptorTable(AtmoRoot_Output, MultiScatteringLut.Uavs[0].Gpu);
				List->Dispatch((Pass.Size + 7) / 8, (Pass.Height + 7) / 8, 1);
			});
	}
	Graph.AddPass("대기 하늘 뷰 LUT", Queue)
		.Read(TransmittanceRef, ERGAccess::SrvNonPixel)
		.Read(MultiScatteringRef, ERGAccess::SrvNonPixel)
		.Write(SkyViewRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, Address](FRGContext& Context) {
			ID3D12GraphicsCommandList* List = Context.CommandList;
			const FPassConstants       Pass{ FAtmosphereMath::SkyViewLutWidth, FAtmosphereMath::SkyViewLutHeight, SkyViewSamples, 0.0f };
			List->SetComputeRootSignature(RootSignature.Get());
			List->SetPipelineState(SkyViewPipeline.Get());
			List->SetComputeRootConstantBufferView(AtmoRoot_Constants, Address);
			List->SetComputeRoot32BitConstants(AtmoRoot_Pass, 4, &Pass, 0);
			List->SetComputeRootDescriptorTable(AtmoRoot_Transmittance, TransmittanceLut.Srv.Gpu);
			List->SetComputeRootDescriptorTable(AtmoRoot_MultiScatter, MultiScatteringLut.Srv.Gpu);
			List->SetComputeRootDescriptorTable(AtmoRoot_Output, SkyViewLut.Uavs[0].Gpu);
			List->Dispatch((Pass.Size + 7) / 8, (Pass.Height + 7) / 8, 1);
		});
}

void FSkyAtmosphereRenderer::DeclareSkyReads(FRenderGraph::FPassBuilder& Pass) const
{
	if (!bActive)
	{
		return;
	}
	Pass.Read(TransmittanceRef, ERGAccess::SrvPixel).Read(SkyViewRef, ERGAccess::SrvPixel);
}

void FSkyAtmosphereRenderer::RenderSky(ID3D12GraphicsCommandList* CommandList) const
{
	if (!bActive)
	{
		return;
	}
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(SkyPipeline.Get());
	CommandList->SetGraphicsRootConstantBufferView(AtmoRoot_Constants, ConstantsAddress);
	CommandList->SetGraphicsRootDescriptorTable(AtmoRoot_Transmittance, TransmittanceLut.Srv.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(AtmoRoot_SkyView, SkyViewLut.Srv.Gpu);
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->DrawInstanced(3, 1, 0, 0);
}

void FSkyAtmosphereRenderer::AddEnvironmentPasses(FRenderGraph& Graph, FRGResourceRef CloudCube, const FD3D12DescriptorHandle& CloudCubeSrv,
                                                  bool bCloudsChanging, bool bAllowUpdate, int32 Timer)
{
	if (!bActive || !bRealtimeEnvironment)
	{
		EnvironmentStage = -1;
		return;
	}
	if (!bAllowUpdate)
	{
		return; // 진행 중 단계는 다음 허용 렌더에서 이어간다
	}
	const float Height = Constants.CameraPosition.Length() - Params.BottomRadius;
	if (EnvironmentStage < 0)
	{
		const bool bChanged = bEnvironmentDirty || !bHasFront || bCloudsChanging ||
		                      FVector3::Dot(Constants.SunDirection, PublishedSunDirection) < SunMoveThresholdCos ||
		                      FMath::Abs(Height - PublishedHeight) > HeightThresholdKm || !SameParams(Params, PublishedParams) ||
		                      !(Constants.SunIlluminance == PublishedSunIlluminance);
		if (!bChanged || bPublishPending)
		{
			return;
		}
		EnvironmentStage         = 0;
		bEnvironmentDirty        = false;
		PublishedSunDirection    = Constants.SunDirection;
		PublishedHeight          = Height;
		PublishedParams          = Params;
		PublishedSunIlluminance  = Constants.SunIlluminance;
	}

	const uint32         Back          = BackIndex;
	const FRGResourceRef SkyCubeRef    = SkyCube.Import(Graph, "AtmosphereSkyCube");
	const FRGResourceRef IrradianceRef = Graph.Import("AtmosphereIrradiance", Irradiance[Back].Resource.Get(),
	                                                  RenderGraphD3D12::FromD3D12(Irradiance[Back].State), ERGAccess::SrvPixel, 1, 6);
	const FRGResourceRef PrefilterRef  = Graph.Import("AtmospherePrefilter", Prefilter[Back].Resource.Get(), RenderGraphD3D12::FromD3D12(Prefilter[Back].State),
	                                                  ERGAccess::SrvPixel, IblMath::PrefilterMipCount, 6);
	Irradiance[Back].State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // 그래프 끝 상태 (위 FinalState)
	Prefilter[Back].State  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	const uint32 SampleCount = static_cast<uint32>(FMath::Clamp(RendererCVars::SkyAtmosphereIblSamples.Get(), 8, 1024));
	const bool   bClouds     = CloudCube.IsValid();
	const FRGResourceRef CloudRef = bClouds ? CloudCube : DummyCloudCube.Import(Graph, "AtmosphereDummyCloudCube");
	const FD3D12DescriptorHandle CloudSrv = bClouds ? CloudCubeSrv : DummyCloudCube.Srv;
	const D3D12_GPU_VIRTUAL_ADDRESS Address = ConstantsAddress;

	// Ibl.hlsl 적분 디스패치. Source/MipSource는 셰이더가 읽을 때만 (ptr 0이면 묶지 않음 — 다른 상태의 리소스를 표에 남기지 않는다)
	const auto Dispatch = [this](ID3D12GraphicsCommandList* List, const FD3D12PipelineState& Pipeline, uint32 Size, uint32 Samples, float Roughness,
	                             D3D12_GPU_DESCRIPTOR_HANDLE Source, D3D12_GPU_DESCRIPTOR_HANDLE Output, D3D12_GPU_DESCRIPTOR_HANDLE MipSource) {
		const FIblBakeConstants Bake{ Size, Samples, Roughness, 0.0f };
		List->SetComputeRootSignature(IblRoot.Get());
		List->SetPipelineState(Pipeline.Get());
		List->SetComputeRoot32BitConstants(IblRoot_Constants, 4, &Bake, 0);
		if (Source.ptr != 0)
		{
			List->SetComputeRootDescriptorTable(IblRoot_Source, Source);
		}
		if (MipSource.ptr != 0)
		{
			List->SetComputeRootDescriptorTable(IblRoot_MipSource, MipSource);
		}
		List->SetComputeRootDescriptorTable(IblRoot_Output, Output);
		List->Dispatch((Size + 7) / 8, (Size + 7) / 8, 6);
	};
	const D3D12_GPU_DESCRIPTOR_HANDLE NoTable{};

	// 첫 결과가 없으면 세 단계를 한 번에 (시작 프레임부터 대기 조명)
	const int32 LastStage = bHasFront ? EnvironmentStage : 2;
	for (int32 Stage = EnvironmentStage; Stage <= LastStage; ++Stage)
	{
		if (Stage == 0)
		{
			Graph.AddPass("대기 하늘 큐브")
				.Read(TransmittanceRef, ERGAccess::SrvNonPixel)
				.Read(MultiScatteringRef, ERGAccess::SrvNonPixel)
				.Read(SkyViewRef, ERGAccess::SrvNonPixel)
				.Read(CloudRef, ERGAccess::SrvNonPixel)
				.Write(SkyCubeRef, ERGAccess::Uav, FRGSubresourceRange::Mip(0), true)
				.Timer(Timer)
				.Execute([this, Address, CloudSrv, bClouds](FRGContext& Context) {
					ID3D12GraphicsCommandList* List = Context.CommandList;
					const FPassConstants       Pass{ IblMath::SkyCubeSize, IblMath::SkyCubeSize, 0, bClouds ? 1.0f : 0.0f };
					List->SetComputeRootSignature(RootSignature.Get());
					List->SetPipelineState(SkyCubePipeline.Get());
					List->SetComputeRootConstantBufferView(AtmoRoot_Constants, Address);
					List->SetComputeRoot32BitConstants(AtmoRoot_Pass, 4, &Pass, 0);
					List->SetComputeRootDescriptorTable(AtmoRoot_Transmittance, TransmittanceLut.Srv.Gpu);
					List->SetComputeRootDescriptorTable(AtmoRoot_MultiScatter, MultiScatteringLut.Srv.Gpu);
					List->SetComputeRootDescriptorTable(AtmoRoot_SkyView, SkyViewLut.Srv.Gpu);
					List->SetComputeRootDescriptorTable(AtmoRoot_CloudCube, CloudSrv.Gpu);
					List->SetComputeRootDescriptorTable(AtmoRoot_CubeOutput, SkyCube.Uavs[0].Gpu);
					List->Dispatch((Pass.Size + 7) / 8, (Pass.Size + 7) / 8, 6);
				});
			for (uint32 Mip = 1; Mip < SkyCube.Desc.MipCount; ++Mip)
			{
				Graph.AddPass("대기 하늘 큐브 밉")
					.Write(SkyCubeRef, ERGAccess::Uav, FRGSubresourceRange::Mip(Mip - 1))
					.Write(SkyCubeRef, ERGAccess::Uav, FRGSubresourceRange::Mip(Mip), true)
					.Timer(Timer)
					.Execute([this, Dispatch, NoTable, Mip](FRGContext& Context) {
						Dispatch(Context.CommandList, DownsamplePipeline, FMath::Max(IblMath::SkyCubeSize >> Mip, 1u), 0, 0.0f, NoTable, SkyCube.Uavs[Mip].Gpu,
						         SkyCube.Uavs[Mip - 1].Gpu);
					});
			}
			Graph.AddPass("대기 IBL 조도")
				.Read(SkyCubeRef, ERGAccess::SrvNonPixel)
				.Write(IrradianceRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
				.Timer(Timer)
				.Execute([this, Dispatch, NoTable, Back, SampleCount](FRGContext& Context) {
					Dispatch(Context.CommandList, IrradiancePipeline, IblMath::IrradianceSize, SampleCount, 0.0f, SkyCube.Srv.Gpu, Irradiance[Back].Uavs[0].Gpu,
					         NoTable);
				});
		}
		else
		{
			const uint32 FirstMip = Stage == 1 ? 0u : 3u;
			const uint32 EndMip   = Stage == 1 ? 3u : IblMath::PrefilterMipCount;
			for (uint32 Mip = FirstMip; Mip < EndMip; ++Mip)
			{
				Graph.AddPass("대기 IBL 프리필터")
					.Read(SkyCubeRef, ERGAccess::SrvNonPixel)
					.Write(PrefilterRef, ERGAccess::Uav, FRGSubresourceRange::Mip(Mip), true)
					.Timer(Timer)
					.Execute([this, Dispatch, NoTable, Back, SampleCount, Mip](FRGContext& Context) {
						Dispatch(Context.CommandList, PrefilterPipeline, IblMath::PrefilterSize >> Mip, SampleCount,
						         IblMath::MipToRoughness(Mip, IblMath::PrefilterMipCount), SkyCube.Srv.Gpu, Prefilter[Back].Uavs[Mip].Gpu, NoTable);
					});
			}
		}
	}
	EnvironmentStage = LastStage + 1;
	if (EnvironmentStage >= 3)
	{
		EnvironmentStage = -1;
		bPublishPending  = true;
	}
}

void FSkyAtmosphereRenderer::ApplyEnvironmentOverride(FIblRenderer& Ibl) const
{
	if (!bActive || !bRealtimeEnvironment || !bHasFront)
	{
		Ibl.SetLightingOverride(nullptr);
		return;
	}
	// BRDF LUT 칸은 FIblRenderer 것을 그대로 (처음 한 번 — 이후 같은 리소스)
	if (ID3D12Resource* Brdf = Ibl.GetBrdfLut(); Brdf != TableBrdfSource && Brdf != nullptr)
	{
		auto* Self = const_cast<FSkyAtmosphereRenderer*>(this);
		D3D12_SHADER_RESOURCE_VIEW_DESC View{};
		View.Format                  = LutFormat;
		View.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		View.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
		View.Texture2D.MipLevels     = 1;
		for (const FD3D12DescriptorHandle& Table : LightingTables)
		{
			Rhi->GetDevice().GetDevice()->CreateShaderResourceView(Brdf, &View, Rhi->GetSrvAllocator().GetCpuHandle(Table.Index + 2));
		}
		Self->TableBrdfSource = Brdf;
	}
	Ibl.SetLightingOverride(&LightingTables[1 - BackIndex]);
}

void FSkyAtmosphereRenderer::ApplyToDirectionalLight(FDirectionalLightConstants& Light) const
{
	if (!bActive)
	{
		return;
	}
	if (bUseMoonLight)
	{
		Light.Direction = -Constants.MoonDirection;
		Light.Color     = Constants.MoonIlluminance;
		Light.Intensity = 1.0f;
		return;
	}
	Light.Color = Light.Color * SunLightTransmittance; // 해질녘 붉은 빛, 지평선 아래 0
}