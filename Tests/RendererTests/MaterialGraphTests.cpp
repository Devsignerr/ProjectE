#include "Core/Testing/TestFramework.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/MaterialGraph.h"

#include <algorithm>
#include <map>
#include <string>

// Phase 49 사이드: 머티리얼 그래프 → HLSL 컴파일러 순수 로직 (규칙은 Renderer/MaterialGraph.h 머리 주석)

namespace
{
	constexpr float Tol = 1.0e-5f;

	FMaterialAsset Parse(const std::string& Json)
	{
		FMaterialAsset Asset;
		E_EXPECT_TRUE(Asset.FromJsonString(Json));
		return Asset;
	}

	FMaterialGraphCompileResult CompileJson(const std::string& Json)
	{
		const FMaterialAsset Asset = Parse(Json);
		return FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters);
	}

	bool Contains(const std::string& Text, const std::string& Part) { return Text.find(Part) != std::string::npos; }

	bool AnyErrorContains(const FMaterialGraphCompileResult& Result, const std::string& Part)
	{
		for (const std::string& Error : Result.Errors)
		{
			if (Contains(Error, Part))
			{
				return true;
			}
		}
		return false;
	}

	size_t CountOf(const std::string& Text, const std::string& Part)
	{
		size_t Count = 0;
		for (size_t Position = Text.find(Part); Position != std::string::npos; Position = Text.find(Part, Position + Part.size()))
		{
			++Count;
		}
		return Count;
	}

	const char* const SimpleGraph = R"({
		"Name": "Simple",
		"Parameters": [
			{ "Name": "Tint", "Type": "Vector", "Value": [1, 0.5, 0.25, 1] },
			{ "Name": "Rough", "Type": "Scalar", "Value": 0.3 },
			{ "Name": "Albedo", "Type": "Texture", "Value": "Albedo.png", "Usage": "Color" }
		],
		"Graph": {
			"Nodes": [
				{ "Id": "uv", "Type": "TexCoord", "Tiling": [2, 2] },
				{ "Id": "tex", "Type": "TextureSample", "Texture": "Albedo", "Inputs": { "UV": "uv" } },
				{ "Id": "tint", "Type": "VectorParameter", "Parameter": "Tint" },
				{ "Id": "mul", "Type": "Multiply", "Inputs": { "A": "tex:1", "B": "tint:1" } },
				{ "Id": "rough", "Type": "ScalarParameter", "Parameter": "Rough" }
			],
			"Output": { "BaseColor": "mul", "Roughness": "rough" }
		}
	})";
} // namespace

E_TEST(MaterialGraph_CompilesSimpleGraph)
{
	const FMaterialGraphCompileResult Result = CompileJson(SimpleGraph);
	E_EXPECT_TRUE(Result.bSuccess);
	if (!Result.bSuccess)
	{
		return;
	}
	const std::string& Hlsl = Result.Shader->Hlsl;
	E_EXPECT_TRUE(Contains(Hlsl, "void EvaluateMaterial(in FMaterialPixelInputs In, out FMaterialSurface Out)"));
	E_EXPECT_TRUE(Contains(Hlsl, "MATERIAL_SAMPLE(E_MATERIAL_TEXTURE(0), E_MATERIAL_SAMPLER_WRAP,"));
	E_EXPECT_TRUE(Contains(Hlsl, "In.UV0 * float2(2.0f, 2.0f)"));
	E_EXPECT_TRUE(Contains(Hlsl, "Out.Roughness = E_MATERIAL_PARAM(1).x;"));
	E_EXPECT_FALSE(Contains(Hlsl, "ddx") || Contains(Hlsl, "ddy") || Contains(Hlsl, ".Sample("));
	// 연결 안 된 출력은 기본값
	E_EXPECT_TRUE(Contains(Hlsl, "Out.Normal = float3(0.0f, 0.0f, 1.0f);"));
	E_EXPECT_TRUE(Contains(Hlsl, "Out.OpacityMask = 1.0f;"));
	E_EXPECT_TRUE(Result.Shader->Hash == FMaterialGraphCompiler::HashText(Hlsl));
}

