#include "Renderer/SpriteRenderer.h"

#include "Core/Console/Console.h"
#include "Core/Paths.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/Image.h"
#include "Renderer/ResourceCollector.h"
#include "Renderer/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// Sprite.hlsl E_SPRITE_FLAG_*
	constexpr uint32 SpriteFlag_Point = 1u;

	// 임시 확인 경로 (머지 뒤 컴포넌트 수집이 붙으면 AppendTestSprites와 함께 지운다)
	TAutoConsoleVariable<int32> GSpriteTest("r.Sprite.Test", 0,
	                                        "시험 스프라이트 N개를 카메라 앞에 격자로 그린다 (스프라이트 렌더러 확인용 — 레이어·블렌드·필터·조명 섞음, 0 = 끔)");

	const wchar_t* GetPixelEntry(ESpriteBlendMode Blend)
	{
		switch (Blend)
		{
		case ESpriteBlendMode::Premultiplied: return L"SpritePSPremultiplied";
		case ESpriteBlendMode::Additive:      return L"SpritePSAdditive";
		case ESpriteBlendMode::Masked:        return L"SpritePSMasked";
		default:                              return L"SpritePSAlpha";
		}
	}

	EBlendMode GetPipelineBlend(ESpriteBlendMode Blend)
	{
		switch (Blend)
		{
		case ESpriteBlendMode::Premultiplied: return EBlendMode::PremultipliedOver;
		case ESpriteBlendMode::Additive:      return EBlendMode::Additive;
		case ESpriteBlendMode::Masked:        return EBlendMode::Opaque;
		default:                              return EBlendMode::Alpha;
		}
	}
} // namespace

FSpriteRenderer::~FSpriteRenderer()
{
	Shutdown();
}

bool FSpriteRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, ID3D12RootSignature* InMeshRootSignature,
                           DXGI_FORMAT InColorFormat, DXGI_FORMAT InDepthFormat)
{
	E_CHECKF(Rhi == nullptr, "스프라이트 렌더러가 이미 초기화되어 있습니다");
	Rhi               = &InRhi;
	ShaderLibrary     = &InShaderLibrary;
	Resources         = &InResources;
	MeshRootSignature = InMeshRootSignature;
	ColorFormat       = InColorFormat;
	DepthFormat       = InDepthFormat;
	// 시험 텍스처 중 경로 텍스처(아이콘)는 리소스 수거 루트로 (생성 텍스처는 만든 쪽 소유라 수거 대상 아님)
	TestRootProviderId = Resources->AddRootProvider([this](FResourceRoots& Roots) {
		for (const FTextureHandle Texture : TestTextures)
		{
			Roots.Add(Texture);
		}
	});
	return CreatePipelines(Pipelines, false);
}

void FSpriteRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (FD3D12PipelineState& Pipeline : Pipelines)
	{
		Pipeline.Shutdown();
	}
	Resources->RemoveRootProvider(TestRootProviderId);
	TestTextures.clear(); // 텍스처는 리소스 관리자가 종료 시 해제
	DrawList.clear();
	Rhi               = nullptr;
	ShaderLibrary     = nullptr;
	Resources         = nullptr;
	MeshRootSignature = nullptr;
}

