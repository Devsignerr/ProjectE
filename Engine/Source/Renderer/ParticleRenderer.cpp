#include "Renderer/ParticleRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/Image.h"
#include "Renderer/ParticleBounds.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Components.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>

E_DECLARE_LOG_CATEGORY(LogRenderer)

// GPU 이미터의 입자 풀 (이미터 인스턴스가 소유, 렌더러가 만든다)
struct FParticleGpuBuffer final : public IParticleGpuState
{
	ComPtr<ID3D12Resource> Buffer;
	uint32                 Capacity = 0;
	D3D12_RESOURCE_STATES  State    = D3D12_RESOURCE_STATE_COMMON;
	FD3D12RHI*             Rhi      = nullptr; // 렌더러 종료 후엔 nullptr (이미 해제됨)

	// 화면 밖이라 미룬 계산 요청 (FParticleRenderer 머리 주석의 규칙) + 각 요청 끝 시각(이미터 절대 시간)
	std::vector<FParticleGpuStep> Deferred;
	std::vector<float>            DeferredTimes;
	// 월드 공간 이미터의 최근 위치 (최대 수명 동안 — 경계에 포함): (이미터 절대 시간, 위치)
	std::deque<std::pair<float, FVector3>> Trail;

	~FParticleGpuBuffer() override
	{
		if (Rhi != nullptr && Buffer)
		{
			Rhi->DeferRelease(Buffer);
		}
	}

	void Transition(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After)
	{
		if (State != After)
		{
			const D3D12_RESOURCE_BARRIER Barrier = MakeTransitionBarrier(Buffer.Get(), State, After);
			CommandList->ResourceBarrier(1, &Barrier);
			State = After;
		}
	}
};

namespace
{
	constexpr uint64 GUploadReserveBytes = 512 * 1024;
	constexpr uint32 GSimulateGroupSize  = 64;

	enum ERootParameter : uint32
	{
		RootParam_Frame     = 0, // b0
		RootParam_Draw      = 1, // b1
		RootParam_Particles = 2, // t1 (루트 SRV)
		RootParam_Texture   = 3, // t0
		RootParam_Fog       = 4, // b2 (안개 상수, 정점)
		RootParam_FogVolume = 5, // t2 (볼류메트릭 안개 결과, 정점)
	};
	enum EComputeRootParameter : uint32
	{
		ComputeParam_Constants = 0, // b0
		ComputeParam_Program   = 1, // t0
		ComputeParam_Particles = 2, // u0
	};

	FShaderCompileDesc MakeDesc(const wchar_t* File, const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = File;
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}