E_TEST(MaterialGraph_ParameterLayoutPacking)
{
	// 벡터 2개(레지스터 0, 1) → 스칼라 5개(레지스터 2.xyzw, 3.x) → 텍스처 칸 0, 1. 쓰지 않는 파라미터는 빠진다
	const FMaterialGraphCompileResult Result = CompileJson(R"({
		"Parameters": [
			{ "Name": "S0", "Value": 1 }, { "Name": "V0", "Value": [1, 2, 3, 4] }, { "Name": "S1", "Value": 2 },
			{ "Name": "Unused", "Value": 9 }, { "Name": "T0", "Value": "a.png", "Usage": "Normal" }, { "Name": "S2", "Value": 3 },
			{ "Name": "V1", "Value": [5, 6, 7] }, { "Name": "S3", "Value": 4 }, { "Name": "S4", "Value": 5 }, { "Name": "T1", "Value": "b.png" }
		],
		"Graph": { "Nodes": [
			{ "Id": "s0", "Type": "ScalarParameter", "Parameter": "S0" }, { "Id": "s1", "Type": "ScalarParameter", "Parameter": "S1" },
			{ "Id": "s2", "Type": "ScalarParameter", "Parameter": "S2" }, { "Id": "s3", "Type": "ScalarParameter", "Parameter": "S3" },
			{ "Id": "s4", "Type": "ScalarParameter", "Parameter": "S4" },
			{ "Id": "v0", "Type": "VectorParameter", "Parameter": "V0" }, { "Id": "v1", "Type": "VectorParameter", "Parameter": "V1" },
			{ "Id": "t0", "Type": "TextureSample", "Texture": "T0" }, { "Id": "t1", "Type": "TextureSample", "Texture": "T1" },
			{ "Id": "a", "Type": "Add", "Inputs": { "A": "s0", "B": "s1" } }, { "Id": "b", "Type": "Add", "Inputs": { "A": "a", "B": "s2" } },
			{ "Id": "c", "Type": "Add", "Inputs": { "A": "b", "B": "s3" } }, { "Id": "d", "Type": "Add", "Inputs": { "A": "c", "B": "s4" } },
			{ "Id": "e", "Type": "Add", "Inputs": { "A": "v0", "B": "v1" } }, { "Id": "f", "Type": "Add", "Inputs": { "A": "t0", "B": "t1" } }
		], "Output": { "Roughness": "d", "BaseColor": "e", "Emissive": "f" } }
	})");
	E_EXPECT_TRUE(Result.bSuccess);
	if (!Result.bSuccess)
	{
		return;
	}
	const FMaterialParameterLayout& Layout = Result.Shader->Layout;
	E_EXPECT_EQ(Layout.ConstantRegisters, 4u);
	E_EXPECT_EQ(Layout.TextureCount, 2u);
	E_EXPECT_TRUE(Layout.Find("Unused") == nullptr);
	const auto Check = [&](const char* Name, uint32 Register, uint32 Component) {
		const FMaterialParameterSlot* Slot = Layout.Find(Name);
		E_EXPECT_TRUE(Slot != nullptr && Slot->Register == Register && Slot->Component == Component);
	};
	Check("V0", 0, 0);
	Check("V1", 1, 0);
	Check("S0", 2, 0);
	Check("S1", 2, 1);
	Check("S2", 2, 2);
	Check("S3", 2, 3);
	Check("S4", 3, 0);
	Check("T0", 0, 0);
	Check("T1", 1, 0);
	E_EXPECT_TRUE(Layout.Find("T0")->Usage == ETextureUsage::Normal);
	E_EXPECT_TRUE(Contains(Result.Shader->Hlsl, "MaterialDecodeNormal(MATERIAL_SAMPLE(E_MATERIAL_TEXTURE(0)"));
	E_EXPECT_TRUE(Contains(Result.Shader->Hlsl, "float4 MaterialParams[4];"));

	// 값 → 상수 (벡터 [5,6,7]은 알파 1)
	const FMaterialAsset          Asset     = Parse(R"({ "Parameters": [ { "Name": "S0", "Value": 1 }, { "Name": "V0", "Value": [1, 2, 3, 4] },
		{ "Name": "S1", "Value": 2 }, { "Name": "S2", "Value": 3 }, { "Name": "V1", "Value": [5, 6, 7] }, { "Name": "S3", "Value": 4 }, { "Name": "S4", "Value": 5 } ] })");
	const std::vector<FVector4>   Constants = Layout.BuildConstants(Asset.Parameters);
	E_EXPECT_EQ(Constants.size(), static_cast<size_t>(4));
	E_EXPECT_NEAR(Constants[0].W, 4.0f, Tol);
	E_EXPECT_NEAR(Constants[1].Z, 7.0f, Tol);
	E_EXPECT_NEAR(Constants[1].W, 1.0f, Tol);
	E_EXPECT_NEAR(Constants[2].X, 1.0f, Tol);
	E_EXPECT_NEAR(Constants[2].W, 4.0f, Tol);
	E_EXPECT_NEAR(Constants[3].X, 5.0f, Tol);
}

