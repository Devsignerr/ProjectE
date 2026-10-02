#include "Core/Testing/TestFramework.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/ShaderManifest.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/MaterialRender.h"

#include <set>

// Phase 49 사이드: 그래프 생성 HLSL이 실제 메시/그림자 셰이더 변형으로 DXC 컴파일되는지 (-HV 2021 -Zpr -WX), 가상 파일 캐시/쿠킹 규칙

namespace
{
	// 노드 표의 거의 모든 종류를 출력까지 잇는 그래프
	const char* const EveryNodeGraph = R"({
		"Name": "EveryNode", "BlendMode": "Masked",
		"Parameters": [
			{ "Name": "Albedo", "Value": "a.png", "Usage": "Color" }, { "Name": "Bumps", "Value": "n.png", "Usage": "Normal" },
			{ "Name": "Detail", "Value": "d.png", "Usage": "Normal" }, { "Name": "Tint", "Value": [1, 0.5, 0.2, 1] },
			{ "Name": "Speed", "Value": 0.1 }, { "Name": "Fancy", "Type": "StaticSwitch", "Value": true }
		],
		"Graph": { "Nodes": [
			{ "Id": "uv", "Type": "TexCoord", "Tiling": [2, 3] },
			{ "Id": "speed", "Type": "ScalarParameter", "Parameter": "Speed" },
			{ "Id": "pan", "Type": "Panner", "Inputs": { "UV": "uv", "Speed": "speed" } },
			{ "Id": "albedo", "Type": "TextureSample", "Texture": "Albedo", "Sampler": "Clamp", "Inputs": { "UV": "pan" } },
			{ "Id": "bumps", "Type": "TextureSample", "Texture": "Bumps" },
			{ "Id": "detail", "Type": "TextureSample", "Texture": "Detail", "Inputs": { "UV": "uv" } },
			{ "Id": "normal", "Type": "BlendNormals", "Inputs": { "A": "bumps:1", "B": "detail" } },
			{ "Id": "tint", "Type": "VectorParameter", "Parameter": "Tint" },
			{ "Id": "vc", "Type": "VertexColor" },
			{ "Id": "base0", "Type": "Multiply", "Inputs": { "A": "albedo:1", "B": "tint:1" } },
			{ "Id": "base1", "Type": "Multiply", "Inputs": { "A": "base0", "B": "vc:1" } },
			{ "Id": "pos", "Type": "WorldPosition" }, { "Id": "wn", "Type": "WorldNormal" }, { "Id": "cam", "Type": "CameraVector" },
			{ "Id": "cross", "Type": "Cross", "Inputs": { "A": "wn", "B": "cam" } },
			{ "Id": "len", "Type": "Length", "Inputs": { "A": "cross" } },
			{ "Id": "nrm", "Type": "Normalize", "Inputs": { "A": "pos" } },
			{ "Id": "dot", "Type": "Dot", "Inputs": { "A": "nrm", "B": "wn" } },
			{ "Id": "fres", "Type": "Fresnel", "Inputs": { "Exponent": 4 } },
			{ "Id": "time", "Type": "Time" }, { "Id": "sin", "Type": "Sine", "Inputs": { "A": "time" } }, { "Id": "cos", "Type": "Cosine", "Inputs": { "A": "time" } },
			{ "Id": "frac", "Type": "Frac", "Inputs": { "A": "sin" } }, { "Id": "floor", "Type": "Floor", "Inputs": { "A": "cos" } },
			{ "Id": "abs", "Type": "Abs", "Inputs": { "A": "floor" } }, { "Id": "om", "Type": "OneMinus", "Inputs": { "A": "frac" } },
			{ "Id": "pow", "Type": "Power", "Inputs": { "A": "om", "B": 2 } }, { "Id": "min", "Type": "Min", "Inputs": { "A": "pow", "B": "abs" } },
			{ "Id": "max", "Type": "Max", "Inputs": { "A": "min", "B": "dot" } }, { "Id": "div", "Type": "Divide", "Inputs": { "A": "max", "B": 2 } },
			{ "Id": "sub", "Type": "Subtract", "Inputs": { "A": "div", "B": "len" } }, { "Id": "add", "Type": "Add", "Inputs": { "A": "sub", "B": "fres" } },
			{ "Id": "sat", "Type": "Saturate", "Inputs": { "A": "add" } }, { "Id": "clamp", "Type": "Clamp", "Inputs": { "A": "sat", "Min": 0.1, "Max": 0.9 } },
			{ "Id": "mask", "Type": "ComponentMask", "Channels": "zx", "Inputs": { "A": "pos" } },
			{ "Id": "split", "Type": "Split", "Inputs": { "A": "mask" } },
			{ "Id": "app", "Type": "Append", "Inputs": { "A": "mask", "B": "split:1" } },
			{ "Id": "lerp", "Type": "Lerp", "Inputs": { "A": "base1", "B": "app", "Alpha": "clamp" } },
			{ "Id": "cmp", "Type": "Compare", "Op": "GreaterEqual", "Inputs": { "A": "albedo:5", "B": 0.5, "True": "lerp", "False": "base1" } },
			{ "Id": "sw", "Type": "StaticSwitch", "Parameter": "Fancy", "Inputs": { "True": "cmp", "False": "base1" } },
			{ "Id": "c", "Type": "Constant", "Value": [0.2, 0.3, 0.4] }
		], "Output": { "BaseColor": "sw", "Metallic": "clamp", "Roughness": "sat", "Normal": "normal", "AmbientOcclusion": "vc:5",
		               "Emissive": "c", "Opacity": "fres", "OpacityMask": "albedo:5" } }
	})";

	std::shared_ptr<const FMaterialShader> CompileGraph(const char* Json)
	{
		FMaterialAsset Asset;
		E_EXPECT_TRUE(Asset.FromJsonString(Json));
		const FMaterialGraphCompileResult Result = FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters);
		E_EXPECT_TRUE(Result.bSuccess);
		return Result.Shader;
	}
} // namespace

