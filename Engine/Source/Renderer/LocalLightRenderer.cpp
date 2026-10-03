#include "Renderer/LocalLightRenderer.h"

#include "Core/FileSystem.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/AreaLightMath.h"
#include "Renderer/Camera.h"
#include "Renderer/IesProfile.h"
#include "Renderer/LightMath.h"
#include "Renderer/MaterialRender.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numeric>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr uint32 GCullGroupSize = 64; // ClusterCulling.hlsl numthreads

	constexpr DXGI_FORMAT ShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
	constexpr DXGI_FORMAT ShadowDsvFormat      = DXGI_FORMAT_D32_FLOAT;
	constexpr DXGI_FORMAT ShadowSrvFormat      = DXGI_FORMAT_R32_FLOAT;

	enum EComputeRootParameter : uint32
	{
		ComputeParam_Constants = 0, // b0
		ComputeParam_Lights    = 1, // t0
		ComputeParam_Clusters  = 2, // u0
	};

	enum EShadowRootParameter : uint32
	{
		ShadowParam_PassConstants   = 0, // b0 (루트 상수 17개: 장 뷰-투영 + 인스턴스 시작 위치, Shadow.hlsl)
		ShadowParam_SkinPalette     = 1, // t15 (프레임 스킨 팔레트)
		ShadowParam_Instances       = 2, // t13
		ShadowParam_InstanceIndices = 3, // t14
		ShadowParam_MaskConstants   = 4, // b1 (Masked: 알파 팩터, 컷오프)
		ShadowParam_MaskTexture     = 5, // t0 (Masked: 베이스 컬러)
		ShadowParam_MaterialConstants = 6, // b2 (그래프 머티리얼 Masked)
		ShadowParam_MaterialTextures  = 7, // 공간 2 t0~ (그래프 머티리얼 텍스처, 무제한 범위)
	};
} // namespace

FLocalLightRenderer::~FLocalLightRenderer()
{
	Shutdown();
}

bool FLocalLightRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources)
{
	E_CHECKF(Rhi == nullptr, "로컬 라이트 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	Resources     = &InResources;

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	const uint32 ConstantsIndex = ComputeRootSignature.AddConstantBufferView(0);
	const uint32 LightsIndex    = ComputeRootSignature.AddShaderResourceView(0);
	const uint32 ClustersIndex  = ComputeRootSignature.AddUnorderedAccessView(0);
	E_CHECK(ConstantsIndex == ComputeParam_Constants && LightsIndex == ComputeParam_Lights && ClustersIndex == ComputeParam_Clusters);
	if (!ComputeRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ClusterCullingRootSignature"))
	{
		return false;
	}

	const uint32 PassIndex      = ShadowRootSignature.AddConstants(17, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 PaletteIndex   = ShadowRootSignature.AddShaderResourceView(15, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 InstancesIndex = ShadowRootSignature.AddShaderResourceView(13, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 IndicesIndex   = ShadowRootSignature.AddShaderResourceView(14, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 MaskIndex      = ShadowRootSignature.AddConstants(2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 MaskTexture    = ShadowRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) },
	                                                                     D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(PassIndex == ShadowParam_PassConstants && PaletteIndex == ShadowParam_SkinPalette && InstancesIndex == ShadowParam_Instances &&
	        IndicesIndex == ShadowParam_InstanceIndices && MaskIndex == ShadowParam_MaskConstants && MaskTexture == ShadowParam_MaskTexture);
	const uint32 MaterialConstants = ShadowRootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 MaterialTextures  = ShadowRootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) },
		D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(MaterialConstants == ShadowParam_MaterialConstants && MaterialTextures == ShadowParam_MaterialTextures);
	ShadowRootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR));
	ShadowRootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)); // 그래프 Clamp
	if (!ShadowRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"LocalShadowRootSignature"))
	{
		return false;
	}

	MaterialPipelines.Init(*Rhi, *ShaderLibrary, L"LocalShadowMaterialPipeline");
	if (!CreateCullPipeline(CullPipeline, false) || !CreateShadowPipeline(ShadowPipelines[0], false, 0) ||
	    !CreateShadowPipeline(ShadowPipelines[1], false, 1) || !CreateShadowPipeline(ShadowPipelines[2], false, 2) ||
	    !CreateShadowPipeline(ShadowPipelines[3], false, 3))
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

	// 셰이더가 항상 유효한 SRV를 참조하도록 작은 한 장짜리를 미리 만든다 (그림자 라이트가 나오면 다시 만든다)
	if (!EnsureShadowMap(1, 1) || !CreateLtcTextures())
	{
		return false;
	}

	Lights.reserve(64);
	return true;
}

void FLocalLightRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 호출자(씬 렌더러)가 GPU Flush 이후 종료한다
	if (ShadowSrv.IsValid())
	{
		Rhi->GetSrvAllocator().Free(ShadowSrv);
		ShadowSrv = FD3D12DescriptorHandle{};
	}
	ShadowMap.Reset();
	ShadowDsvHeap.Shutdown();
	ShadowMapResolution = 0;
	ShadowMapSlices     = 0;
	ClusterBuffer.Reset();
	CullPipeline.Shutdown();
	for (FD3D12PipelineState& Pipeline : ShadowPipelines)
	{
		Pipeline.Shutdown();
	}
	MaterialPipelines.Shutdown();
	ComputeRootSignature.Shutdown();
	ShadowRootSignature.Shutdown();
	Lights.clear();
	if (Resources != nullptr)
	{
		for (FTextureHandle& Handle : LtcTextures)
		{
			if (Handle.IsValid())
			{
				Resources->DestroyTexture(Handle);
			}
			Handle = FTextureHandle{};
		}
		for (auto& [Key, Entry] : IesProfiles)
		{
			if (Entry.Texture.IsValid())
			{
				Resources->DestroyTexture(Entry.Texture);
			}
		}
	}
	IesProfiles.clear();
	CookieTextures.clear(); // 경로 캐시 텍스처는 리소스 관리자 소유 (루트에서 빠지면 수거)
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
	Resources     = nullptr;
}

bool FLocalLightRenderer::CreateLtcTextures()
{
	// LTC 표 (RGBA32F 64x64 두 장) — 셰이더는 선형 클램프로 읽는다 (AreaLight.hlsli)
	const float* Tables[2] = { AreaLightMath::GetLtcTable1(), AreaLightMath::GetLtcTable2() };
	const wchar_t* Names[2] = { L"LtcTable1", L"LtcTable2" };
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		LtcTextures[Index] = Resources->CreateTexture(AreaLightMath::LtcTableSize, AreaLightMath::LtcTableSize, DXGI_FORMAT_R32G32B32A32_FLOAT, Tables[Index],
		                                              sizeof(float) * 4, Names[Index]);
		if (!LtcTextures[Index].IsValid())
		{
			E_LOG(LogRenderer, Error, "LTC 표 텍스처를 만들지 못했습니다");
			return false;
		}
	}
	return true;
}

void FLocalLightRenderer::CollectResourceRoots(FResourceRoots& Roots) const
{
	for (const auto& [Key, Handle] : CookieTextures)
	{
		Roots.Add(Handle);
	}
}

namespace
{
	// Content 기준 경로 키 (소문자, '/')
	std::string MakeContentKey(const std::string& Path)
	{
		std::string Key = Path;
		for (char& Char : Key)
		{
			Char = Char == '\\' ? '/': static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
		}
		return Key;
	}

	std::filesystem::path ResolveContentPath(const std::string& Path)
	{
		const std::filesystem::path Relative = FStringConv::ToWide(Path);
		if (Relative.is_absolute() || !FPaths::HasProject())
		{
			return Relative.lexically_normal();
		}
		return (FPaths::GetProjectContentDirectory() / Relative).lexically_normal();
	}
} // namespace

const FLocalLightRenderer::FIesEntry& FLocalLightRenderer::FindIesProfile(const std::string& Path)
{
	const std::string Key = MakeContentKey(Path);
	auto              It  = IesProfiles.find(Key);
	if (It != IesProfiles.end())
	{
		return It->second;
	}
	FIesEntry&                  Entry    = IesProfiles[Key];
	const std::filesystem::path Absolute = ResolveContentPath(Path);
	std::string                 Text;
	if (!FFileSystem::ReadTextFile(Absolute, Text))
	{
		E_LOG(LogRenderer, Warning, "IES 프로필을 읽지 못했습니다: {}", Path);
		return Entry;
	}
	FIesProfile Profile;
	std::string Error;
	if (!FIesProfile::Parse(Text, Profile, &Error))
	{
		E_LOG(LogRenderer, Warning, "IES 프로필 파싱 실패 ({}): {}", Path, Error);
		return Entry;
	}
	if (!Error.empty())
	{
		E_LOG(LogRenderer, Warning, "IES 프로필 {}: {}", Path, Error);
	}
	const std::vector<float> Pixels = Profile.BakeTexture(AreaLightMath::IesTextureWidth, AreaLightMath::IesTextureHeight);
	Entry.Texture    = Resources->CreateTexture(AreaLightMath::IesTextureWidth, AreaLightMath::IesTextureHeight, DXGI_FORMAT_R32_FLOAT, Pixels.data(),
	                                            sizeof(float), L"IesProfile");
	Entry.MaxCandela = Profile.MaxCandela;
	Entry.bValid     = Entry.Texture.IsValid();
	E_LOG(LogRenderer, Log, "IES 프로필 로드: {} ({}, 최대 {:.1f}cd, 수직 {} × 수평 {})", Path, Profile.FormatName, Profile.MaxCandela,
	      Profile.VerticalAngles.size(), Profile.HorizontalAngles.size());
	return Entry;
}

int32 FLocalLightRenderer::ResolveIesTexture(const FIesEntry& Entry) const
{
	if (!Entry.bValid)
	{
		return -1;
	}
	const FD3D12Texture* Texture = Resources->GetTexture(Entry.Texture);
	return Texture != nullptr && Texture->GetSrv().IsValid() ? static_cast<int32>(Texture->GetSrv().Index) : -1;
}