E_TEST(MaterialGraph_TypeRules)
{
	// float1 확장은 허용 (float3 × float1)
	E_EXPECT_TRUE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "m", "Type": "Multiply", "Inputs": { "A": [1, 2, 3], "B": 0.5 } } ],
		"Output": { "BaseColor": "m" } } })").bSuccess);
	// float2 + float3은 오류 (자르지 않는다)
	const FMaterialGraphCompileResult Mismatch = CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "bad", "Type": "Add", "Inputs": { "A": [1, 2], "B": [1, 2, 3] } } ],
		"Output": { "BaseColor": "bad" } } })");
	E_EXPECT_FALSE(Mismatch.bSuccess);
	E_EXPECT_TRUE(AnyErrorContains(Mismatch, "[bad]"));
	// 출력 핀은 자르기 허용 (float4 → BaseColor float3)
	const FMaterialGraphCompileResult Truncate = CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "c", "Type": "Constant", "Value": [1, 0, 0, 1] } ],
		"Output": { "BaseColor": "c", "Metallic": "c" } } })");
	E_EXPECT_TRUE(Truncate.bSuccess);
	E_EXPECT_TRUE(Truncate.bSuccess && Contains(Truncate.Shader->Hlsl, "Out.BaseColor = (float4(1.0f, 0.0f, 0.0f, 1.0f)).xyz;"));
	// Cross는 float3만, Append는 합 ≤ 4, ComponentMask 채널은 입력 너비 안
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "Cross", "Inputs": { "A": [1, 0], "B": [0, 1, 0] } } ], "Output": { "Normal": "x" } } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "Append", "Inputs": { "A": [1, 0, 0], "B": [0, 1] } } ], "Output": { "BaseColor": "x" } } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "ComponentMask", "Channels": "z", "Inputs": { "A": [1, 0] } } ], "Output": { "Metallic": "x" } } })").bSuccess);
	const FMaterialGraphCompileResult Append = CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "Append", "Inputs": { "A": [1, 0], "B": 0.5 } } ], "Output": { "BaseColor": "x" } } })");
	E_EXPECT_TRUE(Append.bSuccess && Contains(Append.Shader->Hlsl, "float3(float2(1.0f, 0.0f), 0.5f)"));
	// Dot → float1, Split 출력은 입력 너비 안만
	E_EXPECT_TRUE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "d", "Type": "Dot", "Inputs": { "A": [1, 0, 0], "B": [0, 1, 0] } } ], "Output": { "Roughness": "d" } } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "s", "Type": "Split", "Inputs": { "A": [1, 0] } } ], "Output": { "Roughness": "s:2" } } })").bSuccess);
	// 파라미터 타입 불일치
	E_EXPECT_FALSE(CompileJson(R"({ "Parameters": [ { "Name": "P", "Value": 1 } ],
		"Graph": { "Nodes": [ { "Id": "v", "Type": "VectorParameter", "Parameter": "P" } ], "Output": { "BaseColor": "v:1" } } })").bSuccess);
}

