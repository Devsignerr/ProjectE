#include "Core/Testing/TestFramework.h"
#include "Editor/AssetEditors/MaterialGraphEditing.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/MaterialGraph.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// Phase 51 사이드: 노드 머티리얼 편집기의 순수 편집 연산 (MaterialGraphEditing.h 머리 주석이 기준)

namespace
{
	namespace fs = std::filesystem;

	FMaterialAsset Parse(const std::string& Json)
	{
		FMaterialAsset Asset;
		E_EXPECT_TRUE(Asset.FromJsonString(Json));
		return Asset;
	}

	const FMaterialGraphInput* FindLink(const FMaterialGraph& Graph, const std::string& Node, const std::string& Pin)
	{
		return MaterialGraphEditing::FindInput(Graph, Node, Pin);
	}

	struct FRect
	{
		FVector2 Min;
		FVector2 Max;
	};

	bool Overlaps(const FRect& A, const FRect& B)
	{
		return A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y;
	}

	fs::path FindSampleMaterialGraphFolder()
	{
		for (fs::path Dir = fs::current_path(); !Dir.empty(); Dir = Dir.parent_path())
		{
			if (fs::exists(Dir / L"Projects/Sample/Content/MaterialGraph/Lava.emat"))
			{
				return Dir / L"Projects/Sample/Content/MaterialGraph";
			}
			if (Dir == Dir.parent_path())
			{
				break;
			}
		}
		return {};
	}

	std::vector<fs::path> SampleMaterials()
	{
		std::vector<fs::path> Files;
		const fs::path        Folder = FindSampleMaterialGraphFolder();
		E_EXPECT_FALSE(Folder.empty());
		if (!Folder.empty())
		{
			for (const fs::directory_entry& Entry : fs::directory_iterator(Folder))
			{
				if (Entry.path().extension() == L".emat")
				{
					Files.push_back(Entry.path());
				}
			}
		}
		std::sort(Files.begin(), Files.end());
		return Files;
	}

	const FMaterialAsset::FLoader DiskLoader = [](const fs::path& Path, FMaterialAsset& Out) { return Out.LoadFromFile(Path); };

	uint64 ResolvedHash(const FMaterialAsset& Asset, const fs::path& Path)
	{
		FMaterialAsset Resolved;
		FMaterialAsset::Resolve(Asset, Path, DiskLoader, Resolved);
		const FMaterialGraphCompileResult Result = FMaterialGraphCompiler::Compile(Resolved.Graph, Resolved.Parameters);
		return Result.bSuccess ? Result.Shader->Hash : 0;
	}

	const char* const ChainGraph = R"({ "Parameters": [ { "Name": "S", "Value": 2 } ], "Graph": { "Nodes": [
		{ "Id": "a", "Type": "Constant", "Value": 0.5 },
		{ "Id": "t", "Type": "Time" },
		{ "Id": "mul", "Type": "Multiply", "Inputs": { "A": "a", "B": "t" } },
		{ "Id": "s", "Type": "ScalarParameter", "Parameter": "S" },
		{ "Id": "add", "Type": "Add", "Inputs": { "A": "mul", "B": "s" } },
		{ "Id": "lonely", "Type": "Sine", "Inputs": { "A": 1 } } ],
		"Output": { "Roughness": "add", "Metallic": "s" } } })";
} // namespace