	// 가운데가 밝고 가장자리로 부드럽게 사라지는 흰 원 (알파 = 1 - smoothstep)
	FImage MakeSoftCircle(uint32 Size)
	{
		FImage Image;
		Image.Width  = Size;
		Image.Height = Size;
		Image.Pixels.resize(static_cast<size_t>(Size) * Size * FImage::BytesPerPixel);
		for (uint32 Y = 0; Y < Size; ++Y)
		{
			for (uint32 X = 0; X < Size; ++X)
			{
				const float U     = (static_cast<float>(X) + 0.5f) / static_cast<float>(Size) * 2.0f - 1.0f;
				const float V     = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Size) * 2.0f - 1.0f;
				const float T     = FMath::Min(std::sqrt(U * U + V * V), 1.0f);
				const float Alpha = 1.0f - T * T * (3.0f - 2.0f * T);
				uint8*      Pixel = &Image.Pixels[(static_cast<size_t>(Y) * Size + X) * FImage::BytesPerPixel];
				Pixel[0] = Pixel[1] = Pixel[2] = 255;
				Pixel[3]                       = static_cast<uint8>(FMath::Clamp(Alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
		return Image;
	}

	FParticleGpuData ToGpu(const FParticle& Particle)
	{
		FParticleGpuData Data;
		Data.Position   = Particle.Position;
		Data.Age        = Particle.Age;
		Data.Velocity   = Particle.Velocity;
		Data.Lifetime   = Particle.Lifetime;
		Data.BaseColor  = Particle.BaseColor;
		Data.Color      = Particle.Color;
		Data.BaseSize   = Particle.BaseSize;
		Data.Size       = Particle.Size;
		Data.Rotation   = Particle.Rotation;
		Data.Mass       = Particle.Mass;
		Data.SubImage   = Particle.SubImage;
		Data.SpawnIndex = Particle.SpawnIndex;
		return Data;
	}

	float AsFloat(uint32 Value) { return std::bit_cast<float>(Value); }

	// 화면 밖 동안 미룬 요청 상한 (넘으면 이웃끼리 합쳐 절반으로 — 근사)
	constexpr size_t GMaxDeferredSteps = 64;

	// 이미터 절대 시간 (반복 횟수 포함, 컴포넌트 Speed가 이미 반영된 값)
	float GetEmitterAbsoluteTime(const FParticleEmitter& Emitter, const FParticleEmitterInstance& Instance)
	{
		return Instance.EmitterTime + static_cast<float>(Instance.LoopIndex) * FMath::Max(Emitter.Duration, 0.01f);
	}

	// 메시 렌더러 메시의 원점 기준 반경 최댓값 (없으면 0)
	float GetMaxMeshRadius(const FParticleEmitter& Emitter, const FResourceManager& Resources)
	{
		float Radius = 0.0f;
		for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
		{
			if (Renderer.bEnabled && Renderer.Type == EParticleRendererType::Mesh)
			{
				if (const FStaticMesh* Mesh = Resources.GetMesh(Renderer.Mesh))
				{
					const FBox& Bounds = Mesh->GetLocalBounds();
					Radius = FMath::Max(Radius, FMath::Max(Bounds.Min.Length(), Bounds.Max.Length()));
				}
			}
		}
		return Radius;
	}

	// GPU 이미터 월드 경계: 고정 경계(이미터 로컬 상자) 또는 추정 반경 구. 월드 공간 이미터는 최근 위치 자취만큼 넓힌다
	FBox ComputeGpuEmitterBounds(const FParticleRuntime& Runtime, const FParticleEmitter& Emitter, const FParticleGpuBuffer* Pool,
	                             const FResourceManager& Resources)
	{
		const FMatrix4x4& World    = Runtime.LastWorld;
		const FVector3    Position = World.TransformPosition(FVector3::ZeroVector);
		FBox              Current;
		if (Emitter.bFixedBounds)
		{
			Current = FBox(Emitter.FixedBoundsMin, Emitter.FixedBoundsMax).TransformBy(World);
		}
		else
		{
			const float Radius = ParticleBounds::EstimateLocalRadius(Emitter, GetMaxMeshRadius(Emitter, Resources)) * ParticleBounds::MaxAxisScale(World);
			Current            = FBox(Position - FVector3(Radius), Position + FVector3(Radius));
		}
		FBox Result = Current;
		if (!Emitter.bLocalSpace && Pool != nullptr)
		{
			for (const auto& [Time, TrailPosition] : Pool->Trail)
			{
				const FVector3 Shift = TrailPosition - Position;
				Result.AddBox(FBox(Current.Min + Shift, Current.Max + Shift));
			}
		}
		return Result;
	}
} // namespace

FParticleRenderer::~FParticleRenderer()
{
	Shutdown();
}

bool FParticleRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT InColorFormat,
                             DXGI_FORMAT InDepthFormat)
{
	E_CHECKF(Rhi == nullptr, "파티클 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	Resources     = &InResources;
	ColorFormat   = InColorFormat;
	DepthFormat   = InDepthFormat;

	const uint32 FrameIndex     = RootSignature.AddConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 DrawIndex      = RootSignature.AddConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 ParticlesIndex = RootSignature.AddShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 TextureIndex   = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) },
	                                                               D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(FrameIndex == RootParam_Frame && DrawIndex == RootParam_Draw && ParticlesIndex == RootParam_Particles && TextureIndex == RootParam_Texture);
	const uint32 FogIndex       = RootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 FogVolumeIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_VERTEX);
	E_CHECK(FogIndex == RootParam_Fog && FogVolumeIndex == RootParam_FogVolume);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
	                                                                      D3D12_SHADER_VISIBILITY_VERTEX)); // 안개 볼륨
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"ParticleRootSignature"))
	{
		return false;
	}

	const uint32 ConstantsIndex = ComputeRootSignature.AddConstantBufferView(0);
	const uint32 ProgramIndex   = ComputeRootSignature.AddShaderResourceView(0);
	const uint32 UavIndex       = ComputeRootSignature.AddUnorderedAccessView(0);
	E_CHECK(ConstantsIndex == ComputeParam_Constants && ProgramIndex == ComputeParam_Program && UavIndex == ComputeParam_Particles);
	if (!ComputeRootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ParticleSimulateRootSignature"))
	{
		return false;
	}

	if (!CreatePipelines(Pipelines, false))
	{
		return false;
	}
	DefaultTexture = Resources->CreateTexture(MakeSoftCircle(64), true, L"ParticleSoftCircle");
	return DefaultTexture.IsValid();
}

void FParticleRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 이미터가 아직 들고 있는 GPU 버퍼는 지금 지연 해제하고, 나중에 소멸될 때는 아무것도 하지 않게 한다
	for (const std::weak_ptr<FParticleGpuBuffer>& Weak : GpuBuffers)
	{
		if (const std::shared_ptr<FParticleGpuBuffer> Buffer = Weak.lock())
		{
			if (Buffer->Buffer)
			{
				Rhi->DeferRelease(Buffer->Buffer);
			}
			Buffer->Buffer.Reset();
			Buffer->Rhi = nullptr;
		}
	}
	GpuBuffers.clear();

	for (auto& Kind : Pipelines.Graphics)
	{
		for (FD3D12PipelineState& Pipeline : Kind)
		{
			Pipeline.Shutdown();
		}
	}
	Pipelines.Simulate.Shutdown();
	RootSignature.Shutdown();
	ComputeRootSignature.Shutdown();
	// 기본 텍스처는 리소스 관리자가 종료 시 함께 해제한다
	DefaultTexture = FTextureHandle{};
	Rhi            = nullptr;
	ShaderLibrary  = nullptr;
	Resources      = nullptr;
}

bool FParticleRenderer::CreatePipelines(FPipelineSet& Out, bool bForceRecompile)
{
	const FShaderCompileDesc Descs[] = {
		MakeDesc(L"Particle.hlsl", L"VSSprite", EShaderStage::Vertex),        MakeDesc(L"Particle.hlsl", L"VSMesh", EShaderStage::Vertex),
		MakeDesc(L"Particle.hlsl", L"VSRibbon", EShaderStage::Vertex),        MakeDesc(L"Particle.hlsl", L"PSAlpha", EShaderStage::Pixel),
		MakeDesc(L"Particle.hlsl", L"PSAdditive", EShaderStage::Pixel),       MakeDesc(L"ParticleSimulate.hlsl", L"CSMain", EShaderStage::Compute),
	};
	constexpr size_t ShaderCount = std::size(Descs);
	ComPtr<IDxcBlob> Blobs[ShaderCount];
	for (size_t Index = 0; Index < ShaderCount; ++Index)
	{
		if (bForceRecompile && !ShaderLibrary->CookShader(Descs[Index]))
		{
			return false;
		}
		Blobs[Index] = ShaderLibrary->GetShader(Descs[Index]);
		if (!Blobs[Index])
		{
			return false;
		}
	}

	ID3D12Device*                                     Device        = Rhi->GetDevice().GetDevice();
	const std::vector<D3D12_INPUT_ELEMENT_DESC>       RibbonLayout  = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(FParticleRibbonVertex, Position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(FParticleRibbonVertex, UV), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(FParticleRibbonVertex, Color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	const std::vector<D3D12_INPUT_ELEMENT_DESC>* Layouts[Pipeline_Count] = { nullptr, &FStaticMesh::GetInputLayout(), &RibbonLayout };
	const wchar_t* Names[Pipeline_Count][2] = { { L"ParticleSpriteAlpha", L"ParticleSpriteAdditive" },
		                                        { L"ParticleMeshAlpha", L"ParticleMeshAdditive" },
		                                        { L"ParticleRibbonAlpha", L"ParticleRibbonAdditive" } };

	for (uint32 Kind = 0; Kind < Pipeline_Count; ++Kind)
	{
		FGraphicsPipelineDesc Desc;
		Desc.RootSignature = RootSignature.Get();
		Desc.VertexShader  = FD3D12ShaderCompiler::ToBytecode(Blobs[Kind].Get());
		if (Layouts[Kind] != nullptr)
		{
			Desc.InputLayout = *Layouts[Kind];
		}
		Desc.RenderTargetFormats[0] = ColorFormat;
		Desc.DepthStencilFormat     = DepthFormat;
		Desc.CullMode               = D3D12_CULL_MODE_NONE;
		Desc.bDepthEnable           = true;
		Desc.bDepthWrite            = false; // 불투명 메시에 가려지기만 한다
		Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		for (uint32 Blend = 0; Blend < 2; ++Blend)
		{
			Desc.PixelShader = FD3D12ShaderCompiler::ToBytecode(Blobs[3 + Blend].Get());
			Desc.BlendMode   = Blend == 0 ? EBlendMode::Alpha : EBlendMode::Additive;
			if (!Out.Graphics[Kind][Blend].InitGraphics(Device, Desc, Names[Kind][Blend]))
			{
				return false;
			}
		}
	}
	return Out.Simulate.InitCompute(Device, ComputeRootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Blobs[5].Get()), L"ParticleSimulate");
}

bool FParticleRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FPipelineSet New;
	if (!CreatePipelines(New, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "파티클 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	const auto SwapOne = [&](FD3D12PipelineState& Current, FD3D12PipelineState& Next) {
		Current.Swap(Next);
		Rhi->DeferRelease(Next.Detach());
	};
	for (uint32 Kind = 0; Kind < Pipeline_Count; ++Kind)
	{
		for (uint32 Blend = 0; Blend < 2; ++Blend)
		{
			SwapOne(Pipelines.Graphics[Kind][Blend], New.Graphics[Kind][Blend]);
		}
	}
	SwapOne(Pipelines.Simulate, New.Simulate);
	return true;
}

void FParticleRenderer::BuildGpuProgram(const FParticleEmitter& Emitter, std::vector<FVector4>& OutProgram, FParticleSimConstants& OutConstants)
{
	OutProgram.clear();
	// 곡선 키는 모듈 뒤에 모아 둔다 (입력 머리의 키 위치를 나중에 채움)
	struct FPendingCurve
	{
		size_t                HeaderIndex;
		const FParticleValue* Value;
	};
	std::vector<FPendingCurve> Curves;

	const auto EmitStage = [&](EParticleStage Stage, uint32& OutCount, uint32& OutOffset) {
		OutOffset       = static_cast<uint32>(OutProgram.size());
		OutCount        = 0;
		const auto& Modules = Emitter.GetStage(Stage);
		for (size_t Index = 0; Index < Modules.size(); ++Index)
		{
			const FParticleModule& Module = Modules[Index];
			if (!Module.bEnabled)
			{
				continue;
			}
			const uint32 InputCount = static_cast<uint32>(Module.Inputs.size());
			OutProgram.emplace_back(AsFloat(static_cast<uint32>(Module.Type)), AsFloat(InputCount), AsFloat(static_cast<uint32>(Index)), 0.0f);
			for (const FParticleValue& Value : Module.Inputs)
			{
				const size_t HeaderIndex = OutProgram.size();
				OutProgram.emplace_back(AsFloat(static_cast<uint32>(Value.Mode)), AsFloat(0), AsFloat(0), 0.0f);
				OutProgram.push_back(Value.A);
				OutProgram.push_back(Value.B);
				if (Value.Mode == EParticleValueMode::Curve)
				{
					Curves.push_back({ HeaderIndex, &Value });
				}
			}
			++OutCount;
		}
	};
	EmitStage(EParticleStage::ParticleSpawn, OutConstants.SpawnModuleCount, OutConstants.SpawnOffset);
	EmitStage(EParticleStage::ParticleUpdate, OutConstants.UpdateModuleCount, OutConstants.UpdateOffset);

	for (const FPendingCurve& Curve : Curves)
	{
		const uint32 KeyOffset = static_cast<uint32>(OutProgram.size());
		for (const FParticleCurveKey& Key : Curve.Value->Curve)
		{
			OutProgram.emplace_back(Key.Time, 0.0f, 0.0f, 0.0f);
			OutProgram.push_back(Key.Value);
		}
		OutProgram[Curve.HeaderIndex].Y = AsFloat(static_cast<uint32>(Curve.Value->Curve.size()));
		OutProgram[Curve.HeaderIndex].Z = AsFloat(KeyOffset);
	}
	if (OutProgram.empty())
	{
		OutProgram.emplace_back(); // 빈 버퍼 바인딩 방지
	}
	OutConstants.Seed        = Emitter.Seed;
	OutConstants.bLocalSpace = Emitter.bLocalSpace ? 1u : 0u;
	OutConstants.Capacity    = Emitter.MaxParticles;
}

void FParticleRenderer::Simulate(FScene& Scene, const FFrustum& CullFrustum)
{
	CulledGpuEmitters = 0;
	if (Rhi == nullptr)
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	bool                       bBound        = false;

	Scene.GetRegistry().View<FParticleSystemComponent>().Each([&](FEntity, FParticleSystemComponent& Component) {
		FParticleRuntime& Runtime = Component.Runtime;
		if (!Runtime.System)
		{
			return;
		}
		const FParticleSystemAsset& System = *Runtime.System;
		for (size_t Index = 0; Index < System.Emitters.size() && Index < Runtime.Emitters.size(); ++Index)
		{
			const FParticleEmitter&   Emitter  = System.Emitters[Index];
			FParticleEmitterInstance& Instance = Runtime.Emitters[Index];
			auto* ExistingPool = static_cast<FParticleGpuBuffer*>(Instance.GpuState.get());
			if (Emitter.SimTarget != EParticleSimTarget::GPU ||
			    (Instance.PendingGpuSteps.empty() && (ExistingPool == nullptr || ExistingPool->Deferred.empty())))
			{
				continue;
			}

			// 풀 준비 (처음이거나 최대 개수가 바뀌었거나 다른 렌더러가 종료하며 비웠으면 새로)
			auto* Pool = static_cast<FParticleGpuBuffer*>(Instance.GpuState.get());
			if (Pool == nullptr || !Pool->Buffer || Pool->Capacity != Emitter.MaxParticles)
			{
				auto NewPool      = std::make_shared<FParticleGpuBuffer>();
				NewPool->Capacity = Emitter.MaxParticles;
				NewPool->Rhi      = Rhi;
				// 커밋 리소스는 0으로 초기화된다 (Age = Lifetime = 0 → 모두 죽은 입자)
				const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
				const D3D12_RESOURCE_DESC   Desc = MakeBufferDesc(sizeof(FParticleGpuData) * static_cast<uint64>(NewPool->Capacity),
				                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				if (FAILED(Rhi->GetDevice().GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
				                                                                 IID_PPV_ARGS(&NewPool->Buffer))))
				{
					E_LOG(LogRenderer, Error, "GPU 파티클 버퍼를 만들지 못했습니다 (최대 {}개)", NewPool->Capacity);
					Instance.PendingGpuSteps.clear();
					continue;
				}
				NewPool->Buffer->SetName(L"GpuParticlePool");
				std::erase_if(GpuBuffers, [](const std::weak_ptr<FParticleGpuBuffer>& Weak) { return Weak.expired(); });
				GpuBuffers.push_back(NewPool);
				Instance.GpuState = NewPool;
				Pool              = NewPool.get();
			}

			// 새 요청을 풀의 대기열로 옮긴다 (요청마다 끝 시각을 거꾸로 계산 — 계산 셰이더 Time 입력)
			const float Now      = GetEmitterAbsoluteTime(Emitter, Instance);
			const float Lifetime = FMath::Max(ParticleBounds::MaxLifetime(Emitter), 0.0f);
			{
				float StepEnd = Now;
				const size_t First = Pool->Deferred.size();
				for (size_t Step = Instance.PendingGpuSteps.size(); Step-- > 0;)
				{
					Pool->DeferredTimes.insert(Pool->DeferredTimes.begin() + static_cast<std::ptrdiff_t>(First), StepEnd);
					StepEnd -= Instance.PendingGpuSteps[Step].DeltaSeconds;
				}
				for (const FParticleGpuStep& Step : Instance.PendingGpuSteps)
				{
					Pool->Deferred.push_back(Step);
					if (!Emitter.bLocalSpace)
					{
						// 자취는 성기게 (수명의 1/32 간격, 움직이지 않으면 시각만 갱신) — 사이 위치는 경계 상자 크기가 덮는다
						const float    Time     = Pool->DeferredTimes[Pool->Deferred.size() - 1];
						const FVector3 Position = Step.EmitterWorld.TransformPosition(FVector3::ZeroVector);
						if (!Pool->Trail.empty() && FVector3::DistanceSquared(Pool->Trail.back().second, Position) < 1.0f)
						{
							Pool->Trail.back().first = Time;
						}
						else if (Pool->Trail.empty() || Time - Pool->Trail.back().first >= Lifetime / 32.0f)
						{
							Pool->Trail.emplace_back(Time, Position);
						}
					}
				}
				Instance.PendingGpuSteps.clear();
				// 자취: 최대 수명보다 오래된 위치와 되감긴 시간(재시작)은 버린다
				while (!Pool->Trail.empty() && (Now - Pool->Trail.front().first > Lifetime || Pool->Trail.front().first > Now))
				{
					Pool->Trail.pop_front();
				}
			}

			// 화면 밖: 계산을 미룬다. 최대 수명을 넘는 오래된 요청은 버리고(정확 — 아래 머리 주석), 너무 많으면 이웃끼리 합친다
			if (bEnableCulling && !CullFrustum.Intersects(ComputeGpuEmitterBounds(Runtime, Emitter, Pool, *Resources)))
			{
				++CulledGpuEmitters;
				ParticleBounds::TrimDeferredSteps(Pool->Deferred, Pool->DeferredTimes, Lifetime, GMaxDeferredSteps, Emitter.MaxParticles);
				continue;
			}

			FParticleSimConstants Constants;
			BuildGpuProgram(Emitter, ProgramScratch, Constants);
			const uint64                  ProgramBytes = sizeof(FVector4) * ProgramScratch.size();
			const FD3D12DynamicAllocation Program      = DynamicBuffer.Allocate(ProgramBytes, 16);
			std::memcpy(Program.CpuAddress, ProgramScratch.data(), ProgramBytes);

			if (!bBound)
			{
				CommandList->SetComputeRootSignature(ComputeRootSignature.Get());
				CommandList->SetPipelineState(Pipelines.Simulate.Get());
				bBound = true;
			}
			Pool->Transition(CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			CommandList->SetComputeRootShaderResourceView(ComputeParam_Program, Program.GpuAddress);
			CommandList->SetComputeRootUnorderedAccessView(ComputeParam_Particles, Pool->Buffer->GetGPUVirtualAddress());
			for (size_t StepIndex = 0; StepIndex < Pool->Deferred.size(); ++StepIndex)
			{
				const FParticleGpuStep& Step = Pool->Deferred[StepIndex];
				Constants.EmitterWorld = Step.EmitterWorld;
				Constants.DeltaSeconds = Step.DeltaSeconds;
				Constants.EmitterAlpha = Step.EmitterAlpha;
				Constants.Time         = Pool->DeferredTimes[StepIndex];
				Constants.SpawnStart   = Step.SpawnStart;
				Constants.SpawnCount   = Step.SpawnCount;
				CommandList->SetComputeRootConstantBufferView(ComputeParam_Constants, DynamicBuffer.AllocateConstants(Constants).GpuAddress);
				CommandList->Dispatch((Pool->Capacity + GSimulateGroupSize - 1) / GSimulateGroupSize, 1, 1);
				const D3D12_RESOURCE_BARRIER Barrier = MakeUavBarrier(Pool->Buffer.Get());
				CommandList->ResourceBarrier(1, &Barrier);
			}
			Pool->Deferred.clear();
			Pool->DeferredTimes.clear();
			Pool->Transition(CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
	});
}

uint32 FParticleRenderer::Render(FScene& Scene, const FCamera& Camera, const FFrustum& CullFrustum)
{
	CulledEmitters = 0;
	if (Rhi == nullptr)
	{
		return 0;
	}

	// (컴포넌트 × 이미터 × 렌더러) 수집 → 먼 컴포넌트부터
	struct FDraw
	{
		const FParticleRuntime*          Runtime  = nullptr;
		const FParticleEmitter*          Emitter  = nullptr;
		const FParticleEmitterInstance*  Instance = nullptr;
		const FParticleRendererSettings* Renderer = nullptr;
		float                            DistanceSquared = 0.0f;
		uint32                           Order           = 0; // 같은 컴포넌트 안 순서 유지
	};
	std::vector<FDraw> Draws;
	const FVector3     CameraPosition = Camera.GetPosition();
	Scene.GetRegistry().View<FTransformComponent, FParticleSystemComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FParticleSystemComponent& Component) {
			const FParticleRuntime& Runtime = Component.Runtime;
			if (!Runtime.System)
			{
				return;
			}
			const float Distance = FVector3::DistanceSquared(Transform.GetWorldPosition(), CameraPosition);
			for (size_t Index = 0; Index < Runtime.System->Emitters.size() && Index < Runtime.Emitters.size(); ++Index)
			{
				const FParticleEmitter&         Emitter  = Runtime.System->Emitters[Index];
				const FParticleEmitterInstance& Instance = Runtime.Emitters[Index];
				const bool bGpu = Emitter.SimTarget == EParticleSimTarget::GPU;
				if (!Emitter.bEnabled || (bGpu ? (Instance.GpuState == nullptr) : Instance.Particles.empty()))
				{
					continue;
				}
				// 화면 밖 이미터는 그리지 않는다 (CPU = 실제 입자 경계, GPU = 고정/추정 경계)
				if (bEnableCulling)
				{
					FBox Bounds;
					if (bGpu)
					{
						Bounds = ComputeGpuEmitterBounds(Runtime, Emitter, static_cast<const FParticleGpuBuffer*>(Instance.GpuState.get()), *Resources);
					}
					else
					{
						float       StretchSeconds = 0.0f;
						const float SizeFactor = ParticleBounds::RenderSizeFactor(Emitter, GetMaxMeshRadius(Emitter, *Resources), StretchSeconds);
						Bounds = ParticleBounds::ComputeCpuBounds(Instance.Particles, Emitter.bLocalSpace ? Runtime.LastWorld : FMatrix4x4::Identity,
						                                          SizeFactor, StretchSeconds);
					}
					if (!CullFrustum.Intersects(Bounds))
					{
						++CulledEmitters;
						continue;
					}
				}
				for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
				{
					if (Renderer.bEnabled)
					{
						Draws.push_back({ &Runtime, &Emitter, &Instance, &Renderer, Distance, static_cast<uint32>(Draws.size()) });
					}
				}
			}
		});
	if (Draws.empty())
	{
		return 0;
	}
	std::sort(Draws.begin(), Draws.end(), [](const FDraw& A, const FDraw& B) {
		return A.DistanceSquared != B.DistanceSquared ? A.DistanceSquared > B.DistanceSquared : A.Order < B.Order;
	});

	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	FParticleFrameConstants Frame;
	Frame.ViewProjection = Camera.GetViewProjectionMatrix();
	Frame.CameraRight    = Camera.GetRightVector();
	Frame.CameraUp       = Camera.GetUpVector();
	Frame.CameraPosition = CameraPosition;
	const D3D12_GPU_VIRTUAL_ADDRESS FrameAddress = DynamicBuffer.AllocateConstants(Frame).GpuAddress;

	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Frame, FrameAddress);
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Fog, FogConstants);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_FogVolume, FogVolume.Gpu);

	const FVector3        Forward       = Camera.GetForwardVector();
	const FD3D12Texture&  Fallback      = Resources->ResolveTexture(DefaultTexture);
	ID3D12PipelineState*  BoundPipeline = nullptr;
	uint32                DrawnCount    = 0;
	std::vector<const FParticleEmitterInstance*> CountedInstances;

	// 프레임 업로드 버퍼(용량 고정)를 넘기면 남은 만큼만 그린다 (뒤따르는 패스 상수용 여유를 남긴다)
	const auto AvailableBytes = [&]() {
		const uint64 Max = DynamicBuffer.GetMaxAllocation();
		return Max > GUploadReserveBytes + 256 ? Max - GUploadReserveBytes - 256 : 0;
	};
	const auto WarnFull = [&]() {
		if (!bWarnedBufferFull)
		{
			E_LOG(LogRenderer, Warning, "파티클이 너무 많아 일부를 그리지 않습니다 (프레임 업로드 버퍼 부족)");
			bWarnedBufferFull = true;
		}
	};

	for (const FDraw& Draw : Draws)
	{
		const FParticleEmitter&          Emitter  = *Draw.Emitter;
		const FParticleEmitterInstance&  Instance = *Draw.Instance;
		const FParticleRendererSettings& Settings = *Draw.Renderer;
		const bool                       bGpu     = Emitter.SimTarget == EParticleSimTarget::GPU;
		const bool                       bAlpha   = Settings.BlendMode == EParticleBlendMode::Alpha;
		const FMatrix4x4                 LocalToWorld = Emitter.bLocalSpace ? Draw.Runtime->LastWorld : FMatrix4x4::Identity;

		uint32 Kind = Pipeline_Sprite;
		if (Settings.Type == EParticleRendererType::Mesh)
		{
			Kind = Pipeline_Mesh;
		}
		else if (Settings.Type == EParticleRendererType::Ribbon)
		{
			if (bGpu)
			{
				if (!bWarnedGpuRibbon)
				{
					E_LOG(LogRenderer, Warning, "리본 렌더러는 CPU 이미터에서만 그려집니다 (이미터 '{}')", Emitter.Name);
					bWarnedGpuRibbon = true;
				}
				continue;
			}
			Kind = Pipeline_Ribbon;
		}
		const FStaticMesh* Mesh = Kind == Pipeline_Mesh ? Resources->GetMesh(Settings.Mesh) : nullptr;
		if (Kind == Pipeline_Mesh && Mesh == nullptr)
		{
			continue;
		}

		// 입자 데이터 주소 + 개수
		D3D12_GPU_VIRTUAL_ADDRESS ParticleAddress = FrameAddress; // 리본은 읽지 않음 (아무 유효 주소)
		uint32                    InstanceCount   = 0;
		uint32                    VisibleCount    = 0;
		if (bGpu)
		{
			auto* Pool = static_cast<FParticleGpuBuffer*>(Instance.GpuState.get());
			if (!Pool->Buffer)
			{
				continue;
			}
			Pool->Transition(CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ParticleAddress = Pool->Buffer->GetGPUVirtualAddress();
			InstanceCount   = Pool->Capacity;
			VisibleCount    = Instance.GpuEstimatedAlive;
		}
		else if (Kind != Pipeline_Ribbon)
		{
			const size_t Count = FMath::Min(Instance.Particles.size(), static_cast<size_t>(AvailableBytes() / sizeof(FParticleGpuData)));
			if (Count == 0)
			{
				WarnFull();
				continue;
			}
			// 반투명은 시선 깊이 기준 뒤→앞
			OrderScratch.resize(Instance.Particles.size());
			for (uint32 Index = 0; Index < OrderScratch.size(); ++Index)
			{
				OrderScratch[Index] = Index;
			}
			if (bAlpha)
			{
				const auto Depth = [&](uint32 Index) {
					return FVector3::Dot(LocalToWorld.TransformPosition(Instance.Particles[Index].Position) - CameraPosition, Forward);
				};
				std::sort(OrderScratch.begin(), OrderScratch.end(), [&](uint32 A, uint32 B) { return Depth(A) > Depth(B); });
			}
			UploadScratch.resize(Count);
			for (size_t Index = 0; Index < Count; ++Index)
			{
				UploadScratch[Index] = ToGpu(Instance.Particles[OrderScratch[Index]]);
			}
			const uint64                  Bytes      = sizeof(FParticleGpuData) * Count;
			const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(Bytes, 16);
			std::memcpy(Allocation.CpuAddress, UploadScratch.data(), Bytes);
			ParticleAddress = Allocation.GpuAddress;
			InstanceCount   = static_cast<uint32>(Count);
			VisibleCount    = InstanceCount;
		}

		// 리본: 생성 순서로 이은 띠 (정점을 CPU에서 만든다)
		D3D12_VERTEX_BUFFER_VIEW RibbonView{};
		if (Kind == Pipeline_Ribbon)
		{
			OrderScratch.resize(Instance.Particles.size());
			for (uint32 Index = 0; Index < OrderScratch.size(); ++Index)
			{
				OrderScratch[Index] = Index;
			}
			std::sort(OrderScratch.begin(), OrderScratch.end(),
			          [&](uint32 A, uint32 B) { return Instance.Particles[A].SpawnIndex < Instance.Particles[B].SpawnIndex; });
			const size_t PointCount = OrderScratch.size();
			if (PointCount < 2)
			{
				continue;
			}
			const size_t VertexCount = (PointCount - 1) * 6;
			if (AvailableBytes() < VertexCount * sizeof(FParticleRibbonVertex))
			{
				WarnFull();
				continue;
			}
			// 점마다 왼쪽/오른쪽 가장자리
			std::vector<std::pair<FParticleRibbonVertex, FParticleRibbonVertex>> Edges(PointCount);
			for (size_t Point = 0; Point < PointCount; ++Point)
			{
				const FParticle& P       = Instance.Particles[OrderScratch[Point]];
				const FVector3   World   = LocalToWorld.TransformPosition(P.Position);
				const FParticle& Prev    = Instance.Particles[OrderScratch[Point == 0 ? 0 : Point - 1]];
				const FParticle& Next    = Instance.Particles[OrderScratch[Point + 1 < PointCount ? Point + 1 : Point]];
				const FVector3   Tangent = LocalToWorld.TransformVector(Next.Position - Prev.Position);
				const FVector3   ToCam   = CameraPosition - World;
				FVector3         Side    = FVector3::Cross(Tangent, ToCam);
				Side                     = Side.LengthSquared() > FMath::SmallNumber ? Side.GetNormalized() : Camera.GetRightVector();
				const float      Half    = P.Size.X * Settings.RibbonWidthScale * 0.5f;
				const float      U       = static_cast<float>(Point) / static_cast<float>(PointCount - 1);
				Edges[Point].first       = { World - Side * Half, FVector2(U, 0.0f), P.Color };
				Edges[Point].second      = { World + Side * Half, FVector2(U, 1.0f), P.Color };
			}
			RibbonScratch.clear();
			for (size_t Point = 0; Point + 1 < PointCount; ++Point)
			{
				const auto& A = Edges[Point];
				const auto& B = Edges[Point + 1];
				RibbonScratch.insert(RibbonScratch.end(), { A.first, A.second, B.second, A.first, B.second, B.first });
			}
			const uint64                  Bytes      = sizeof(FParticleRibbonVertex) * RibbonScratch.size();
			const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(Bytes, 16);
			std::memcpy(Allocation.CpuAddress, RibbonScratch.data(), Bytes);
			RibbonView.BufferLocation = Allocation.GpuAddress;
			RibbonView.SizeInBytes    = static_cast<UINT>(Bytes);
			RibbonView.StrideInBytes  = sizeof(FParticleRibbonVertex);
			VisibleCount              = static_cast<uint32>(PointCount);
		}

		FParticleDrawConstants DrawConstants;
		DrawConstants.LocalToWorld    = LocalToWorld;
		DrawConstants.SubImageColumns = std::max(Settings.SubImageColumns, 1);
		DrawConstants.SubImageRows    = std::max(Settings.SubImageRows, 1);
		DrawConstants.Alignment       = static_cast<int32>(Settings.Alignment);
		DrawConstants.VelocityStretch = Settings.VelocityStretch;

		ID3D12PipelineState* Pipeline = Pipelines.Graphics[Kind][bAlpha ? 0 : 1].Get();
		if (Pipeline != BoundPipeline)
		{
			CommandList->SetPipelineState(Pipeline);
			BoundPipeline = Pipeline;
		}
		CommandList->SetGraphicsRootConstantBufferView(RootParam_Draw, DynamicBuffer.AllocateConstants(DrawConstants).GpuAddress);
		CommandList->SetGraphicsRootShaderResourceView(RootParam_Particles, ParticleAddress);
		const FD3D12Texture& Texture = Settings.Texture.IsValid() ? Resources->ResolveTexture(Settings.Texture) : Fallback;
		CommandList->SetGraphicsRootDescriptorTable(RootParam_Texture, Texture.GetSrv().Gpu);

		if (Kind == Pipeline_Sprite)
		{
			CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CommandList->DrawInstanced(6, InstanceCount, 0, 0);
		}
		else if (Kind == Pipeline_Mesh)
		{
			Mesh->DrawInstanced(CommandList, InstanceCount);
		}
		else
		{
			CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CommandList->IASetVertexBuffers(0, 1, &RibbonView);
			CommandList->DrawInstanced(static_cast<UINT>(RibbonScratch.size()), 1, 0, 0);
		}
		// 입자 수는 이미터당 한 번만 센다 (렌더러가 여러 개여도)
		if (std::find(CountedInstances.begin(), CountedInstances.end(), &Instance) == CountedInstances.end())
		{
			CountedInstances.push_back(&Instance);
			DrawnCount += VisibleCount;
		}
	}
	return DrawnCount;
}