int32 FLocalLightRenderer::ResolveCookieTexture(const std::string& Path)
{
	if (Path.empty())
	{
		return -1;
	}
	const std::string Key = MakeContentKey(Path);
	auto              It  = CookieTextures.find(Key);
	if (It == CookieTextures.end())
	{
		It = CookieTextures.emplace(Key, Resources->LoadTexture(ResolveContentPath(Path), ETextureUsage::Color)).first;
	}
	// 준비 전(비동기)·실패는 흰색 텍스처 = 쿠키 없음과 같은 밝기. 힙 칸은 매 프레임 다시 읽는다 (재생성·수거 안전)
	const FD3D12DescriptorHandle& Srv = Resources->ResolveTexture(It->second).GetSrv();
	return Srv.IsValid() ? static_cast<int32>(Srv.Index) : -1;
}

bool FLocalLightRenderer::CreateCullPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
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

bool FLocalLightRenderer::CreateShadowPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, uint32 Variant)
{
	const bool         bSkinned = (Variant & DepthVariantSkinned) != 0;
	const bool         bMasked  = (Variant & DepthVariantMasked) != 0;
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Shadow.hlsl";
	VertexDesc.EntryPoint = bMasked ? (bSkinned ? L"ShadowSkinnedMaskedVS" : L"ShadowMaskedVS") : (bSkinned ? L"ShadowSkinnedVS" : L"ShadowVS");
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc;
	PixelDesc.FileName   = L"Shadow.hlsl";
	PixelDesc.EntryPoint = L"ShadowMaskedPS";
	PixelDesc.Stage      = EShaderStage::Pixel;
	if (bForceRecompile && (!ShaderLibrary->CookShader(VertexDesc) || (bMasked && !ShaderLibrary->CookShader(PixelDesc))))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary->GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = bMasked ? ShaderLibrary->GetShader(PixelDesc) : ComPtr<IDxcBlob>();
	if (!VertexShader || (bMasked && !PixelShader))
	{
		return false;
	}

	FGraphicsPipelineDesc Desc;
	Desc.RootSignature        = ShadowRootSignature.Get();
	Desc.VertexShader         = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	if (bMasked)
	{
		Desc.PixelShader = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	}
	Desc.InputLayout          = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout();
	Desc.NumRenderTargets     = 0;
	Desc.DepthStencilFormat   = ShadowDsvFormat;
	Desc.bDepthEnable         = true;
	Desc.CullMode             = D3D12_CULL_MODE_NONE; // 한 면짜리 메시도 그림자를 드리운다
	Desc.bDepthClip           = true;                 // 원근: 광원 뒤 지오메트리는 잘라야 한다
	Desc.DepthBias            = BakedDepthBias;
	Desc.SlopeScaledDepthBias = BakedSlopeBias;
	if (Variant == 0)
	{
		MaterialPipelines.SetBaseDesc(Desc); // 그래프 머티리얼 Masked PSO도 같은 설정
	}
	static const wchar_t* const Names[DepthVariantCount] = { L"LocalShadowPipeline", L"LocalShadowSkinnedPipeline", L"LocalShadowMaskedPipeline",
	                                                         L"LocalShadowSkinnedMaskedPipeline" };
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, Names[Variant]);
}

bool FLocalLightRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	MaterialPipelines.Reset(); // 그래프 머티리얼 PSO는 다음 그리기에 다시 만든다
	FD3D12PipelineState NewCull;
	FD3D12PipelineState NewShadow[DepthVariantCount];
	bool                bOk = CreateCullPipeline(NewCull, bForceRecompile);
	for (uint32 Variant = 0; bOk && Variant < DepthVariantCount; ++Variant)
	{
		bOk = CreateShadowPipeline(NewShadow[Variant], bForceRecompile, Variant);
	}
	if (!bOk)
	{
		E_LOG(LogRenderer, Error, "로컬 라이트 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	CullPipeline.Swap(NewCull);
	Rhi->DeferRelease(NewCull.Detach());
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		ShadowPipelines[Variant].Swap(NewShadow[Variant]);
		Rhi->DeferRelease(NewShadow[Variant].Detach());
	}
	return true;
}

D3D12_GPU_VIRTUAL_ADDRESS FLocalLightRenderer::GetClusterData() const
{
	return ClusterBuffer ? ClusterBuffer->GetGPUVirtualAddress() : 0;
}

FRGResourceRef FLocalLightRenderer::ImportShadowMap(FRenderGraph& Graph) const
{
	return ShadowMap ? Graph.Import("LocalShadowMap", ShadowMap.Get(), ERGAccess::SrvPixel, ERGAccess::SrvPixel, 1, ShadowMapSlices) : FRGResourceRef{};
}

FRGResourceRef FLocalLightRenderer::ImportClusters(FRenderGraph& Graph)
{
	return Graph.ImportTracked("ClusterBuffer", ClusterBuffer.Get(), &ClusterState);
}