E_TEST(MaterialGraph_StructureErrors)
{
	// 순환 (쓰지 않는 노드끼리여도)
	const FMaterialGraphCompileResult Cycle = CompileJson(R"({ "Graph": { "Nodes": [
		{ "Id": "a", "Type": "Add", "Inputs": { "A": "b", "B": 1 } }, { "Id": "b", "Type": "Abs", "Inputs": { "A": "a" } } ],
		"Output": { "Roughness": 0.5 } } })");
	E_EXPECT_FALSE(Cycle.bSuccess);
	E_EXPECT_TRUE(AnyErrorContains(Cycle, "순환"));
	// 미연결 필수 입력
	const FMaterialGraphCompileResult Missing = CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "l", "Type": "Lerp", "Inputs": { "A": 0, "B": 1 } } ],
		"Output": { "Roughness": "l" } } })");
	E_EXPECT_FALSE(Missing.bSuccess);
	E_EXPECT_TRUE(AnyErrorContains(Missing, "Alpha"));
	// 없는 노드 참조, 알 수 없는 종류/핀/출력, 중복 Id
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [], "Output": { "BaseColor": "nope" } } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "Frobnicate" } ], "Output": {} } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "Abs", "Inputs": { "Q": 1 } } ], "Output": {} } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [], "Output": { "Shininess": 1 } } })").bSuccess);
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "x", "Type": "Time" }, { "Id": "x", "Type": "Time" } ], "Output": {} } })").bSuccess);
	// 정점 형식에 없는 UV 채널
	E_EXPECT_FALSE(CompileJson(R"({ "Graph": { "Nodes": [ { "Id": "uv", "Type": "TexCoord", "Channel": 1 } ], "Output": { "BaseColor": "uv" } } })").bSuccess);
}

E_TEST(MaterialGraph_UnusedNodesAndCommonSubexpressions)
{
	// 출력에 닿지 않는 노드(오류가 있어도)는 코드에 들어가지 않고, 같은 계산을 하는 다른 노드는 지역 변수 하나를 공유한다
	const FMaterialGraphCompileResult Result = CompileJson(R"({ "Graph": { "Nodes": [
		{ "Id": "t", "Type": "Time" },
		{ "Id": "s1", "Type": "Sine", "Inputs": { "A": "t" } },
		{ "Id": "s2", "Type": "Sine", "Inputs": { "A": "t" } },
		{ "Id": "sum", "Type": "Add", "Inputs": { "A": "s1", "B": "s2" } },
		{ "Id": "orphan", "Type": "Cosine", "Inputs": { "A": "t" } },
		{ "Id": "orphanBad", "Type": "Lerp", "Inputs": { "A": 1 } } ],
		"Output": { "Roughness": "sum", "Metallic": "s2" } } })");
	E_EXPECT_TRUE(Result.bSuccess);
	if (!Result.bSuccess)
	{
		return;
	}
	E_EXPECT_EQ(CountOf(Result.Shader->Hlsl, "sin("), static_cast<size_t>(1));
	E_EXPECT_EQ(CountOf(Result.Shader->Hlsl, "cos("), static_cast<size_t>(0));
	E_EXPECT_TRUE(Contains(Result.Shader->Hlsl, "Local0 + Local0"));
}

E_TEST(MaterialGraph_DeterministicOutput)
{
	// 같은 그래프 → 같은 문자열·해시. 노드 Id/순서/편집기 위치가 달라도 의미가 같으면 같은 HLSL
	const FMaterialGraphCompileResult A = CompileJson(SimpleGraph);
	const FMaterialGraphCompileResult B = CompileJson(SimpleGraph);
	E_EXPECT_TRUE(A.bSuccess && B.bSuccess && A.Shader->Hlsl == B.Shader->Hlsl && A.Shader->Hash == B.Shader->Hash);

	const FMaterialGraphCompileResult Renamed = CompileJson(R"({
		"Parameters": [
			{ "Name": "Tint", "Type": "Vector", "Value": [0, 0, 0, 1] },
			{ "Name": "Rough", "Type": "Scalar", "Value": 0.9 },
			{ "Name": "Albedo", "Type": "Texture", "Value": "Other.png", "Usage": "Color" }
		],
		"Graph": {
			"Nodes": [
				{ "Id": "R", "Type": "ScalarParameter", "Parameter": "Rough", "EditorPosition": [100, 20] },
				{ "Id": "M", "Type": "Multiply", "Inputs": { "A": "T:1", "B": "C:1" } },
				{ "Id": "C", "Type": "VectorParameter", "Parameter": "Tint" },
				{ "Id": "T", "Type": "TextureSample", "Texture": "Albedo", "Inputs": { "UV": "U" } },
				{ "Id": "U", "Type": "TexCoord", "Tiling": [2, 2] }
			],
			"Output": { "Roughness": "R", "BaseColor": "M" }
		}
	})");
	E_EXPECT_TRUE(Renamed.bSuccess && A.bSuccess && Renamed.Shader->Hlsl == A.Shader->Hlsl); // 파라미터 값/텍스처 경로는 셰이더에 없다

	// 그래프가 바뀌면 해시가 바뀐다
	FMaterialAsset Changed = Parse(SimpleGraph);
	Changed.Graph.Nodes[0].Value.X = 3.0f;
	const FMaterialGraphCompileResult C = FMaterialGraphCompiler::Compile(Changed.Graph, Changed.Parameters);
	E_EXPECT_TRUE(C.bSuccess && A.bSuccess && C.Shader->Hash != A.Shader->Hash);
}