E_TEST(MaterialGraphEditing_AutoLayoutColumnsFromOutput)
{
	FMaterialAsset Asset = Parse(ChainGraph);
	E_EXPECT_TRUE(MaterialGraphEditing::HasMissingPositions(Asset.Graph));
	MaterialGraphEditing::AutoLayout(Asset.Graph, false);
	E_EXPECT_FALSE(MaterialGraphEditing::HasMissingPositions(Asset.Graph));
	const auto X = [&](const char* Id) { return Asset.Graph.FindNode(Id)->EditorPosition.X; };
	const float Column = MaterialGraphEditing::ColumnSpacing;
	E_EXPECT_NEAR(Asset.Graph.OutputEditorPosition.X, 0.0f, 1.0e-3f);
	// 열 = 출력까지 가장 긴 경로: add 1, mul 2, a/t 3, s는 출력(Metallic)과 add 둘 다 쓰지만 가장 긴 경로 2
	E_EXPECT_NEAR(X("add"), -Column, 1.0e-3f);
	E_EXPECT_NEAR(X("mul"), -2.0f * Column, 1.0e-3f);
	E_EXPECT_NEAR(X("s"), -2.0f * Column, 1.0e-3f);
	E_EXPECT_NEAR(X("a"), -3.0f * Column, 1.0e-3f);
	E_EXPECT_NEAR(X("t"), -3.0f * Column, 1.0e-3f);
	// 출력에서 닿지 않는 노드: 소비자가 없으면 열 1, 닿는 노드 아래
	E_EXPECT_NEAR(X("lonely"), -Column, 1.0e-3f);
	E_EXPECT_TRUE(Asset.Graph.FindNode("lonely")->EditorPosition.Y > Asset.Graph.FindNode("add")->EditorPosition.Y);
	// 결정적: 다시 해도 같은 위치
	FMaterialGraph Again = Asset.Graph;
	MaterialGraphEditing::AutoLayout(Again, false);
	for (size_t Index = 0; Index < Again.Nodes.size(); ++Index)
	{
		E_EXPECT_NEAR(Again.Nodes[Index].EditorPosition.Y, Asset.Graph.Nodes[Index].EditorPosition.Y, 1.0e-3f);
	}
	// bOnlyMissing: 위치가 있는 노드는 그대로
	Asset.Graph.FindNode("a")->EditorPosition = FVector2(500.0f, 500.0f);
	Asset.Graph.FindNode("t")->EditorPosition = FVector2::ZeroVector;
	MaterialGraphEditing::AutoLayout(Asset.Graph, true);
	E_EXPECT_NEAR(X("a"), 500.0f, 1.0e-3f);
	E_EXPECT_NEAR(X("t"), -3.0f * Column, 1.0e-3f);
}

E_TEST(MaterialGraphEditing_AutoLayoutHandlesCycles)
{
	// 편집 중 순환이 있어도 끝나야 한다 (컴파일러는 오류)
	FMaterialAsset Asset = Parse(R"({ "Parameters": [], "Graph": { "Nodes": [
		{ "Id": "x", "Type": "Add", "Inputs": { "A": "y", "B": 1 } },
		{ "Id": "y", "Type": "Add", "Inputs": { "A": "x", "B": 1 } } ], "Output": { "Roughness": "x" } } })");
	MaterialGraphEditing::AutoLayout(Asset.Graph, false);
	E_EXPECT_FALSE(MaterialGraphEditing::HasMissingPositions(Asset.Graph));
}

E_TEST(MaterialGraphEditing_SampleMaterialsLayoutWithoutOverlap)
{
	// 예제 그래프 머티리얼: 위치를 지우고 자동 배치하면 노드(추정 크기)가 겹치지 않는다
	uint32 Checked = 0;
	for (const fs::path& Path : SampleMaterials())
	{
		FMaterialAsset Asset;
		E_EXPECT_TRUE(Asset.LoadFromFile(Path));
		if (!Asset.IsGraphMaterial() || Asset.IsInstance())
		{
			continue;
		}
		for (FMaterialGraphNode& Node : Asset.Graph.Nodes)
		{
			Node.EditorPosition = FVector2::ZeroVector;
		}
		Asset.Graph.OutputEditorPosition = FVector2::ZeroVector;
		MaterialGraphEditing::AutoLayout(Asset.Graph, false);
		std::vector<FRect> Rects;
		Rects.push_back({ Asset.Graph.OutputEditorPosition, Asset.Graph.OutputEditorPosition + MaterialGraphEditing::OutputNodeSize() });
		for (const FMaterialGraphNode& Node : Asset.Graph.Nodes)
		{
			Rects.push_back({ Node.EditorPosition, Node.EditorPosition + MaterialGraphEditing::EstimateNodeSize(Node) });
		}
		for (size_t A = 0; A < Rects.size(); ++A)
		{
			for (size_t B = A + 1; B < Rects.size(); ++B)
			{
				E_EXPECT_FALSE(Overlaps(Rects[A], Rects[B]));
			}
		}
		++Checked;
	}
	E_EXPECT_TRUE(Checked >= 5u);
}