bool FLocalLightRenderer::EnsureShadowMap(uint32 Resolution, uint32 Slices)
{
	if (ShadowMap && ShadowMapResolution == Resolution && ShadowMapSlices >= Slices)
	{
		return true;
	}
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	ComPtr<ID3D12Resource> NewShadowMap;
	FD3D12DescriptorHeap   NewDsvHeap;

	D3D12_RESOURCE_DESC Desc = MakeTexture2DDesc(Resolution, Resolution, ShadowResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	Desc.DepthOrArraySize    = static_cast<UINT16>(Slices);

	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format             = ShadowDsvFormat;
	ClearValue.DepthStencil.Depth = 1.0f;

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
	                                           IID_PPV_ARGS(&NewShadowMap))))
	{
		E_LOG(LogRenderer, Error, "로컬 그림자 맵 생성 실패 ({}x{} x {})", Resolution, Resolution, Slices);
		return false;
	}
	NewShadowMap->SetName(L"LocalShadowMap");

	if (!NewDsvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, Slices, false, L"LocalShadowDsvHeap"))
	{
		return false;
	}
	for (uint32 Index = 0; Index < Slices; ++Index)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc{};
		DsvDesc.Format                         = ShadowDsvFormat;
		DsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		DsvDesc.Texture2DArray.FirstArraySlice = Index;
		DsvDesc.Texture2DArray.ArraySize       = 1;
		Device->CreateDepthStencilView(NewShadowMap.Get(), &DsvDesc, NewDsvHeap.GetCpuHandle(Index));
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                   = ShadowSrvFormat;
	SrvDesc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	SrvDesc.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2DArray.MipLevels = 1;
	SrvDesc.Texture2DArray.ArraySize = Slices;
	const FD3D12DescriptorHandle NewSrv = Rhi->GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(NewShadowMap.Get(), &SrvDesc, NewSrv.Cpu);

	// 새 리소스가 모두 준비된 뒤 기존 리소스를 지연 해제한다
	ReleaseShadowMap();
	ShadowMap           = std::move(NewShadowMap);
	ShadowDsvHeap       = std::move(NewDsvHeap);
	ShadowSrv           = NewSrv;
	ShadowMapResolution = Resolution;
	ShadowMapSlices     = Slices;
	if (Slices > 1)
	{
		E_LOG(LogRenderer, Log, "로컬 그림자 맵 생성: {}x{} x {}장", Resolution, Resolution, Slices);
	}
	return true;
}

void FLocalLightRenderer::ReleaseShadowMap()
{
	if (!ShadowMap)
	{
		return;
	}
	// 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	Rhi->DeferRelease(ShadowMap);
	Rhi->DeferFreeDescriptor(ShadowSrv);
	ShadowSrv = FD3D12DescriptorHandle{};
	ShadowMap.Reset();
	ShadowDsvHeap.Shutdown(); // DSV 힙은 기록 시점에만 읽히므로 즉시 해제
	ShadowMapResolution = 0;
	ShadowMapSlices     = 0;
}