bool FSpriteRenderer::CreatePipelines(FD3D12PipelineState (&Out)[SpriteBatching::PipelineKeyCount], bool bForceRecompile)
{
	const auto GetBlob = [&](const wchar_t* Entry, EShaderStage Stage, bool bLit) -> ComPtr<IDxcBlob> {
		FShaderCompileDesc Desc;
		Desc.FileName   = L"Sprite.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		if (bLit)
		{
			Desc.Defines.push_back(L"E_SPRITE_LIT");
		}
		if (bForceRecompile && !ShaderLibrary->CookShader(Desc))
		{
			return nullptr;
		}
		return ShaderLibrary->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> Vertex = GetBlob(L"SpriteVS", EShaderStage::Vertex, false);
	if (!Vertex)
	{
		return false;
	}
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	for (uint32 BlendIndex = 0; BlendIndex < static_cast<uint32>(ESpriteBlendMode::Count); ++BlendIndex)
	{
		const ESpriteBlendMode Blend = static_cast<ESpriteBlendMode>(BlendIndex);
		for (uint32 Lit = 0; Lit < 2; ++Lit)
		{
			const ComPtr<IDxcBlob> Pixel = GetBlob(GetPixelEntry(Blend), EShaderStage::Pixel, Lit != 0);
			if (!Pixel)
			{
				return false;
			}
			FGraphicsPipelineDesc Desc;
			Desc.RootSignature          = MeshRootSignature;
			Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(Vertex.Get());
			Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(Pixel.Get());
			Desc.RenderTargetFormats[0] = ColorFormat;
			Desc.DepthStencilFormat     = DepthFormat;
			Desc.CullMode               = D3D12_CULL_MODE_NONE; // 양면
			Desc.bDepthEnable           = true;
			Desc.bDepthWrite            = Blend == ESpriteBlendMode::Masked; // 반투명 계열은 3D 불투명에 가려지기만
			Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			Desc.BlendMode              = GetPipelineBlend(Blend);
			const std::wstring Name     = std::wstring(L"Sprite") + GetPixelEntry(Blend) + (Lit != 0 ? L"Lit" : L"");
			if (!Out[SpriteBatching::MakePipelineKey(Blend, Lit != 0)].InitGraphics(Device, Desc, Name.c_str()))
			{
				return false;
			}
		}
	}
	return true;
}

bool FSpriteRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState New[SpriteBatching::PipelineKeyCount];
	if (!CreatePipelines(New, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "스프라이트 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	for (uint32 Index = 0; Index < SpriteBatching::PipelineKeyCount; ++Index)
	{
		Pipelines[Index].Swap(New[Index]);
		Rhi->DeferRelease(New[Index].Detach());
	}
	return true;
}

void FSpriteRenderer::Prepare(const FCamera& Camera, FPreparedFrame& Out)
{
	Out = FPreparedFrame{};
	if (Rhi == nullptr)
	{
		return;
	}
	const std::vector<FSpriteDrawItem>* Items = &DrawList;
	if (const int32 TestCount = GSpriteTest.Get(); TestCount > 0)
	{
		FrameItems = DrawList;
		AppendTestSprites(Camera, TestCount, FrameItems);
		Items = &FrameItems;
	}
	else
	{
		FrameItems.clear();
	}
	if (Items->empty())
	{
		return;
	}

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	size_t                     Count         = Items->size();
	const uint64               MaxCount      = DynamicBuffer.GetMaxAllocation() / sizeof(FSpriteInstanceGpu);
	if (Count > MaxCount)
	{
		if (!bWarnedBufferFull)
		{
			E_LOG(LogRenderer, Warning, "스프라이트가 너무 많아 {}개 중 {}개만 그립니다 (프레임 업로드 상한)", Count, MaxCount);
			bWarnedBufferFull = true;
		}
		Count = static_cast<size_t>(MaxCount);
	}

	// 정렬 키 (깊이 = 사각형 가운데의 카메라 시선 거리)
	const FVector3 CameraPosition = Camera.GetPosition();
	const FVector3 CameraForward  = Camera.GetForwardVector();
	SortKeys.resize(Count);
	for (size_t Index = 0; Index < Count; ++Index)
	{
		const FSpriteDrawItem& Item = (*Items)[Index];
		SortKeys[Index]             = { Item.SortLayer, Item.OrderInLayer,
		                                SpriteMath::ComputeSortDepth(SpriteMath::ComputeQuad(Item), CameraPosition, CameraForward) };
	}
	SpriteSorting::Sort(SortKeys, SortOrder);

	// 정렬 순서대로 인스턴스를 업로드 버퍼에 바로 쓴다 (텍스처 칸 번호는 이번 프레임 값 — 같은 핸들이 이어지면 다시 찾지 않음)
	const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FSpriteInstanceGpu) * Count, 16);
	auto*                         Gpu        = static_cast<FSpriteInstanceGpu*>(Allocation.CpuAddress);
	PipelineKeys.resize(Count);
	FTextureHandle LastTexture;
	uint32         LastTextureIndex = Resources->ResolveTexture(LastTexture).GetSrv().Index;
	for (size_t Index = 0; Index < Count; ++Index)
	{
		const FSpriteDrawItem&  Item = (*Items)[SortOrder[Index]];
		const SpriteMath::FQuad Quad = SpriteMath::ComputeQuad(Item);
		if (Item.Texture != LastTexture)
		{
			LastTexture      = Item.Texture;
			LastTextureIndex = Resources->ResolveTexture(Item.Texture).GetSrv().Index;
		}
		FSpriteInstanceGpu Instance;
		Instance.Origin       = Quad.Origin;
		Instance.TextureIndex = LastTextureIndex;
		Instance.AxisX        = Quad.AxisX;
		Instance.Flags        = Item.Filter == ESpriteFilter::Point ? SpriteFlag_Point : 0u;
		Instance.AxisZ        = Quad.AxisZ;
		Instance.AlphaCutoff  = Item.AlphaCutoff;
		Instance.UVRect       = FVector4(Item.UVMin.X, Item.UVMin.Y, Item.UVMax.X, Item.UVMax.Y);
		Instance.Color        = Item.Color;
		std::memcpy(&Gpu[Index], &Instance, sizeof(Instance)); // 업로드 힙(쓰기 결합)에 순서대로
		const ESpriteBlendMode Blend = Item.Blend < ESpriteBlendMode::Count ? Item.Blend : ESpriteBlendMode::Alpha;
		PipelineKeys[Index]          = static_cast<uint8>(SpriteBatching::MakePipelineKey(Blend, Item.bLit));
	}
	SpriteBatching::BuildRuns(PipelineKeys, Runs);

	Out.Instances   = Allocation.GpuAddress;
	Out.SpriteCount = static_cast<uint32>(Count);
	Out.Runs.reserve(Runs.size());
	for (const SpriteBatching::FRun& Run : Runs)
	{
		Out.Runs.push_back({ Pipelines[Run.PipelineKey].Get(), Run.First, Run.Count });
	}
}

