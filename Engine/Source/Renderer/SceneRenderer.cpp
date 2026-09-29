#include "Renderer/SceneRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/Camera.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// Mesh.hlsl 루트 시그니처 레이아웃
	enum ERootParameter : uint32
	{
		RootParam_PerObject       = 0, // b0
		RootParam_PerFrame        = 1, // b1
		RootParam_Material        = 2, // b2
		RootParam_MaterialTexture = 3, // t0
	};
} // namespace

bool FSceneRenderer::Init(FD3D12RHI& InRhi, FResourceManager& InResources)
{
	E_CHECKF(Rhi == nullptr, "씬 렌더러가 이미 초기화되어 있습니다");
	Rhi       = &InRhi;
	Resources = &InResources;

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	if (!ShaderCompiler.Init())
	{
		return false;
	}

	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Mesh.hlsl";
	VertexDesc.EntryPoint = L"VSMain";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"PSMain";
	PixelDesc.Stage              = EShaderStage::Pixel;

	const ComPtr<IDxcBlob> VertexShader = ShaderCompiler.Compile(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = ShaderCompiler.Compile(PixelDesc);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}

	const uint32 PerObjectIndex = RootSignature.AddConstantBufferView(0);
	const uint32 PerFrameIndex  = RootSignature.AddConstantBufferView(1);
	const uint32 MaterialIndex  = RootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 TextureIndex   = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(PerObjectIndex == RootParam_PerObject && PerFrameIndex == RootParam_PerFrame &&
	        MaterialIndex == RootParam_Material && TextureIndex == RootParam_MaterialTexture);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0));
	if (!RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"MeshRootSignature"))
	{
		return false;
	}

	FGraphicsPipelineDesc PsoDesc;
	PsoDesc.RootSignature          = RootSignature.Get();
	PsoDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	PsoDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	PsoDesc.InputLayout            = FStaticMesh::GetInputLayout();
	PsoDesc.RenderTargetFormats[0] = FD3D12RHI::RenderTargetFormat;
	PsoDesc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
	PsoDesc.bDepthEnable           = true;
	if (!PipelineState.InitGraphics(Device, PsoDesc, L"MeshPipeline"))
	{
		return false;
	}

	E_LOG(LogRenderer, Display, "씬 렌더러 초기화 완료");
	return true;
}

void FSceneRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	Rhi->GetGraphicsQueue().Flush();
	PipelineState.Shutdown();
	RootSignature.Shutdown();
	ShaderCompiler.Shutdown();
	DrawCommands.clear();
	Rhi       = nullptr;
	Resources = nullptr;
}

void FSceneRenderer::SetFreezeCulling(bool bFreeze)
{
	bCullingFrozen = bFreeze;
}