void FLocalLightRenderer::CollectLights(FScene& Scene, const FCamera& Camera)
{
	Lights.clear();
	LightScores.clear();
	LightOuterAngles.clear();
	LightShadowRadius.clear();
	LightWantsShadow.clear();
	AreaLightCount    = 0;
	DirectionalCookie = -1;

	const FFrustum Frustum        = FFrustum::FromViewProjection(Camera.GetViewProjectionMatrix());
	const FVector3 CameraPosition = Camera.GetPosition();
	FRegistry&     Registry       = Scene.GetRegistry();
	const float    Time           = MaterialRender::GetMaterialTime(); // 쿠키 패닝 (머티리얼 Time 노드와 같은 시계)

	// CullRadius = 컬링·정렬·그림자 판정 반경 (점/스포트 = Radius, 면광원 = 경계 구)
	auto AddLight = [&](const FVector3& Position, const FVector3& SrgbColor, float Intensity, float Radius, float CullRadius,
	                    bool bShadow) -> FLocalLightGpuData* {
		if (Intensity <= 0.0f || Radius <= 0.0f)
		{
			return nullptr;
		}
		if (!Frustum.Intersects(FBox(Position - FVector3(CullRadius), Position + FVector3(CullRadius))))
		{
			return nullptr;
		}
		FLocalLightGpuData& Light = Lights.emplace_back();
		Light.Position            = Position;
		Light.Radius              = Radius;
		Light.Color               = LightMath::SrgbToLinear(SrgbColor) * Intensity;
		LightScores.push_back(FMath::Max(FVector3::Distance(Position, CameraPosition) - CullRadius, 0.0f));
		LightOuterAngles.push_back(0.0f);
		LightShadowRadius.push_back(CullRadius);
		LightWantsShadow.push_back(bShadow ? 1 : 0);
		return &Light;
	};
	// 트랜스폼 축 (스케일 무시, 직교화): Cross(Forward, Right) = Up
	auto SetAxes = [](FLocalLightGpuData& Light, const FTransformComponent& Transform) {
		const FVector3 Forward = Transform.GetWorldForward();
		FVector3       Right   = Transform.WorldMatrix.GetAxisY();
		Right                  = (Right - Forward * FVector3::Dot(Right, Forward)).GetNormalized();
		if (Right.LengthSquared() < 0.5f)
		{
			const FVector3 Helper = FMath::Abs(Forward.Z) > 0.99f ? FVector3::ForwardVector : FVector3::UpVector;
			Right                 = FVector3::Cross(Helper, Forward).GetNormalized();
		}
		Light.Direction = Forward;
		Light.Right     = Right;
		Light.Up        = FVector3::Cross(Forward, Right);
	};
	// IES 밝기: 프로필 최대 칸델라 기준이면 Intensity 대체
	auto ResolveIntensity = [&](const std::string& IesPath, bool bUseIes, float Scale, float Intensity, int32& OutIes) {
		OutIes = -1;
		if (IesPath.empty())
		{
			return Intensity;
		}
		const FIesEntry& Entry = FindIesProfile(IesPath);
		OutIes                 = ResolveIesTexture(Entry);
		return Entry.bValid && bUseIes ? Entry.MaxCandela * AreaLightMath::CandelaToIntensity * Scale : Intensity;
	};
	auto SetCookie = [&](FLocalLightGpuData& Light, const std::string& CookiePath, LightMath::ELocalLightType Type, float ProjectionDegrees,
	                     const FVector2& Scale, const FVector2& PanSpeed) {
		Light.CookieTexture = ResolveCookieTexture(CookiePath);
		if (Light.CookieTexture >= 0)
		{
			const FVector2 Offset(PanSpeed.X * Time - std::floor(PanSpeed.X * Time), PanSpeed.Y * Time - std::floor(PanSpeed.Y * Time));
			Light.CookieTransform = AreaLightMath::ComputeCookieTransform(Type, ProjectionDegrees, Scale, Offset);
		}
	};

	Registry.View<FTransformComponent, FPointLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FPointLightComponent& Point) {
		int32       Ies       = -1;
		const float Intensity = ResolveIntensity(Point.IesProfile, Point.bUseIesIntensity, Point.IesIntensityScale, Point.Intensity, Ies);
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Point.Color, Intensity, Point.Radius, Point.Radius, Point.bCastShadows))
		{
			Light->Type = static_cast<uint32>(LightMath::ELocalLightType::Point);
			if (Ies >= 0 || !Point.CookieTexture.empty())
			{
				// 방향이 필요한 경우에만 축을 채운다 (없으면 Phase 23 값 그대로 — 화면 비트 동일)
				SetAxes(*Light, Transform);
				Light->IesTexture = Ies;
				SetCookie(*Light, Point.CookieTexture, LightMath::ELocalLightType::Point, 0.0f, Point.CookieScale, Point.CookiePanSpeed);
			}
		}
	});
	Registry.View<FTransformComponent, FSpotLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FSpotLightComponent& Spot) {
		int32       Ies       = -1;
		const float Intensity = ResolveIntensity(Spot.IesProfile, Spot.bUseIesIntensity, Spot.IesIntensityScale, Spot.Intensity, Ies);
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Spot.Color, Intensity, Spot.Radius, Spot.Radius, Spot.bCastShadows))
		{
			const LightMath::FConeParams Cone = LightMath::ComputeConeParams(Spot.InnerConeAngle, Spot.OuterConeAngle);
			Light->Type            = static_cast<uint32>(LightMath::ELocalLightType::Spot);
			Light->Direction       = Transform.GetWorldForward();
			Light->ConeScale       = Cone.Scale;
			Light->ConeOffset      = Cone.Offset;
			LightOuterAngles.back() = Spot.OuterConeAngle;
			if (Ies >= 0 || !Spot.CookieTexture.empty())
			{
				SetAxes(*Light, Transform);
				Light->IesTexture = Ies;
				SetCookie(*Light, Spot.CookieTexture, LightMath::ELocalLightType::Spot, Spot.OuterConeAngle, Spot.CookieScale, Spot.CookiePanSpeed);
			}
		}
	});
	Registry.View<FTransformComponent, FAreaLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FAreaLightComponent& Area) {
		const LightMath::ELocalLightType Type =
			Area.Shape == static_cast<int32>(EAreaLightShape::Disc) ? LightMath::ELocalLightType::Disc : LightMath::ELocalLightType::Rect;
		const float HalfWidth  = FMath::Max(Area.Width, 0.1f) * 0.5f;
		const float HalfHeight = FMath::Max(Area.Height, 0.1f) * 0.5f;
		int32       Ies        = -1;
		const float Intensity  = ResolveIntensity(Area.IesProfile, Area.bUseIesIntensity, Area.IesIntensityScale, Area.Intensity, Ies);
		const float Bounds     = AreaLightMath::ComputeBoundingRadius(Type, Area.Radius, HalfWidth, HalfHeight);
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Area.Color, Intensity, Area.Radius, Bounds, Area.bCastShadows))
		{
			SetAxes(*Light, Transform);
			const float                  Extent = FMath::Max(HalfWidth, HalfHeight);
			const LightMath::FConeParams Cone   = AreaLightMath::ComputeBarnDoorCone(Area.BarnDoorAngle, Area.BarnDoorLength, Extent);
			Light->Type         = static_cast<uint32>(Type);
			Light->Color        = Light->Color * AreaLightMath::IntensityToRadianceScale(AreaLightMath::ComputeArea(Type, HalfWidth, HalfHeight));
			Light->HalfWidth    = HalfWidth;
			Light->HalfHeight   = HalfHeight;
			Light->ConeScale    = Cone.Scale;
			Light->ConeOffset   = Cone.Offset;
			Light->Flags        = Area.bTwoSided ? LightMath::LocalLightFlag_TwoSided : 0u;
			Light->SourceRadius = Type == LightMath::ELocalLightType::Disc ? Extent : std::sqrt(HalfWidth * HalfWidth + HalfHeight * HalfHeight);
			Light->IesTexture   = Ies;
			// 그림자·쿠키 투영 반각 = 문 덮개 각 (없으면 상한 80°)
			const float Projection   = FMath::Min(Area.BarnDoorAngle, AreaLightMath::MaxCookieAngle);
			LightOuterAngles.back() = Projection;
			SetCookie(*Light, Area.CookieTexture, Type, Projection, Area.CookieScale, Area.CookiePanSpeed);
			++AreaLightCount;
		}
	});

	// 방향광 쿠키 (씬의 첫 방향광 — PerFrame 방향광과 같은 것)
	bool bDirectionalFound = false;
	Registry.View<FTransformComponent, FDirectionalLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FDirectionalLightComponent& Light) {
		if (bDirectionalFound)
		{
			return;
		}
		bDirectionalFound = true;
		DirectionalCookie = ResolveCookieTexture(Light.CookieTexture);
		if (DirectionalCookie >= 0)
		{
			const FVector2 Offset(Light.CookiePanSpeed.X * Time - std::floor(Light.CookiePanSpeed.X * Time),
			                      Light.CookiePanSpeed.Y * Time - std::floor(Light.CookiePanSpeed.Y * Time));
			AreaLightMath::ComputeDirectionalCookieAxes(Transform.GetWorldForward(), Light.CookieTileSize, Offset, DirectionalCookieU, DirectionalCookieV);
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
		auto Keep = [&Order](auto& Values) {
			std::remove_reference_t<decltype(Values)> Kept;
			Kept.reserve(Order.size());
			for (const uint32 Index : Order)
			{
				Kept.push_back(Values[Index]);
			}
			Values.swap(Kept);
		};
		Keep(Lights);
		Keep(LightScores);
		Keep(LightOuterAngles);
		Keep(LightShadowRadius);
		Keep(LightWantsShadow);
	}
}