E_TEST(MaterialGraphShader_GeneratedHlslCompilesWithDxc)
{
	FD3D12ShaderCompiler Compiler;
	E_EXPECT_TRUE(Compiler.Init());
	if (!Compiler.IsAvailable())
	{
		return; // DXC 없는 환경 (패키지 등)
	}
	const std::shared_ptr<const FMaterialShader> Shader = CompileGraph(EveryNodeGraph);
	if (!Shader)
	{
		return;
	}
	// 메시 패스 픽셀 엔트리 6개 + 그래프 Masked 그림자 + 정적 스위치 끈 변형
	for (const FShaderCompileDesc& Desc : MaterialRender::GetGraphShaderDescs(*Shader))
	{
		E_EXPECT_TRUE(Compiler.Compile(Desc) != nullptr);
	}
	// 그림자 정점 셰이더 (머티리얼 무관)
	for (const wchar_t* Entry : { L"ShadowMaterialVS", L"ShadowMaterialSkinnedVS" })
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"Shadow.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = EShaderStage::Vertex;
		E_EXPECT_TRUE(Compiler.Compile(Desc) != nullptr);
	}
	// 레이 트레이싱/계산용 샘플 매크로 교체 (SampleLevel, 파생 함수 없음) — 계산 셰이더에서 쓸 수 있어야 한다
	{
		FShaderVirtualFile Generated{ L"MaterialGraph.generated.hlsli", Shader->Hlsl };
		FShaderVirtualFile Probe{ L"MaterialProbe.hlsl",
			                       "SamplerState LinearSampler : register(s0);\n"
			                       "SamplerState IblSampler : register(s1);\n"
			                       "#define MATERIAL_SAMPLE(Texture, Sampler, Uv) (Texture).SampleLevel(Sampler, Uv, 0.0f)\n"
			                       "#include \"MaterialCommon.hlsli\"\n"
			                       "#include \"MaterialGraph.generated.hlsli\"\n"
			                       "RWStructuredBuffer<float4> Result : register(u0);\n"
			                       "[numthreads(1, 1, 1)] void CSMain(uint3 Id : SV_DispatchThreadID)\n{\n"
			                       "\tFMaterialPixelInputs In = (FMaterialPixelInputs)0;\n"
			                       "\tIn.WorldNormal = float3(0, 0, 1); In.WorldTangent = float4(1, 0, 0, 1); In.CameraVector = float3(0, 0, 1);\n"
			                       "\tIn.UV0 = float2(Id.xy) / 8.0f; In.VertexColor = 1.0f; In.bFrontFace = true;\n"
			                       "\tFMaterialSurface Surface;\n\tEvaluateMaterial(In, Surface);\n"
			                       "\tResult[Id.x] = float4(Surface.BaseColor, Surface.OpacityMask) + float4(MaterialTangentToWorld(In.WorldNormal, In.WorldTangent, Surface.Normal), 0);\n}\n" };
		FShaderCompileDesc Desc; // 진입 파일도 가상 (엔진 셰이더 폴더 기준 포함은 디스크)
		Desc.FileName     = L"MaterialProbe.hlsl";
		Desc.EntryPoint   = L"CSMain";
		Desc.Stage        = EShaderStage::Compute;
		Desc.VirtualFiles = { Generated, Probe };
		E_EXPECT_TRUE(Compiler.Compile(Desc) != nullptr);
	}
}