E_TEST(MaterialGraphEditing_SampleMaterialsRoundTrip)
{
	// 열기(자동 배치) → 저장 형식 → 다시 읽기: 같은 생성 HLSL 해시 + 같은 JSON (예제 8개 모두, 인스턴스 포함)
	uint32 Checked = 0;
	for (const fs::path& Path : SampleMaterials())
	{
		FMaterialAsset Original;
		E_EXPECT_TRUE(Original.LoadFromFile(Path));
		FMaterialAsset Edited = Original;
		if (Edited.IsGraphMaterial() && !Edited.IsInstance())
		{
			MaterialGraphEditing::AutoLayout(Edited.Graph, true);
		}
		const std::string Saved = Edited.ToJsonString();
		FMaterialAsset    Reloaded;
		E_EXPECT_TRUE(Reloaded.FromJsonString(Saved));
		E_EXPECT_TRUE(Reloaded.ToJsonString() == Saved);
		const uint64 Hash = ResolvedHash(Original, Path);
		E_EXPECT_TRUE(Hash != 0);
		E_EXPECT_EQ(ResolvedHash(Reloaded, Path), Hash);
		++Checked;
	}
	E_EXPECT_EQ(Checked, 8u);
}

E_TEST(MaterialGraphEditing_AddConnectRemove)
{
	FMaterialGraph                  Graph;
	std::vector<FMaterialParameter> Parameters;
	FMaterialGraphNode*             Mul = MaterialGraphEditing::AddNode(Graph, Parameters, "Multiply", FVector2(-300.0f, 0.0f));
	E_EXPECT_TRUE(Mul != nullptr && Mul->Id == "multiply1");
	const std::string MulId = Mul->Id;
	const std::string TimeId   = MaterialGraphEditing::AddNode(Graph, Parameters, "Time", FVector2(-600.0f, 0.0f))->Id;
	const std::string ScalarId = MaterialGraphEditing::AddNode(Graph, Parameters, "ScalarParameter", FVector2(-600.0f, 100.0f))->Id;
	E_EXPECT_TRUE(MaterialGraphEditing::AddNode(Graph, Parameters, "NoSuchNode", FVector2(1.0f, 1.0f)) == nullptr);
	// 파라미터 노드는 새 파라미터를 만든다
	E_EXPECT_EQ(Parameters.size(), size_t(1));
	E_EXPECT_TRUE(Parameters[0].Type == EMaterialParameterType::Scalar && Graph.FindNode(ScalarId)->Name == Parameters[0].Name);
	// 두 번째 Multiply는 번호가 오른다
	E_EXPECT_TRUE(MaterialGraphEditing::AddNode(Graph, Parameters, "Multiply", FVector2(1.0f, 1.0f))->Id == "multiply2");

	E_EXPECT_TRUE(MaterialGraphEditing::Connect(Graph, TimeId, 0, MulId, "A"));
	E_EXPECT_TRUE(MaterialGraphEditing::Connect(Graph, ScalarId, 0, MulId, "B"));
	E_EXPECT_TRUE(MaterialGraphEditing::Connect(Graph, MulId, 0, FMaterialGraphCompiler::OutputNodeId, "Roughness"));
	// 같은 핀에 다시 연결하면 바뀐다
	E_EXPECT_TRUE(MaterialGraphEditing::Connect(Graph, ScalarId, 0, MulId, "A"));
	E_EXPECT_TRUE(FindLink(Graph, MulId, "A")->Node == ScalarId);
	E_EXPECT_EQ(Graph.FindNode(MulId)->Inputs.size(), size_t(2));
	// 순환 거부
	E_EXPECT_FALSE(MaterialGraphEditing::Connect(Graph, MulId, 0, MulId, "A"));
	const std::string Mul2 = "multiply2";
	E_EXPECT_TRUE(MaterialGraphEditing::Connect(Graph, MulId, 0, Mul2, "A"));
	E_EXPECT_TRUE(MaterialGraphEditing::WouldCreateCycle(Graph, Mul2, MulId));
	E_EXPECT_FALSE(MaterialGraphEditing::Connect(Graph, Mul2, 0, MulId, "B"));

	FMaterialGraphCompileResult Result = FMaterialGraphCompiler::Compile(Graph, Parameters);
	E_EXPECT_TRUE(Result.bSuccess);

	// 상수 지정 → 연결이 바뀐다, 끊기
	MaterialGraphEditing::SetConstant(Graph, MulId, "A", FVector4(0.25f, 0.0f, 0.0f, 0.0f), 1);
	E_EXPECT_TRUE(FindLink(Graph, MulId, "A") != nullptr && !FindLink(Graph, MulId, "A")->IsLink());
	E_EXPECT_TRUE(MaterialGraphEditing::Disconnect(Graph, MulId, "A"));
	E_EXPECT_TRUE(FindLink(Graph, MulId, "A") == nullptr);

	// 삭제: 그 노드로 가는 연결(출력 포함)도 지운다, 파라미터 정의는 남는다
	MaterialGraphEditing::RemoveNodes(Graph, { MulId });
	E_EXPECT_TRUE(Graph.FindNode(MulId) == nullptr);
	E_EXPECT_TRUE(FindLink(Graph, FMaterialGraphCompiler::OutputNodeId, "Roughness") == nullptr);
	E_EXPECT_TRUE(FindLink(Graph, Mul2, "A") == nullptr);
	E_EXPECT_EQ(Parameters.size(), size_t(1));

	// JSON 왕복 (저장 형식)
	FMaterialAsset Asset;
	Asset.bHasGraph  = true;
	Asset.Graph      = Graph;
	Asset.Parameters = Parameters;
	FMaterialAsset Reloaded = Parse(Asset.ToJsonString());
	E_EXPECT_EQ(Reloaded.Graph.Nodes.size(), Graph.Nodes.size());
	E_EXPECT_NEAR(Reloaded.Graph.FindNode(TimeId)->EditorPosition.X, -600.0f, 1.0e-3f);
}