void FLocalLightRenderer::AssignShadows(const FLocalShadowSettings& Settings)
{
	ShadowSlices.clear();
	ShadowMatrices.clear();
	if (!Settings.bEnabled)
	{
		return;
	}

	// 카메라에 가까운 그림자 라이트부터 장을 배정 (스포트 1장, 점광원 6장). 모자라면 그 라이트는 그림자 없이
	std::vector<uint32> Candidates;
	for (uint32 Index = 0; Index < Lights.size(); ++Index)
	{
		if (LightWantsShadow[Index] != 0)
		{
			Candidates.push_back(Index);
		}
	}
	std::sort(Candidates.begin(), Candidates.end(), [&](uint32 A, uint32 B) { return LightScores[A] < LightScores[B]; });

	const uint32 Resolution = FMath::Clamp<uint32>(Settings.Resolution, 64, 4096);
	const uint32 MaxSlices  = FMath::Clamp<uint32>(Settings.MaxSlices, 1, 2048);
	const float  NearZ      = FMath::Max(Settings.NearZ, 0.1f);
	for (const uint32 Index : Candidates)
	{
		FLocalLightGpuData& Light  = Lights[Index];
		const bool          bPoint = Light.Type == static_cast<uint32>(LightMath::ELocalLightType::Point);
		const uint32        Needed = bPoint ? LightMath::CubeFaceCount : 1u;
		if (ShadowSlices.size() + Needed > MaxSlices)
		{
			continue; // 더 작은 스포트는 들어갈 수 있다
		}
		Light.ShadowIndex = static_cast<int32>(ShadowSlices.size());
		if (bPoint)
		{
			Light.ShadowTexelFactor = LightMath::ShadowTexelWorldFactor(LightMath::CubeFaceTanHalfFov(Resolution), Resolution);
			for (uint32 Face = 0; Face < LightMath::CubeFaceCount; ++Face)
			{
				ShadowSlices.push_back({ LightMath::ComputeCubeFaceViewProjection(Light.Position, Face, Light.Radius, NearZ, Resolution),
				                         Light.Position, Light.Radius });
			}
		}
		else
		{
			// 스포트와 면광원 (면 가운데에서 법선 쪽 원근 1장 — 반각 = 문 덮개 각, 상한 80°. 원평면 = 경계 구)
			const float Outer = FMath::Clamp(LightOuterAngles[Index], 1.0f, LightMath::MaxSpotConeAngle);
			const float Tan   = FMath::Tan(FMath::DegreesToRadians(Outer)) * LightMath::CubeFaceTanHalfFov(Resolution);
			const float Far   = LightShadowRadius[Index];
			Light.ShadowTexelFactor = LightMath::ShadowTexelWorldFactor(Tan, Resolution);
			Light.ShadowFar         = FMath::Max(Far, NearZ * 2.0f);
			ShadowSlices.push_back({ LightMath::ComputeSpotViewProjection(Light.Position, Light.Direction, Outer, Far, NearZ, Resolution),
			                         Light.Position, Far });
		}
	}
	for (FShadowSlice& Slice : ShadowSlices)
	{
		Slice.Frustum     = FFrustum::FromViewProjection(Slice.ViewProjection);
		Slice.LightBounds = FBox(Slice.LightPosition - FVector3(Slice.Radius), Slice.LightPosition + FVector3(Slice.Radius));
		ShadowMatrices.push_back(Slice.ViewProjection);
	}
}

