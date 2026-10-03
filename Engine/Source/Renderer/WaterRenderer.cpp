#include "Renderer/WaterRenderer.h"

#include "Core/Math/Frustum.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Renderer/ScreenPass.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SkyAtmosphere.h"

#include <cmath>
#include <random>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum EWaterRoot : uint32
	{
		WaterRoot_Frame        = 0, // b0
		WaterRoot_Body         = 1, // b1
		WaterRoot_Fog          = 2, // b2
		WaterRoot_Shadow       = 3, // b3
		WaterRoot_Copy         = 4, // t0
		WaterRoot_Depth        = 5, // t1
		WaterRoot_Waves        = 6, // t2
		WaterRoot_Ibl          = 7, // t4~t6
		WaterRoot_ShadowMap    = 8, // t7
		WaterRoot_FogVolume    = 9, // t8
		WaterRoot_Captures     = 10, // t9 (루트 SRV)
		WaterRoot_CaptureAtlas = 11, // t10
	};

	constexpr uint32 WaveTextureSize = 256;

	FShaderCompileDesc MakeDesc(const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"Water.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}
} // namespace

FWaterRenderer::~FWaterRenderer()
{
	Shutdown();
}

bool FWaterRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi       = &InRhi;
	Library   = &InLibrary;
	StartTime = std::chrono::steady_clock::now();
	const auto Table = [](uint32 Count, uint32 Register) {
		return std::vector<D3D12_DESCRIPTOR_RANGE1>{
			FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, Count, Register, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) };
	};
	const uint32 Frame   = RootSignature.AddConstantBufferView(0);
	const uint32 Body    = RootSignature.AddConstantBufferView(1);
	const uint32 Fog     = RootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Shadow  = RootSignature.AddConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Copy    = RootSignature.AddDescriptorTable(Table(1, 0), D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Depth   = RootSignature.AddDescriptorTable(Table(1, 1), D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Waves   = RootSignature.AddDescriptorTable(Table(1, 2), D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Ibl     = RootSignature.AddDescriptorTable(Table(3, 4), D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 ShadowMap = RootSignature.AddDescriptorTable(Table(1, 7), D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 FogVolume = RootSignature.AddDescriptorTable(Table(1, 8), D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Captures  = RootSignature.AddShaderResourceView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 Atlas     = RootSignature.AddDescriptorTable(Table(1, 10), D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(Frame == WaterRoot_Frame && Body == WaterRoot_Body && Fog == WaterRoot_Fog && Shadow == WaterRoot_Shadow && Copy == WaterRoot_Copy &&
	        Depth == WaterRoot_Depth && Waves == WaterRoot_Waves && Ibl == WaterRoot_Ibl && ShadowMap == WaterRoot_ShadowMap &&
	        FogVolume == WaterRoot_FogVolume && Captures == WaterRoot_Captures && Atlas == WaterRoot_CaptureAtlas);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP));
	D3D12_STATIC_SAMPLER_DESC ShadowSampler =
		FD3D12RootSignature::MakeStaticSampler(2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
	ShadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	ShadowSampler.BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	ShadowSampler.MaxAnisotropy  = 1;
	RootSignature.AddStaticSampler(ShadowSampler);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"WaterRoot"))
	{
		return false;
	}
	return CreatePipelines(false) && CreateWaveTexture();
}

bool FWaterRenderer::CreatePipelines(bool bForceRecompile)
{
	const auto Load = [&](const wchar_t* Entry, EShaderStage Stage) {
		const FShaderCompileDesc Desc = MakeDesc(Entry, Stage);
		if (bForceRecompile && !Library->CookShader(Desc))
		{
			return ComPtr<IDxcBlob>();
		}
		return Library->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> SurfaceVs    = Load(L"VSWater", EShaderStage::Vertex);
	const ComPtr<IDxcBlob> SurfacePs    = Load(L"PSWater", EShaderStage::Pixel);
	const ComPtr<IDxcBlob> UnderwaterVs = Load(L"VSUnderwater", EShaderStage::Vertex);
	const ComPtr<IDxcBlob> UnderwaterPs = Load(L"PSUnderwater", EShaderStage::Pixel);
	if (!SurfaceVs || !SurfacePs || !UnderwaterVs || !UnderwaterPs)
	{
		return false;
	}
	ID3D12Device*         Device = Rhi->GetDevice().GetDevice();
	FGraphicsPipelineDesc Surface;
	Surface.RootSignature          = RootSignature.Get();
	Surface.VertexShader           = FD3D12ShaderCompiler::ToBytecode(SurfaceVs.Get());
	Surface.PixelShader            = FD3D12ShaderCompiler::ToBytecode(SurfacePs.Get());
	Surface.NumRenderTargets       = 2;
	Surface.RenderTargetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	Surface.RenderTargetFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
	Surface.CullMode               = D3D12_CULL_MODE_NONE; // 물속에서 위로 보는 수면도
	FD3D12PipelineState NewSurface;
	if (!NewSurface.InitGraphics(Device, Surface, L"WaterSurfacePipeline"))
	{
		return false;
	}
	FGraphicsPipelineDesc Under;
	Under.RootSignature          = RootSignature.Get();
	Under.VertexShader           = FD3D12ShaderCompiler::ToBytecode(UnderwaterVs.Get());
	Under.PixelShader            = FD3D12ShaderCompiler::ToBytecode(UnderwaterPs.Get());
	Under.RenderTargetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	Under.CullMode               = D3D12_CULL_MODE_NONE;
	Under.BlendMode              = EBlendMode::Premultiplied; // 색 = 산란 + 원래 × (1 - a), 알파(TAA 마스크) 유지
	FD3D12PipelineState NewUnder;
	if (!NewUnder.InitGraphics(Device, Under, L"WaterUnderwaterPipeline"))
	{
		return false;
	}
	SurfacePipeline.Swap(NewSurface);
	UnderwaterPipeline.Swap(NewUnder);
	if (NewSurface.Get() != nullptr)
	{
		Rhi->DeferRelease(NewSurface.Detach());
	}
	if (NewUnder.Get() != nullptr)
	{
		Rhi->DeferRelease(NewUnder.Detach());
	}
	return true;
}

bool FWaterRenderer::CreateWaveTexture()
{
	// 잔물결 높이 = 정수 파수 코사인 합 (타일 이음매 없음) → 해석 기울기로 노멀. B = 타일 워리 노이즈(거품), A = 높이
	struct FWave
	{
		float Kx;
		float Ky;
		float Phase;
		float Amplitude;
	};
	std::mt19937                          Random(4949u);
	std::uniform_real_distribution<float> Unit(0.0f, 1.0f);
	std::vector<FWave>                    Waves;
	float                                 SlopeSquare = 0.0f;
	for (uint32 Index = 0; Index < 40; ++Index)
	{
		const float Radius = 3.0f + 21.0f * Unit(Random) * Unit(Random);
		const float Angle  = Unit(Random) * FMath::TwoPi;
		FWave       Wave{ std::round(std::cos(Angle) * Radius), std::round(std::sin(Angle) * Radius), Unit(Random) * FMath::TwoPi, 0.0f };
		const float K = std::sqrt(Wave.Kx * Wave.Kx + Wave.Ky * Wave.Ky);
		if (K < 1.0f)
		{
			continue;
		}
		Wave.Amplitude = 1.0f / std::pow(K, 1.8f);
		SlopeSquare += 0.5f * FMath::Square(Wave.Amplitude * FMath::TwoPi * K);
		Waves.push_back(Wave);
	}
	const float SlopeScale = 0.35f / std::sqrt(FMath::Max(SlopeSquare, 1.0e-6f)); // RMS 기울기 0.35

	// 워리 노이즈 (8x8 칸, 감김)
	constexpr int32 Cells = 8;
	FVector2        Points[Cells][Cells];
	for (int32 Y = 0; Y < Cells; ++Y)
	{
		for (int32 X = 0; X < Cells; ++X)
		{
			Points[Y][X] = FVector2(Unit(Random), Unit(Random));
		}
	}

	std::vector<uint8> Pixels(WaveTextureSize * WaveTextureSize * 4);
	for (uint32 Y = 0; Y < WaveTextureSize; ++Y)
	{
		for (uint32 X = 0; X < WaveTextureSize; ++X)
		{
			const float U      = (static_cast<float>(X) + 0.5f) / WaveTextureSize;
			const float V      = (static_cast<float>(Y) + 0.5f) / WaveTextureSize;
			float       Height = 0.0f;
			float       DhDu   = 0.0f;
			float       DhDv   = 0.0f;
			for (const FWave& Wave : Waves)
			{
				const float Arg = FMath::TwoPi * (Wave.Kx * U + Wave.Ky * V) + Wave.Phase;
				Height += Wave.Amplitude * std::cos(Arg);
				DhDu -= Wave.Amplitude * std::sin(Arg) * FMath::TwoPi * Wave.Kx;
				DhDv -= Wave.Amplitude * std::sin(Arg) * FMath::TwoPi * Wave.Ky;
			}
			FVector3 Normal(-DhDu * SlopeScale, -DhDv * SlopeScale, 1.0f);
			Normal = Normal.GetNormalized();

			float      MinDistance = 10.0f;
			const float CellU      = U * Cells;
			const float CellV      = V * Cells;
			const int32 BaseX      = static_cast<int32>(CellU);
			const int32 BaseY      = static_cast<int32>(CellV);
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const int32    CellX = BaseX + OffsetX;
					const int32    CellY = BaseY + OffsetY;
					const FVector2 Point = Points[(CellY + Cells) % Cells][(CellX + Cells) % Cells];
					const float    Dx    = static_cast<float>(CellX) + Point.X - CellU;
					const float    Dy    = static_cast<float>(CellY) + Point.Y - CellV;
					MinDistance          = FMath::Min(MinDistance, std::sqrt(Dx * Dx + Dy * Dy));
				}
			}
			const float Foam = FMath::Clamp(1.0f - MinDistance * 1.1f, 0.0f, 1.0f);

			uint8* Out = &Pixels[(Y * WaveTextureSize + X) * 4];
			Out[0]     = static_cast<uint8>(FMath::Clamp(Normal.X * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
			Out[1]     = static_cast<uint8>(FMath::Clamp(Normal.Y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
			Out[2]     = static_cast<uint8>(Foam * 255.0f + 0.5f);
			Out[3]     = static_cast<uint8>(FMath::Clamp(Height * SlopeScale * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
	}
	return WaveTexture.Init2D(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), WaveTextureSize, WaveTextureSize,
	                          DXGI_FORMAT_R8G8B8A8_UNORM, Pixels.data(), 4, L"WaterWaveNormals", true);
}

bool FWaterRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return false;
	}
	if (!CreatePipelines(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "물 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	return true;
}

void FWaterRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	WaveTexture.Shutdown();
	SurfacePipeline.Shutdown();
	UnderwaterPipeline.Shutdown();
	RootSignature.Shutdown();
	Bodies.clear();
	Rhi     = nullptr;
	Library = nullptr;
}

uint32 FWaterRenderer::Prepare(FScene& Scene, const FFrustum& Frustum, const FVector3& CameraPosition)
{
	Bodies.clear();
	UnderwaterIndex = -1;
	if (!RendererCVars::Water.Get())
	{
		return 0;
	}
	Scene.GetRegistry().View<FTransformComponent, FWaterBodyComponent>().Each([&](FEntity, FTransformComponent& Transform, FWaterBodyComponent& Water) {
		const FVector3 Half    = FVector3(FMath::Max(Water.Size.X, 1.0f), FMath::Max(Water.Size.Y, 1.0f), FMath::Max(Water.Size.Z, 1.0f)) * 0.5f;
		const FVector3 Center  = Transform.GetWorldPosition();
		const FVector3 Forward = Transform.GetWorldForward();
		const float    Yaw     = FMath::Atan2(Forward.Y, Forward.X);
		const float    CosYaw  = FMath::Cos(Yaw);
		const float    SinYaw  = FMath::Sin(Yaw);
		// 요 회전 상자의 월드 AABB (컬링)
		const FVector3 Extent(FMath::Abs(CosYaw) * Half.X + FMath::Abs(SinYaw) * Half.Y, FMath::Abs(SinYaw) * Half.X + FMath::Abs(CosYaw) * Half.Y, Half.Z);
		// 카메라가 상자 안인가 (물속)
		const FVector3 Rel   = CameraPosition - Center;
		const float    LocalX = Rel.X * CosYaw + Rel.Y * SinYaw;
		const float    LocalY = -Rel.X * SinYaw + Rel.Y * CosYaw;
		const bool     bInside = FMath::Abs(LocalX) < Half.X && FMath::Abs(LocalY) < Half.Y && FMath::Abs(Rel.Z) < Half.Z;
		if (!bInside && !Frustum.Intersects(FBox(Center - Extent, Center + Extent)))
		{
			return;
		}
		FWaterBodyConstants Body;
		Body.Center              = Center;
		Body.CosYaw              = CosYaw;
		Body.HalfSize            = Half;
		Body.SinYaw              = SinYaw;
		Body.ScatterColor        = Water.ScatterColor;
		Body.NormalStrength      = FMath::Max(Water.NormalStrength, 0.0f);
		Body.Absorption          = FVector3(FMath::Max(Water.Absorption.X, 0.0f), FMath::Max(Water.Absorption.Y, 0.0f), FMath::Max(Water.Absorption.Z, 0.0f));
		Body.WaveScale           = FMath::Max(Water.WaveScale, 1.0f);
		const float FlowAngle    = Yaw + FMath::DegreesToRadians(Water.FlowDirection); // 로컬 흐름 → 월드
		Body.FlowDirection       = FVector2(FMath::Cos(FlowAngle), FMath::Sin(FlowAngle));
		Body.FlowSpeed           = Water.FlowSpeed;
		Body.WaveSpeed           = Water.WaveSpeed;
		Body.FoamIntensity       = FMath::Max(Water.FoamIntensity, 0.0f);
		Body.FoamDistance        = FMath::Max(Water.FoamDistance, 0.0f);
		Body.RefractionStrength  = FMath::Max(Water.RefractionStrength, 0.0f);
		Body.ReflectionIntensity = FMath::Max(Water.ReflectionIntensity, 0.0f);
		Body.Roughness           = FMath::Clamp(Water.Roughness, 0.01f, 1.0f);
		if (bInside && UnderwaterIndex < 0)
		{
			UnderwaterIndex = static_cast<int32>(Bodies.size());
			UnderwaterBody  = Body;
		}
		Bodies.push_back(Body);
	});
	return static_cast<uint32>(Bodies.size());
}

D3D12_GPU_VIRTUAL_ADDRESS FWaterRenderer::UploadFrameConstants(const FWaterPassInputs& Inputs) const
{
	FWaterFrameConstants Frame;
	Frame.ViewProjection           = Inputs.ViewProjection;
	Frame.UnjitteredViewProjection = Inputs.UnjitteredViewProjection;
	Frame.PrevViewProjection       = Inputs.PrevViewProjection;
	Frame.InvViewProjection        = Inputs.ViewProjection.GetInverse();
	Frame.CameraPosition           = Inputs.CameraPosition;
	Frame.Time                     = std::chrono::duration<float>(std::chrono::steady_clock::now() - StartTime).count();
	Frame.SunDirection             = Inputs.SunDirection;
	Frame.AmbientIntensity         = Inputs.AmbientIntensity;
	Frame.SunColor                 = Inputs.SunColor;
	Frame.ReflectionCaptureCount   = Inputs.ReflectionCaptureCount;
	const float Width              = static_cast<float>(Inputs.SceneColor->GetWidth());
	const float Height             = static_cast<float>(Inputs.SceneColor->GetHeight());
	Frame.ScreenSize               = FVector2(Width, Height);
	Frame.InvScreenSize            = FVector2(1.0f / Width, 1.0f / Height);
	Frame.bScreenReflections       = RendererCVars::WaterScreenReflections.Get() ? 1u : 0u;
	Frame.ReactiveMask             = 0.2f;
	return Rhi->GetDynamicBuffer().AllocateConstants(Frame).GpuAddress;
}

void FWaterRenderer::BindCommon(ID3D12GraphicsCommandList* List, D3D12_GPU_VIRTUAL_ADDRESS Frame, const FWaterPassInputs& Inputs,
                                const FD3D12DescriptorHandle& CopySrv) const
{
	List->SetGraphicsRootSignature(RootSignature.Get());
	List->SetGraphicsRootConstantBufferView(WaterRoot_Frame, Frame);
	List->SetGraphicsRootConstantBufferView(WaterRoot_Fog, Inputs.FogConstants);
	List->SetGraphicsRootConstantBufferView(WaterRoot_Shadow, Inputs.ShadowConstants);
	List->SetGraphicsRootDescriptorTable(WaterRoot_Copy, CopySrv.Gpu);
	List->SetGraphicsRootDescriptorTable(WaterRoot_Depth, Inputs.SceneColor->GetDepthSrv().Gpu);
	List->SetGraphicsRootDescriptorTable(WaterRoot_Waves, WaveTexture.GetSrv().Gpu);
	List->SetGraphicsRootDescriptorTable(WaterRoot_Ibl, Inputs.IblTable.Gpu);
	List->SetGraphicsRootDescriptorTable(WaterRoot_ShadowMap, Inputs.ShadowMapSrv.Gpu);
	List->SetGraphicsRootDescriptorTable(WaterRoot_FogVolume, Inputs.FogVolumeSrv.Gpu);
	List->SetGraphicsRootShaderResourceView(WaterRoot_Captures, Inputs.CaptureList);
	List->SetGraphicsRootDescriptorTable(WaterRoot_CaptureAtlas, Inputs.CaptureAtlasSrv.Gpu);
}

void FWaterRenderer::AddSurfacePass(FRenderGraph& Graph, const FWaterPassInputs& Inputs, int32 Timer)
{
	if (Bodies.empty())
	{
		return;
	}
	const uint32   Width   = Inputs.SceneColor->GetWidth();
	const uint32   Height  = Inputs.SceneColor->GetHeight();
	FRGTextureDesc CopyDesc;
	CopyDesc.Width  = Width;
	CopyDesc.Height = Height;
	CopyDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	const FRGResourceRef    CopyRef = Graph.CreateTexture("WaterRefractionSource", CopyDesc);
	const FRGPooledTexture* Copy    = Graph.GetTexture(CopyRef);
	ID3D12Resource*         SceneColorResource = Inputs.SceneColor->GetColorResource();
	ID3D12Resource*         CopyResource       = Copy->Resource.Get();
	Graph.AddPass("물 굴절 원본 복사")
		.Read(Inputs.ColorRef, ERGAccess::CopySource)
		.Write(CopyRef, ERGAccess::CopyDest, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([SceneColorResource, CopyResource](FRGContext& Context) { Context.CommandList->CopyResource(CopyResource, SceneColorResource); });

	const D3D12_GPU_VIRTUAL_ADDRESS Frame = UploadFrameConstants(Inputs);
	std::vector<D3D12_GPU_VIRTUAL_ADDRESS> BodyAddresses;
	BodyAddresses.reserve(Bodies.size());
	for (const FWaterBodyConstants& Body : Bodies)
	{
		BodyAddresses.push_back(Rhi->GetDynamicBuffer().AllocateConstants(Body).GpuAddress);
	}
	const FD3D12DescriptorHandle CopySrv = Copy->Srv;
	FRenderGraph::FPassBuilder   Pass    = Graph.AddPass("물 수면");
	Pass.Read(CopyRef, ERGAccess::SrvPixel)
		.Read(Inputs.DepthRef, ERGAccess::SrvPixel)
		.Write(Inputs.ColorRef, ERGAccess::RenderTarget)
		.Write(Inputs.VelocityRef, ERGAccess::RenderTarget)
		.Timer(Timer);
	if (Inputs.ShadowMapRef.IsValid())
	{
		Pass.Read(Inputs.ShadowMapRef, ERGAccess::SrvPixel);
	}
	if (Inputs.FogVolumeRef.IsValid())
	{
		Pass.Read(Inputs.FogVolumeRef, ERGAccess::SrvPixel);
	}
	Pass.Execute([this, Inputs, Frame, BodyAddresses, CopySrv, Width, Height](FRGContext& Context) {
		ID3D12GraphicsCommandList*        List   = Context.CommandList;
		const D3D12_CPU_DESCRIPTOR_HANDLE Rtvs[] = { Inputs.SceneColor->GetRtv(), Inputs.SceneVelocity->GetRtv() };
		List->OMSetRenderTargets(2, Rtvs, FALSE, nullptr);
		SetScreenPassViewport(List, Width, Height);
		BindCommon(List, Frame, Inputs, CopySrv);
		List->SetPipelineState(SurfacePipeline.Get());
		List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (const D3D12_GPU_VIRTUAL_ADDRESS Body : BodyAddresses)
		{
			List->SetGraphicsRootConstantBufferView(WaterRoot_Body, Body);
			List->DrawInstanced(6, 1, 0, 0);
		}
	});
}

void FWaterRenderer::AddUnderwaterPass(FRenderGraph& Graph, const FWaterPassInputs& Inputs, int32 Timer)
{
	if (UnderwaterIndex < 0)
	{
		return;
	}
	const D3D12_GPU_VIRTUAL_ADDRESS Frame  = UploadFrameConstants(Inputs);
	const D3D12_GPU_VIRTUAL_ADDRESS Body   = Rhi->GetDynamicBuffer().AllocateConstants(UnderwaterBody).GpuAddress;
	const uint32                    Width  = Inputs.SceneColor->GetWidth();
	const uint32                    Height = Inputs.SceneColor->GetHeight();
	FRenderGraph::FPassBuilder      Pass   = Graph.AddPass("물속");
	Pass.Read(Inputs.DepthRef, ERGAccess::SrvPixel).Write(Inputs.ColorRef, ERGAccess::RenderTarget).Timer(Timer);
	if (Inputs.ShadowMapRef.IsValid())
	{
		Pass.Read(Inputs.ShadowMapRef, ERGAccess::SrvPixel);
	}
	if (Inputs.FogVolumeRef.IsValid())
	{
		Pass.Read(Inputs.FogVolumeRef, ERGAccess::SrvPixel);
	}
	Pass.Execute([this, Inputs, Frame, Body, Width, Height](FRGContext& Context) {
		ID3D12GraphicsCommandList*        List = Context.CommandList;
		const D3D12_CPU_DESCRIPTOR_HANDLE Rtv  = Inputs.SceneColor->GetRtv();
		List->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
		SetScreenPassViewport(List, Width, Height);
		BindCommon(List, Frame, Inputs, WaveTexture.GetSrv()); // t0(복사본)은 이 셰이더가 읽지 않는다 — 항상 유효한 정적 텍스처로
		List->SetGraphicsRootConstantBufferView(WaterRoot_Body, Body);
		List->SetPipelineState(UnderwaterPipeline.Get());
		List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		List->DrawInstanced(3, 1, 0, 0);
	});
}