E_TEST(MaterialGraph_StaticSwitchSelectsBranch)
{
	const std::string Json = R"({
		"Parameters": [ { "Name": "UseTexture", "Type": "StaticSwitch", "Value": true }, { "Name": "Tex", "Value": "t.png" } ],
		"Graph": { "Nodes": [
			{ "Id": "tex", "Type": "TextureSample", "Texture": "Tex" },
			{ "Id": "sw", "Type": "StaticSwitch", "Parameter": "UseTexture", "Inputs": { "True": "tex:1", "False": [0.2, 0.2, 0.2] } } ],
			"Output": { "BaseColor": "sw" } } })";
	FMaterialAsset                    Asset = Parse(Json);
	const FMaterialGraphCompileResult On    = FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters);
	E_EXPECT_TRUE(On.bSuccess && On.Shader->Layout.TextureCount == 1 && Contains(On.Shader->Hlsl, "MATERIAL_SAMPLE"));
	Asset.FindParameter("UseTexture")->Value.X = 0.0f;
	const FMaterialGraphCompileResult Off = FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters);
	E_EXPECT_TRUE(Off.bSuccess && Off.Shader->Layout.TextureCount == 0 && !Contains(Off.Shader->Hlsl, "MATERIAL_SAMPLE"));
	E_EXPECT_TRUE(On.bSuccess && Off.bSuccess && On.Shader->Hash != Off.Shader->Hash);
}

E_TEST(MaterialGraph_JsonRoundTrip)
{
	const FMaterialAsset Original = Parse(R"({
		"Name": "Round",
		"BlendMode": "Masked",
		"Parameters": [
			{ "Name": "S", "Type": "Scalar", "Value": 0.25 }, { "Name": "V", "Type": "Vector", "Value": [1, 2, 3, 4] },
			{ "Name": "T", "Type": "Texture", "Value": "../x.png", "Usage": "Mask" }, { "Name": "B", "Type": "StaticSwitch", "Value": true }
		],
		"Graph": { "Nodes": [
			{ "Id": "c", "Type": "Constant", "Value": [0.5, 0.25] },
			{ "Id": "uv", "Type": "TexCoord", "Tiling": [3, 4], "EditorPosition": [10, -20] },
			{ "Id": "m", "Type": "ComponentMask", "Channels": "yx", "Inputs": { "A": "uv" } },
			{ "Id": "t", "Type": "TextureSample", "Texture": "T", "Sampler": "Clamp", "Inputs": { "UV": "m" } },
			{ "Id": "cmp", "Type": "Compare", "Op": "Less", "Inputs": { "A": "t:2", "B": 0.5, "True": 1, "False": 0 } }
		], "Output": { "OpacityMask": "cmp", "BaseColor": [0.1, 0.2, 0.3], "Roughness": "t:5" } }
	})");
	const FMaterialAsset Loaded = Parse(Original.ToJsonString());
	E_EXPECT_TRUE(Loaded.IsGraphMaterial());
	E_EXPECT_EQ(Loaded.Parameters.size(), static_cast<size_t>(4));
	E_EXPECT_TRUE(Loaded.FindParameter("T") != nullptr && Loaded.FindParameter("T")->Usage == ETextureUsage::Mask &&
	              Loaded.FindParameter("T")->Texture == "../x.png");
	E_EXPECT_TRUE(Loaded.FindParameter("B") != nullptr && Loaded.FindParameter("B")->Type == EMaterialParameterType::StaticSwitch &&
	              Loaded.FindParameter("B")->GetBool());
	E_EXPECT_EQ(Loaded.Graph.Nodes.size(), static_cast<size_t>(5));
	E_EXPECT_TRUE(Loaded.Graph.FindNode("uv")->EditorPosition.Y == -20.0f && Loaded.Graph.FindNode("uv")->Value.Y == 4.0f);
	E_EXPECT_TRUE(Loaded.Graph.FindNode("t")->Option == "Clamp" && Loaded.Graph.FindNode("cmp")->Option == "Less");
	E_EXPECT_TRUE(Loaded.Graph.FindNode("t")->FindInput("UV")->Node == "m");
	// 같은 HLSL
	const FMaterialGraphCompileResult A = FMaterialGraphCompiler::Compile(Original.Graph, Original.Parameters);
	const FMaterialGraphCompileResult B = FMaterialGraphCompiler::Compile(Loaded.Graph, Loaded.Parameters);
	E_EXPECT_TRUE(A.bSuccess && B.bSuccess && A.Shader->Hlsl == B.Shader->Hlsl);
	E_EXPECT_TRUE(A.bSuccess && Contains(A.Shader->Hlsl, "E_MATERIAL_SAMPLER_CLAMP") && Contains(A.Shader->Hlsl, ".yx"));

	// 그래프 없는 기존 .emat은 Parameters/Graph 키를 쓰지 않는다
	const FMaterialAsset Plain = Parse(R"({ "Name": "Plain", "Roughness": 0.4 })");
	E_EXPECT_FALSE(Plain.IsGraphMaterial());
	E_EXPECT_FALSE(Contains(Plain.ToJsonString(), "Graph") || Contains(Plain.ToJsonString(), "Parameters"));
}