E_TEST(MaterialGraphEditing_RenameParameterUpdatesNodes)
{
	FMaterialAsset Asset = Parse(ChainGraph);
	E_EXPECT_EQ(MaterialGraphEditing::CountParameterUses(Asset.Graph, "S"), 1u);
	E_EXPECT_TRUE(MaterialGraphEditing::RenameParameter(Asset.Graph, Asset.Parameters, "S", "Strength"));
	E_EXPECT_TRUE(Asset.Graph.FindNode("s")->Name == "Strength");
	E_EXPECT_TRUE(Asset.FindParameter("Strength") != nullptr && Asset.FindParameter("S") == nullptr);
	E_EXPECT_TRUE(FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters).bSuccess);
	// 빈 이름/이미 있는 이름은 거부
	E_EXPECT_FALSE(MaterialGraphEditing::RenameParameter(Asset.Graph, Asset.Parameters, "Strength", ""));
	Asset.Parameters.push_back(Asset.Parameters[0]);
	Asset.Parameters.back().Name = "Other";
	E_EXPECT_FALSE(MaterialGraphEditing::RenameParameter(Asset.Graph, Asset.Parameters, "Strength", "Other"));
}

E_TEST(MaterialGraphEditing_CopyPasteReassignsIds)
{
	FMaterialAsset Asset = Parse(ChainGraph);
	MaterialGraphEditing::AutoLayout(Asset.Graph, false);
	// mul + s 복사: mul의 입력 a/t는 밖이라 끊기고, s 파라미터 정의가 함께 실린다
	const std::string Text = MaterialGraphEditing::CopyNodes(Asset.Graph, Asset.Parameters, { "mul", "add", "s" });
	E_EXPECT_TRUE(MaterialGraphEditing::IsClipboardText(Text));
	E_EXPECT_FALSE(MaterialGraphEditing::IsClipboardText("{ \"Name\": 1 }"));

	// 같은 그래프에 붙이기: 새 Id, 내부 연결은 새 Id로
	const size_t                   Before = Asset.Graph.Nodes.size();
	const std::vector<std::string> NewIds = MaterialGraphEditing::PasteNodes(Asset.Graph, Asset.Parameters, Text, FVector2(1000.0f, 2000.0f));
	E_EXPECT_EQ(NewIds.size(), size_t(3));
	E_EXPECT_EQ(Asset.Graph.Nodes.size(), Before + 3);
	for (const std::string& Id : NewIds)
	{
		E_EXPECT_TRUE(Id != "mul" && Id != "add" && Id != "s");
	}
	const FMaterialGraphNode* PastedAdd = Asset.Graph.FindNode(NewIds[2]); // 복사 순서 = 그래프 순서 (mul, s, add)
	E_EXPECT_TRUE(PastedAdd != nullptr && PastedAdd->Type == "Add");
	if (PastedAdd != nullptr)
	{
		E_EXPECT_TRUE(PastedAdd->FindInput("A")->Node == NewIds[0]); // mul → 새 mul
		E_EXPECT_TRUE(PastedAdd->FindInput("B")->Node == NewIds[1]); // s → 새 s
	}
	const FMaterialGraphNode* PastedMul = Asset.Graph.FindNode(NewIds[0]);
	E_EXPECT_TRUE(PastedMul != nullptr && PastedMul->Inputs.empty()); // 밖(a, t)으로 가는 연결은 끊김
	// 묶음 왼쪽 위 = Anchor
	float MinX = 1.0e9f;
	float MinY = 1.0e9f;
	for (const std::string& Id : NewIds)
	{
		MinX = std::min(MinX, Asset.Graph.FindNode(Id)->EditorPosition.X);
		MinY = std::min(MinY, Asset.Graph.FindNode(Id)->EditorPosition.Y);
	}
	E_EXPECT_NEAR(MinX, 1000.0f, 1.0e-2f);
	E_EXPECT_NEAR(MinY, 2000.0f, 1.0e-2f);
	E_EXPECT_EQ(Asset.Parameters.size(), size_t(1)); // 같은 이름 파라미터는 다시 넣지 않는다

	// 빈 그래프에 붙이기: 파라미터 정의도 들어와 그대로 컴파일된다
	FMaterialGraph                  Empty;
	std::vector<FMaterialParameter> EmptyParameters;
	const std::vector<std::string>  Pasted = MaterialGraphEditing::PasteNodes(Empty, EmptyParameters, Text, FVector2(5.0f, 5.0f));
	E_EXPECT_EQ(Pasted.size(), size_t(3));
	E_EXPECT_TRUE(Pasted[0] == "mul" && Pasted[1] == "s"); // 겹치지 않으면 원래 Id
	E_EXPECT_EQ(EmptyParameters.size(), size_t(1));
	E_EXPECT_TRUE(MaterialGraphEditing::Connect(Empty, Pasted[2], 0, FMaterialGraphCompiler::OutputNodeId, "Roughness"));
	E_EXPECT_FALSE(FMaterialGraphCompiler::Compile(Empty, EmptyParameters).bSuccess); // mul의 A/B 필수 입력이 끊김
	MaterialGraphEditing::SetConstant(Empty, Pasted[0], "A", FVector4(1.0f, 0.0f, 0.0f, 0.0f), 1);
	MaterialGraphEditing::SetConstant(Empty, Pasted[0], "B", FVector4(2.0f, 0.0f, 0.0f, 0.0f), 1);
	E_EXPECT_TRUE(FMaterialGraphCompiler::Compile(Empty, EmptyParameters).bSuccess);
}