E_TEST(MaterialGraphShader_VirtualFilesKeyCacheAndCookedName)
{
	const std::shared_ptr<const FMaterialShader> Shader = CompileGraph(EveryNodeGraph);
	if (!Shader)
	{
		return;
	}
	const FShaderCompileDesc A = MaterialRender::MakeGraphShaderDesc(L"Mesh.hlsl", L"PSMain", EShaderStage::Pixel, *Shader);
	const FShaderCompileDesc B = MaterialRender::MakeGraphShaderDesc(L"Mesh.hlsl", L"PSMain", EShaderStage::Pixel, *Shader);
	FShaderCompileDesc       Plain;
	Plain.FileName   = L"Mesh.hlsl";
	Plain.EntryPoint = L"PSMain";
	Plain.Stage      = EShaderStage::Pixel;
	// 같은 생성 소스 → 같은 키/파일, 기존(가상 파일 없음) 이름은 그대로
	E_EXPECT_TRUE(FShaderLibrary::MakeCacheKey(A) == FShaderLibrary::MakeCacheKey(B));
	E_EXPECT_TRUE(GetCookedShaderFileName(A, false) == GetCookedShaderFileName(B, false));
	E_EXPECT_TRUE(GetCookedShaderFileName(Plain, false) == L"Mesh_PSMain_Pixel.dxil");
	E_EXPECT_TRUE(HashShaderVariant(Plain) == 0);
	// 생성 소스가 다르면 다른 키/파일
	FShaderCompileDesc C = A;
	C.VirtualFiles[0].Content += "// 다른 그래프\n";
	E_EXPECT_TRUE(FShaderLibrary::MakeCacheKey(A) != FShaderLibrary::MakeCacheKey(C));
	E_EXPECT_TRUE(GetCookedShaderFileName(A, false) != GetCookedShaderFileName(C, false));
	// 디파인만 같은 다른 변형과도 구분 (디파인 해시 + 가상 파일)
	FShaderCompileDesc DefinesOnly = Plain;
	DefinesOnly.Defines            = A.Defines;
	E_EXPECT_TRUE(GetCookedShaderFileName(DefinesOnly, false) != GetCookedShaderFileName(A, false));

	// 쿠킹 → 다른 라이브러리(빈 메모리 캐시)가 같은 파일을 읽는다
	FD3D12ShaderCompiler Compiler;
	E_EXPECT_TRUE(Compiler.Init());
	if (!Compiler.IsAvailable())
	{
		return;
	}
	const std::filesystem::path CookedDir = FTestRegistry::GetTempDirectory() / L"ProjectE_RhiTests" / L"MaterialGraphCooked";
	std::error_code             ErrorCode;
	std::filesystem::remove_all(CookedDir, ErrorCode);
	FShaderLibrary Writer;
	Writer.Init(Compiler, CookedDir, true);
	E_EXPECT_TRUE(Writer.CookShader(A));
	FShaderLibrary Reader;
	Reader.Init(Compiler, CookedDir, false);
	E_EXPECT_TRUE(Reader.GetShader(B) != nullptr);
	E_EXPECT_EQ(Reader.GetStats().CookedLoads, 1u);
	E_EXPECT_EQ(Reader.GetStats().Compiles, 0u);
	E_EXPECT_TRUE(Reader.GetShader(B) != nullptr);
	E_EXPECT_EQ(Reader.GetStats().MemoryHits, 1u);
}