E_TEST(MaterialGraph_InstanceOverridesParametersOnly)
{
	std::map<std::wstring, std::string> Files;
	Files[FMaterialAsset::MakePathKey(L"C:/Content/Mat/Base.emat")] = R"({
		"Name": "Base", "BlendMode": "Masked",
		"Parameters": [ { "Name": "Color", "Value": [1, 0, 0, 1] }, { "Name": "Tex", "Value": "Base.png" }, { "Name": "Gloss", "Value": 0.5 } ],
		"Graph": { "Nodes": [ { "Id": "c", "Type": "VectorParameter", "Parameter": "Color" } ], "Output": { "BaseColor": "c:1" } } })";
	const FMaterialAsset::FLoader Loader = [&Files](const std::filesystem::path& Path, FMaterialAsset& Out) {
		const auto Found = Files.find(FMaterialAsset::MakePathKey(Path));
		return Found != Files.end() && Out.FromJsonString(Found->second);
	};
	const FMaterialAsset Child = Parse(R"({ "Name": "Child", "Parent": "../Mat/Base.emat",
		"Parameters": [ { "Name": "Color", "Value": [0, 1, 0, 1] }, { "Name": "Tex", "Value": "Child.png" }, { "Name": "Missing", "Value": 3 },
		                { "Name": "Gloss", "Value": [1, 1, 1, 1] } ],
		"Graph": { "Nodes": [], "Output": { "BaseColor": [0, 0, 1] } } })");
	FMaterialAsset Resolved;
	E_EXPECT_TRUE(FMaterialAsset::Resolve(Child, L"C:/Content/Inst/Child.emat", Loader, Resolved));
	E_EXPECT_TRUE(Resolved.IsGraphMaterial());
	E_EXPECT_TRUE(Resolved.BlendMode == EMaterialBlendMode::Masked);
	E_EXPECT_EQ(Resolved.Parameters.size(), static_cast<size_t>(3)); // 부모 목록 (없는 이름은 무시)
	E_EXPECT_NEAR(Resolved.FindParameter("Color")->Value.Y, 1.0f, Tol);
	E_EXPECT_NEAR(Resolved.FindParameter("Gloss")->Value.X, 0.5f, Tol); // 타입이 다른 덮어쓰기는 무시
	E_EXPECT_TRUE(Resolved.FindParameter("Tex")->Texture == "Child.png");
	// 그래프는 부모 것 (인스턴스의 Graph 무시) → 부모와 같은 셰이더
	E_EXPECT_EQ(Resolved.Graph.Nodes.size(), static_cast<size_t>(1));
	FMaterialAsset Parent;
	E_EXPECT_TRUE(Loader(L"C:/Content/Mat/Base.emat", Parent));
	const FMaterialGraphCompileResult ParentShader = FMaterialGraphCompiler::Compile(Parent.Graph, Parent.Parameters);
	const FMaterialGraphCompileResult ChildShader  = FMaterialGraphCompiler::Compile(Resolved.Graph, Resolved.Parameters);
	E_EXPECT_TRUE(ParentShader.bSuccess && ChildShader.bSuccess && ParentShader.Shader->Hash == ChildShader.Shader->Hash);

	// 부모 텍스처 경로는 자식 폴더 기준으로 다시 쓴다 (덮어쓰지 않은 경우)
	const FMaterialAsset Child2 = Parse(R"({ "Parent": "../Mat/Base.emat", "Parameters": [] })");
	FMaterialAsset       Resolved2;
	E_EXPECT_TRUE(FMaterialAsset::Resolve(Child2, L"C:/Content/Inst/Child2.emat", Loader, Resolved2));
	E_EXPECT_TRUE(Resolved2.FindParameter("Tex")->Texture == "../Mat/Base.png");
}