E_TEST(MaterialGraphEditing_ConvertToGraph)
{
	// 고정 PBR → 그래프: 모든 슬롯 + 팩터가 노드/파라미터로, 같은 출력 연결
	FMaterialAsset Fixed;
	Fixed.Name                         = "Fixed";
	Fixed.Constants.BaseColorFactor    = FVector4(0.5f, 0.6f, 0.7f, 0.8f);
	Fixed.Constants.Metallic           = 0.3f;
	Fixed.Constants.Roughness          = 0.6f;
	Fixed.Constants.NormalScale        = 0.5f;
	Fixed.Constants.OcclusionStrength  = 0.75f;
	Fixed.Constants.EmissiveFactor     = FVector3(1.0f, 0.5f, 0.0f);
	Fixed.BlendMode                    = EMaterialBlendMode::Masked;
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		Fixed.TexturePaths[Slot] = "Tex" + std::to_string(Slot) + ".png";
	}
	FMaterialAsset Graph = Fixed;
	MaterialGraphEditing::ConvertToGraph(Graph, false);
	E_EXPECT_TRUE(Graph.IsGraphMaterial());
	E_EXPECT_FALSE(MaterialGraphEditing::HasMissingPositions(Graph.Graph));
	const FMaterialGraphCompileResult Result = FMaterialGraphCompiler::Compile(Graph.Graph, Graph.Parameters);
	E_EXPECT_TRUE(Result.bSuccess);
	if (Result.bSuccess)
	{
		E_EXPECT_EQ(Result.Shader->Layout.TextureCount, 5u);
		// 텍스처 용도 = 슬롯 용도
		const FMaterialParameterSlot* Normal = Result.Shader->Layout.Find("NormalTexture");
		E_EXPECT_TRUE(Normal != nullptr && Normal->Usage == ETextureUsage::Normal);
		E_EXPECT_TRUE(Result.Shader->Hlsl.find("lerp(") != std::string::npos); // AO = lerp(1, AO, 강도)
	}
	for (const char* Output : { "BaseColor", "Metallic", "Roughness", "Normal", "AmbientOcclusion", "Emissive", "Opacity", "OpacityMask" })
	{
		E_EXPECT_TRUE(FindLink(Graph.Graph, FMaterialGraphCompiler::OutputNodeId, Output) != nullptr);
	}
	const FMaterialParameter* Base = Graph.FindParameter("BaseColor");
	E_EXPECT_TRUE(Base != nullptr && Base->Type == EMaterialParameterType::Vector);
	if (Base != nullptr)
	{
		E_EXPECT_NEAR(Base->Value.W, 0.8f, 1.0e-5f);
	}
	const FMaterialParameter* Texture = Graph.FindParameter("BaseColorTexture");
	E_EXPECT_TRUE(Texture != nullptr && Texture->Texture == "Tex0.png" && Texture->Usage == ETextureUsage::Color);
	E_EXPECT_TRUE(Graph.TexturePaths[0].empty()); // 고정 슬롯 경로는 파라미터로 옮김
	E_EXPECT_NEAR(Graph.Constants.AlphaCutoff, Fixed.Constants.AlphaCutoff, 1.0e-6f);

	// 불투명 + 텍스처 없음: Opacity 연결 없음, 팩터만
	FMaterialAsset Plain;
	MaterialGraphEditing::ConvertToGraph(Plain, false);
	E_EXPECT_TRUE(FMaterialGraphCompiler::Compile(Plain.Graph, Plain.Parameters).bSuccess);
	E_EXPECT_TRUE(FindLink(Plain.Graph, FMaterialGraphCompiler::OutputNodeId, "Opacity") == nullptr);
	E_EXPECT_TRUE(Plain.FindParameter("BaseColorTexture") == nullptr);

	// 새 그래프 머티리얼: 베이스 텍스처 파라미터(빈 경로 = 기본 흰색)가 있다
	const FMaterialAsset New = MaterialGraphEditing::MakeDefaultGraphMaterial("NewMaterialGraph");
	E_EXPECT_TRUE(New.Name == "NewMaterialGraph" && New.IsGraphMaterial());
	E_EXPECT_TRUE(New.FindParameter("BaseColorTexture") != nullptr);
	const FMaterialGraphCompileResult NewResult = FMaterialGraphCompiler::Compile(New.Graph, New.Parameters);
	E_EXPECT_TRUE(NewResult.bSuccess);
	// 저장 → 다시 읽어도 같은 셰이더
	const FMaterialAsset Reloaded = Parse(New.ToJsonString());
	E_EXPECT_EQ(FMaterialGraphCompiler::Compile(Reloaded.Graph, Reloaded.Parameters).Shader->Hash, NewResult.Shader->Hash);
}