bool FLocalLightRenderer::IntersectsShadowCaster(const FBox& WorldBounds) const
{
	for (const FShadowSlice& Slice : ShadowSlices)
	{
		if (Slice.IsCaster(WorldBounds))
		{
			return true;
		}
	}
	return false;
}

void FLocalLightRenderer::RecordShadows(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes)
{
	const uint32 Resolution = ShadowMapResolution;

	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Resolution), static_cast<float>(Resolution), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Resolution), static_cast<LONG>(Resolution) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
	CommandList->SetGraphicsRootSignature(ShadowRootSignature.Get());
	CommandList->SetPipelineState(ShadowPipelines[0].Get());

	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_Instances, Instances.GetGpuData());
	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_SkinPalette, SkinPalettes);
	FD3D12DynamicUploadBuffer&        DynamicBuffer = Rhi->GetDynamicBuffer();
	const std::vector<FMeshInstance>& List          = Instances.GetInstances();
	FDepthPassBindings                Bindings;
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		Bindings.Pipelines[Variant] = ShadowPipelines[Variant].Get();
	}
	Bindings.InstanceRootIndex  = ShadowParam_PassConstants;
	Bindings.InstanceDestOffset = 16;
	Bindings.MaskRootIndex      = ShadowParam_MaskConstants;
	Bindings.MaskTextureRoot    = ShadowParam_MaskTexture;
	Bindings.MaterialPipelines    = &MaterialPipelines;
	Bindings.DynamicBuffer        = &Rhi->GetDynamicBuffer();
	Bindings.MaterialConstantRoot = ShadowParam_MaterialConstants;
	Bindings.MaterialTextureRoot  = ShadowParam_MaterialTextures;
	for (uint32 Index = 0; Index < ShadowSlices.size(); ++Index)
	{
		const FShadowSlice&               Slice = ShadowSlices[Index];
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv   = ShadowDsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		// (정적/스킨 × 불투명/Masked)·메시·LOD별 묶음 (Masked만 머티리얼별, 반투명 제외), 스킨은 팔레트가 바로 월드로 보내므로 상수는 뷰-투영 그대로
		ShadowBatches.Reset();
		for (uint32 InstanceIndex = 0; InstanceIndex < static_cast<uint32>(List.size()); ++InstanceIndex)
		{
			const FMeshInstance& Instance = List[InstanceIndex];
			if (Instance.CastsShadow() && Slice.IsCaster(Instance.WorldBounds))
			{
				ShadowBatches.Add(MakeDepthBatchKey(Instance), 0.0f, InstanceIndex);
			}
		}
		ShadowBatches.Finalize(DynamicBuffer);

		CommandList->SetGraphicsRoot32BitConstants(ShadowParam_PassConstants, 16, &Slice.ViewProjection.M[0][0], 0);
		CommandList->SetGraphicsRootShaderResourceView(ShadowParam_InstanceIndices, ShadowBatches.GetIndexBuffer());
		DrawDepthBatches(CommandList, ShadowBatches, Instances, Bindings, ShadowDrawCalls, ShadowTriangles);
	}
	// 추가 캐스터 (지형 등): 장마다 DSV를 다시 바인딩해 그린다
	for (uint32 Index = 0; ExtraCasters && Index < ShadowSlices.size(); ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = ShadowDsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		ExtraCasters(CommandList, ShadowSlices[Index].ViewProjection, ShadowSlices[Index].Frustum, true);
	}
}

void FLocalLightRenderer::PrepareLights(FScene& Scene, const FCamera& Camera, const FLocalShadowSettings& ShadowSettings)
{
	E_CHECKF(Rhi != nullptr, "로컬 라이트 렌더러가 초기화되지 않았습니다");

	CollectLights(Scene, Camera);

	// ---- 그림자: 바이어스는 PSO에 고정되므로 바뀌면 재생성, 타일 배열은 필요한 만큼 (8장 단위로 키운다)
	if (ShadowSettings.DepthBias != BakedDepthBias || ShadowSettings.SlopeBias != BakedSlopeBias)
	{
		BakedDepthBias = ShadowSettings.DepthBias;
		BakedSlopeBias = ShadowSettings.SlopeBias;
		ReloadShaders(false);
	}
	AssignShadows(ShadowSettings);
	if (!ShadowSlices.empty())
	{
		const uint32 Resolution = FMath::Clamp<uint32>(ShadowSettings.Resolution, 64, 4096);
		const uint32 Needed     = static_cast<uint32>(ShadowSlices.size());
		const uint32 Capacity   = FMath::Max(ShadowMapResolution == Resolution ? ShadowMapSlices : 0u, (Needed + 7u) / 8u * 8u);
		if (!EnsureShadowMap(Resolution, Capacity))
		{
			// 만들지 못하면 이번 프레임은 그림자 없이
			ShadowSlices.clear();
			ShadowMatrices.clear();
			for (FLocalLightGpuData& Light : Lights)
			{
				Light.ShadowIndex = -1;
			}
			EnsureShadowMap(1, 1);
		}
	}
}