void FSceneRenderer::Render(FScene& Scene, const FCamera& Camera)
{
	E_CHECKF(Rhi != nullptr, "씬 렌더러가 초기화되지 않았습니다");

	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();
	if (!bCullingFrozen)
	{
		FrozenFrustum = FFrustum::FromViewProjection(ViewProjection);
	}

	CollectDrawCommands(Scene, FrozenFrustum, Camera.GetPosition());

	// 정렬: 머티리얼 → 메시 → 가까운 순 (상태 변경 최소화 + 초기 깊이 기각)
	std::sort(DrawCommands.begin(), DrawCommands.end(), [](const FMeshDrawCommand& A, const FMeshDrawCommand& B) {
		return std::tie(A.MaterialHandle.Index, A.MeshHandle.Index, A.DistanceSquared) <
		       std::tie(B.MaterialHandle.Index, B.MeshHandle.Index, B.DistanceSquared);
	});

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();

	const FD3D12DynamicAllocation PerFrameAllocation = DynamicBuffer.AllocateConstants(BuildPerFrameConstants(Scene, Camera));

	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(PipelineState.Get());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_PerFrame, PerFrameAllocation.GpuAddress);

	// 머티리얼 상수는 프레임 내에서 한 번만 업로드
	std::unordered_map<uint64, D3D12_GPU_VIRTUAL_ADDRESS> MaterialConstantCache;

	const FMaterial* BoundMaterial = nullptr;
	Stats.DrawCalls                = 0;

	for (const FMeshDrawCommand& Command : DrawCommands)
	{
		if (Command.Material != BoundMaterial)
		{
			const uint64 Key   = Command.MaterialHandle.ToId();
			auto         Found = MaterialConstantCache.find(Key);
			if (Found == MaterialConstantCache.end())
			{
				Found = MaterialConstantCache.emplace(Key, DynamicBuffer.AllocateConstants(Command.Material->Constants).GpuAddress).first;
			}
			CommandList->SetGraphicsRootConstantBufferView(RootParam_Material, Found->second);
			CommandList->SetGraphicsRootDescriptorTable(RootParam_MaterialTexture,
			                                            Resources->ResolveTexture(Command.Material->BaseColorTexture).GetSrv().Gpu);
			BoundMaterial = Command.Material;
		}

		FPerObjectConstants PerObject;
		PerObject.World                 = Command.World;
		PerObject.WorldInverseTranspose = Command.World.GetInverse().GetTransposed();
		CommandList->SetGraphicsRootConstantBufferView(RootParam_PerObject, DynamicBuffer.AllocateConstants(PerObject).GpuAddress);

		Command.Mesh->Draw(CommandList);
		++Stats.DrawCalls;
	}
}

void FSceneRenderer::CollectDrawCommands(FScene& Scene, const FFrustum& Frustum, const FVector3& CameraPosition)
{
	DrawCommands.clear();
	Stats.TotalMeshes   = 0;
	Stats.VisibleMeshes = 0;

	Scene.GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			++Stats.TotalMeshes;
			if (!MeshComponent.bVisible)
			{
				return;
			}

			const FStaticMesh* Mesh = Resources->GetMesh(MeshComponent.Mesh);
			if (Mesh == nullptr)
			{
				return;
			}

			// 월드 AABB로 프러스텀 컬링
			const FBox WorldBounds = Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix);
			if (!Frustum.Intersects(WorldBounds))
			{
				return;
			}
			++Stats.VisibleMeshes;

			FMeshDrawCommand& Command = DrawCommands.emplace_back();
			Command.Mesh              = Mesh;
			Command.MeshHandle        = MeshComponent.Mesh;
			Command.Material          = &Resources->ResolveMaterial(MeshComponent.Material);
			Command.MaterialHandle    = MeshComponent.Material.IsValid() ? MeshComponent.Material : Resources->GetDefaultMaterial();
			Command.World             = Transform.WorldMatrix;
			Command.DistanceSquared   = FVector3::DistanceSquared(WorldBounds.GetCenter(), CameraPosition);
		});
}

FPerFrameConstants FSceneRenderer::BuildPerFrameConstants(FScene& Scene, const FCamera& Camera) const
{
	FPerFrameConstants PerFrame;
	PerFrame.ViewProjection = Camera.GetViewProjectionMatrix();
	PerFrame.CameraPosition = Camera.GetPosition();
	PerFrame.AmbientColor   = AmbientColor;

	// 첫 번째 방향광 사용 (여러 광원은 Phase 6)
	bool bFoundLight = false;
	Scene.GetRegistry().View<FTransformComponent, FDirectionalLightComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FDirectionalLightComponent& Light) {
			if (bFoundLight)
			{
				return;
			}
			PerFrame.DirectionalLight.Direction = Transform.GetWorldForward();
			PerFrame.DirectionalLight.Color     = Light.Color;
			PerFrame.DirectionalLight.Intensity = Light.Intensity;
			bFoundLight                         = true;
		});

	if (!bFoundLight)
	{
		PerFrame.DirectionalLight.Direction = FVector3(1.0f, 0.5f, -1.0f).GetNormalized();
		PerFrame.DirectionalLight.Intensity = 1.0f;
	}
	return PerFrame;
}