void FSpriteRenderer::RecordDraws(ID3D12GraphicsCommandList* CommandList, const FPreparedFrame& Prepared, uint32 DrawConstantsParam, uint32 InstancesParam,
                                  uint32 InstanceIndicesParam)
{
	if (Prepared.IsEmpty())
	{
		return;
	}
	CommandList->SetGraphicsRootShaderResourceView(InstancesParam, Prepared.Instances);
	CommandList->SetGraphicsRootShaderResourceView(InstanceIndicesParam, Prepared.Instances); // 읽지 않음 (유효 주소)
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12PipelineState* Bound = nullptr;
	for (const FPreparedRun& Run : Prepared.Runs)
	{
		if (Run.Pipeline != Bound)
		{
			CommandList->SetPipelineState(Run.Pipeline);
			Bound = Run.Pipeline;
		}
		CommandList->SetGraphicsRoot32BitConstant(DrawConstantsParam, Run.First, 0);
		CommandList->DrawInstanced(6, Run.Count, 0, 0);
	}
}

// ---- 임시 확인 경로 (r.Sprite.Test N) — 머지 뒤 컴포넌트 수집이 붙으면 이 함수와 TestTextures/TestRootProviderId를 지운다.
// 카메라 앞 일정 거리 평면에 N개를 격자로 놓는다. 축은 카메라 오른쪽/위 (2D 카메라(+Y에서 -Y를 봄)면 정확히 월드 X/Z 평면 — 3D 시험 씬에서도
// 보이도록 카메라 축을 쓴다). 섞는 것: 정렬 레이어(큰 스프라이트가 위 레이어로 이웃을 덮음)·순번·깊이, 블렌드 4종, Point/Linear, 조명, 회전, UV 반전.
void FSpriteRenderer::AppendTestSprites(const FCamera& Camera, int32 Count, std::vector<FSpriteDrawItem>& Out)
{
	if (TestTextures.empty())
	{
		// 16x16 체커 + 원형 알파 (가장자리 부드러움) — Point/Linear 차이와 Masked 컷오프가 잘 보인다
		constexpr uint32 Size = 16;
		FImage           Straight;
		Straight.Width  = Size;
		Straight.Height = Size;
		Straight.Pixels.resize(static_cast<size_t>(Size) * Size * FImage::BytesPerPixel);
		FImage Premultiplied = Straight;
		for (uint32 Y = 0; Y < Size; ++Y)
		{
			for (uint32 X = 0; X < Size; ++X)
			{
				const float  U      = (static_cast<float>(X) + 0.5f) / Size * 2.0f - 1.0f;
				const float  V      = (static_cast<float>(Y) + 0.5f) / Size * 2.0f - 1.0f;
				const float  Radius = std::sqrt(U * U + V * V);
				const float  Alpha  = std::clamp((1.0f - Radius) / 0.35f, 0.0f, 1.0f);
				const bool   bLight = ((X / 4) + (Y / 4)) % 2 == 0;
				const uint8  Shade  = bLight ? 255 : (Y < Size / 2 ? 120 : 60); // 위아래가 달라 상하 반전이 보인다
				const size_t Offset = (static_cast<size_t>(Y) * Size + X) * FImage::BytesPerPixel;
				const uint8  A      = static_cast<uint8>(Alpha * 255.0f + 0.5f);
				Straight.Pixels[Offset + 0] = Shade;
				Straight.Pixels[Offset + 1] = X < Size / 2 ? Shade : static_cast<uint8>(Shade / 2); // 좌우가 달라 좌우 반전이 보인다
				Straight.Pixels[Offset + 2] = Shade;
				Straight.Pixels[Offset + 3] = A;
				for (uint32 Channel = 0; Channel < 3; ++Channel)
				{
					Premultiplied.Pixels[Offset + Channel] = static_cast<uint8>(Straight.Pixels[Offset + Channel] * Alpha + 0.5f);
				}
				Premultiplied.Pixels[Offset + 3] = A;
			}
		}
		TestTextures.push_back(Resources->CreateTexture(Straight, true, L"SpriteTestChecker"));
		TestTextures.push_back(Resources->CreateTexture(Premultiplied, true, L"SpriteTestCheckerPremultiplied"));
		// 프로젝트 콘텐츠의 PNG (머티리얼 밖 텍스처 = 공개 LoadTexture, 고정 전체 밉). 없는 프로젝트면 생성 텍스처만
		for (const char* Icon : { "Asset/Icons/7Soul_RPG_Icons/I_Crystal01.png", "Asset/Icons/7Soul_RPG_Icons/I_Chest02.png",
		                          "Asset/Icons/7Soul_RPG_Icons/E_Wood02.png" })
		{
			const std::filesystem::path Path = FPaths::GetProjectContentDirectory() / Icon;
			std::error_code             Error;
			if (std::filesystem::exists(Path, Error))
			{
				TestTextures.push_back(Resources->LoadTexture(Path, ETextureUsage::Color));
			}
		}
	}

	const FVector3 Position = Camera.GetPosition();
	const FVector3 Forward  = Camera.GetForwardVector();
	const FVector3 Right    = Camera.GetRightVector();
	const FVector3 Up       = Camera.GetUpVector();
	constexpr float Distance = 500.0f;
	const float HalfHeight   = Camera.IsOrthographic() ? Camera.GetOrthoHeight() * 0.5f
	                                                   : Distance * FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	const float HalfWidth    = HalfHeight * Camera.GetAspectRatio();
	const int32 Columns      = std::max(1, static_cast<int32>(std::lround(std::sqrt(static_cast<float>(Count) * HalfWidth / std::max(HalfHeight, 1.0f)))));
	const int32 Rows         = (Count + Columns - 1) / Columns;
	const float Cell         = std::min(HalfWidth * 1.6f / static_cast<float>(Columns), HalfHeight * 1.6f / static_cast<float>(Rows));
	static const FVector4 Tints[] = { { 1.0f, 1.0f, 1.0f, 1.0f },  { 1.0f, 0.35f, 0.3f, 1.0f }, { 0.35f, 1.0f, 0.4f, 1.0f },
		                              { 0.35f, 0.5f, 1.0f, 1.0f }, { 1.0f, 0.85f, 0.3f, 1.0f }, { 0.9f, 0.4f, 1.0f, 1.0f } };
	static const ESpriteBlendMode Blends[] = { ESpriteBlendMode::Alpha,    ESpriteBlendMode::Masked, ESpriteBlendMode::Additive, ESpriteBlendMode::Alpha,
		                                       ESpriteBlendMode::Premultiplied, ESpriteBlendMode::Masked, ESpriteBlendMode::Alpha, ESpriteBlendMode::Additive };
	const size_t IconCount = TestTextures.size() > 2 ? TestTextures.size() - 2 : 0;

	Out.reserve(Out.size() + static_cast<size_t>(Count));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row    = Index / Columns;
		const bool  bBig   = Index % 6 == 0; // 위 레이어, 1.7배 — 이웃을 덮어 레이어 정렬이 보인다
		const float Angle  = Index % 4 == 3 ? FMath::DegreesToRadians(15.0f) : 0.0f;
		const FVector3 AxisX = Right * std::cos(Angle) + Up * std::sin(Angle);
		const FVector3 AxisZ = Up * std::cos(Angle) - Right * std::sin(Angle);
		const FVector3 Center = Position + Forward * (Distance + static_cast<float>((Index * 7) % 5) * 15.0f) +
		                        Right * ((static_cast<float>(Column) - static_cast<float>(Columns - 1) * 0.5f) * Cell) +
		                        Up * ((static_cast<float>(Rows - 1) * 0.5f - static_cast<float>(Row)) * Cell);

		FSpriteDrawItem Item;
		const FVector3  Front = -Forward; // 로컬 +Y = 카메라 쪽
		Item.World.M[0][0] = AxisX.X; Item.World.M[0][1] = AxisX.Y; Item.World.M[0][2] = AxisX.Z; Item.World.M[0][3] = 0.0f;
		Item.World.M[1][0] = Front.X; Item.World.M[1][1] = Front.Y; Item.World.M[1][2] = Front.Z; Item.World.M[1][3] = 0.0f;
		Item.World.M[2][0] = AxisZ.X; Item.World.M[2][1] = AxisZ.Y; Item.World.M[2][2] = AxisZ.Z; Item.World.M[2][3] = 0.0f;
		Item.World.M[3][0] = Center.X; Item.World.M[3][1] = Center.Y; Item.World.M[3][2] = Center.Z; Item.World.M[3][3] = 1.0f;
		Item.Size         = FVector2(Cell * 0.9f) * (bBig ? 1.7f : 1.0f);
		Item.SortLayer    = bBig ? 1 : 0;
		Item.OrderInLayer = Index % 3;
		Item.Blend        = Blends[Index % 8];
		Item.Filter       = (Index / 2) % 2 == 0 ? ESpriteFilter::Point : ESpriteFilter::Linear;
		Item.bLit         = Index % 3 == 0;
		Item.Color        = Tints[Index % 6];
		if (Item.Blend == ESpriteBlendMode::Alpha)
		{
			Item.Color.W = 0.8f;
		}
		if (Item.Blend == ESpriteBlendMode::Premultiplied)
		{
			Item.Texture = TestTextures[1];
		}
		else if (IconCount > 0 && Index % 2 == 1)
		{
			Item.Texture = TestTextures[2 + static_cast<size_t>(Index / 2) % IconCount];
		}
		else
		{
			Item.Texture = TestTextures[0];
		}
		if (Index % 5 == 4)
		{
			std::swap(Item.UVMin.X, Item.UVMax.X); // 좌우 반전
		}
		Out.push_back(Item);
	}
}