void FLocalLightRenderer::PrepareFrame(const FCamera& Camera, uint32 Width, uint32 Height, const FLocalShadowSettings& ShadowSettings)
{
	E_CHECKF(Rhi != nullptr, "로컬 라이트 렌더러가 초기화되지 않았습니다");

	// ---- 상수
	const float NearZ = Camera.GetNearZ();
	const float FarZ  = FMath::Max(Camera.GetFarZ(), NearZ * 2.0f);
	const LightMath::FSliceParams Slices = LightMath::ComputeSliceParams(NearZ, FarZ, LightMath::ClusterGridZ);

	FClusterConstants Constants; // 프레임마다 새로 (지역 — 정렬 지정 멤버를 두지 않는다)
	Constants.View               = Camera.GetViewMatrix();
	Constants.GridX              = LightMath::ClusterGridX;
	Constants.GridY              = LightMath::ClusterGridY;
	Constants.GridZ              = LightMath::ClusterGridZ;
	Constants.LightCount         = static_cast<uint32>(Lights.size());
	Constants.ScreenSize         = FVector2(static_cast<float>(FMath::Max<uint32>(Width, 1)), static_cast<float>(FMath::Max<uint32>(Height, 1)));
	Constants.SliceScale         = Slices.Scale;
	Constants.SliceBias          = Slices.Bias;
	Constants.NearZ              = NearZ;
	Constants.FarZ               = FarZ;
	Constants.bOrthographic      = Camera.IsOrthographic() ? 1u : 0u;
	Constants.ShadowNormalOffset = ShadowSettings.NormalOffset;
	Constants.ShadowTexelSize    = 1.0f / static_cast<float>(FMath::Max<uint32>(ShadowMapResolution, 1));
	Constants.ShadowNearZ        = FMath::Max(ShadowSettings.NearZ, 0.1f);
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		const FD3D12Texture* Ltc = Resources->GetTexture(LtcTextures[Index]);
		(Index == 0 ? Constants.LtcTexture1 : Constants.LtcTexture2) = Ltc != nullptr ? Ltc->GetSrv().Index : 0u;
	}
	Constants.DirectionalCookieTexture = DirectionalCookie;
	Constants.DirectionalCookieU       = DirectionalCookieU;
	Constants.DirectionalCookieV       = DirectionalCookieV;
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
	auto Upload = [&DynamicBuffer](const void* Data, size_t Bytes, size_t MinBytes) {
		const size_t                  Size       = FMath::Max(Bytes, MinBytes);
		const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(Size, 16);
		std::memset(Allocation.CpuAddress, 0, Size);
		if (Bytes > 0)
		{
			std::memcpy(Allocation.CpuAddress, Data, Bytes);
		}
		return Allocation.GpuAddress;
	};
	LightListAddress      = Upload(Lights.data(), sizeof(FLocalLightGpuData) * Lights.size(), sizeof(FLocalLightGpuData));
	ShadowMatricesAddress = Upload(ShadowMatrices.data(), sizeof(FMatrix4x4) * ShadowMatrices.size(), sizeof(FMatrix4x4));
	ConstantsAddress      = DynamicBuffer.AllocateConstants(Constants).GpuAddress;
}

void FLocalLightRenderer::AddPasses(FRenderGraph& Graph, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
                                    ID3D12RootSignature* BreakRootSignature, int32 Timer)
{
	ShadowDrawCalls = 0;
	ShadowTriangles = 0;
	if (!ShadowSlices.empty())
	{
		// 장마다 지우고 그린다 (쓰지 않는 장은 셰이더가 읽지 않음)
		Graph.AddPass("로컬 그림자")
			.Write(ImportShadowMap(Graph), ERGAccess::DepthWrite, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, &Instances, SkinPalettes](FRGContext& Context) { RecordShadows(Context.CommandList, Instances, SkinPalettes); });
	}

	// ---- 클러스터 컬링 (전체 클러스터를 다시 쓴다)
	Graph.AddPass("클러스터 컬링")
		.Write(ImportClusters(Graph), ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, BreakRootSignature](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			if (BreakRootSignature != nullptr && !Context.bAsyncCompute)
			{
				CommandList->SetGraphicsRootSignature(BreakRootSignature);
			}
			CommandList->SetComputeRootSignature(ComputeRootSignature.Get());
			CommandList->SetPipelineState(CullPipeline.Get());
			CommandList->SetComputeRootConstantBufferView(ComputeParam_Constants, ConstantsAddress);
			CommandList->SetComputeRootShaderResourceView(ComputeParam_Lights, LightListAddress);
			CommandList->SetComputeRootUnorderedAccessView(ComputeParam_Clusters, ClusterBuffer->GetGPUVirtualAddress());
			CommandList->Dispatch((LightMath::ClusterCount + GCullGroupSize - 1) / GCullGroupSize, 1, 1);
		});
}