E_TEST(MaterialGraph_AllNodeTypesCompile)
{
	// 노드 표의 모든 종류가 최소 입력으로 코드가 되는지 (새 노드를 추가하면 여기에 입력 예시를 넣는다)
	const std::map<std::string, std::string> Inputs = {
		{ "Constant", R"("Value": 1)" },
		{ "ScalarParameter", R"("Parameter": "S")" },
		{ "VectorParameter", R"("Parameter": "V")" },
		{ "StaticSwitch", R"("Parameter": "B", "Inputs": { "True": 1, "False": 0 })" },
		{ "TextureSample", R"("Texture": "T")" },
		{ "TexCoord", "" },
		{ "VertexColor", "" },
		{ "WorldPosition", "" },
		{ "WorldNormal", "" },
		{ "CameraVector", "" },
		{ "Time", "" },
		{ "Clamp", R"("Inputs": { "A": 2 })" },
		{ "Lerp", R"("Inputs": { "A": 0, "B": 1, "Alpha": 0.5 })" },
		{ "Compare", R"("Op": "Equal", "Inputs": { "A": 0, "B": 1, "True": 1, "False": 0 })" },
		{ "Fresnel", "" },
		{ "Panner", R"("Inputs": { "Speed": [1, 0] })" },
		{ "BlendNormals", R"("Inputs": { "A": [0, 0, 1], "B": [0, 0, 1] })" },
		{ "ComponentMask", R"("Channels": "x", "Inputs": { "A": [1, 2] })" },
		{ "Split", R"("Inputs": { "A": [1, 2] })" },
		{ "Cross", R"("Inputs": { "A": [1, 0, 0], "B": [0, 1, 0] })" },
	};
	for (const std::string& Type : FMaterialGraphCompiler::GetNodeTypes())
	{
		static const std::string Unary[] = { "Saturate", "Abs", "Frac", "Floor", "OneMinus", "Sine", "Cosine", "Normalize", "Length" };
		const bool  bUnary   = std::find(std::begin(Unary), std::end(Unary), Type) != std::end(Unary);
		std::string Settings = bUnary ? R"("Inputs": { "A": [1, 2, 3] })" : R"("Inputs": { "A": 1, "B": 2 })"; // 단항/이항 산술 기본
		if (const auto Found = Inputs.find(Type); Found != Inputs.end())
		{
			Settings = Found->second;
		}
		const std::string Json = std::string(R"({ "Parameters": [ { "Name": "S", "Value": 1 }, { "Name": "V", "Value": [1, 1, 1, 1] },
			{ "Name": "T", "Value": "t.png" }, { "Name": "B", "Value": true } ], "Graph": { "Nodes": [ { "Id": "n", "Type": ")") +
		                         Type + "\"" + (Settings.empty() ? "" : ", ") + Settings + R"( } ], "Output": { "Roughness": "n" } } })";
		const FMaterialGraphCompileResult Result = CompileJson(Json);
		E_EXPECT_TRUE(Result.bSuccess);
	}
}
