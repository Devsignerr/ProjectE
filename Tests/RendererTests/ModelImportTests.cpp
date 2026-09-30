#include "Core/Testing/TestFramework.h"
#include "Renderer/GltfLoader.h"
#include "Renderer/ModelImportSettings.h"

namespace
{
	constexpr float Tol = 1.0e-4f;

	// 루트(Hips) → 자식(Spine) 두 노드, 스킨 1개, 클립 1개, 머티리얼 1개짜리 모델
	FModelData MakeModel()
	{
		FModelData Model;
		Model.Nodes.resize(2);
		Model.Nodes[0].Name     = "Hips";
		Model.Nodes[0].Children = { 1 };
		Model.Nodes[0].Skin     = 0;
		Model.Nodes[1].Name     = "Spine";
		Model.Nodes[1].Parent   = 0;
		Model.RootNodes         = { 0 };
		Model.Skins.resize(1);
		Model.Skins[0].Joints = { 0, 1 };
		Model.Materials.resize(1);
		Model.Images.resize(1);
		FModelMesh Mesh;
		Mesh.Material = 0;
		Mesh.Data.Vertices.resize(3);
		Mesh.Data.Vertices[1].Position = FVector3(0, 100, 0);
		Mesh.Data.Vertices[2].Position = FVector3(0, 0, 100);
		Mesh.Data.Indices = { 0, 1, 2 };
		Mesh.SkinVertices.resize(3);
		Model.Meshes.push_back(Mesh);
		FAnimationClip Clip;
		Clip.Name = "Idle";
		FAnimationChannel Channel;
		Channel.Node  = 1;
		Channel.Times = { 0.0f };
		Channel.Values = { FVector4(0, 0, 1, 0) };
		Clip.Channels.push_back(Channel);
		Model.Animations.push_back(Clip);
		return Model;
	}
} // namespace

E_TEST(ModelImport_SettingsJsonRoundTrip)
{
	FModelImportSettings Settings;
	Settings.Scale             = 2.5f;
	Settings.YawDegrees        = 90.0f;
	Settings.bImportMaterials  = false;
	Settings.bRecomputeNormals = true;
	Settings.AnimationSources  = { "Anims/Run.fbx", "Anims/Walk.fbx" };
	E_EXPECT_FALSE(Settings.IsDefault());

	FModelImportSettings Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Settings.ToJsonString()));
	E_EXPECT_NEAR(Loaded.Scale, 2.5f, Tol);
	E_EXPECT_NEAR(Loaded.YawDegrees, 90.0f, Tol);
	E_EXPECT_FALSE(Loaded.bImportMaterials);
	E_EXPECT_TRUE(Loaded.bRecomputeNormals);
	E_EXPECT_EQ(Loaded.AnimationSources.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(FModelImportSettings{}.IsDefault());
	E_EXPECT_TRUE(FModelImportSettings::GetSidecarPath(L"C:/A/Fox.fbx") == std::filesystem::path(L"C:/A/Fox.fbx.eimport"));
}

E_TEST(ModelImport_ApplyScaleYawAddsRootAndKeepsData)
{
	FModelData           Model = MakeModel();
	FModelImportSettings Settings;
	Settings.Scale      = 2.0f;
	Settings.YawDegrees = 90.0f;
	Settings.Apply(Model);

	// 새 루트 아래로 옛 루트, 정점/애니메이션은 그대로
	E_EXPECT_EQ(Model.Nodes.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Model.RootNodes.size(), static_cast<size_t>(1));
	const FModelNode& Root = Model.Nodes[static_cast<size_t>(Model.RootNodes[0])];
	E_EXPECT_TRUE(Root.Name == "ImportRoot");
	E_EXPECT_EQUALS(Root.Scale, FVector3(2.0f), Tol);
	E_EXPECT_EQUALS(Root.Rotation.RotateVector(FVector3::ForwardVector), FVector3::RightVector, 1.0e-3f);
	E_EXPECT_EQ(Model.Nodes[0].Parent, Model.RootNodes[0]);
	E_EXPECT_EQUALS(Model.Meshes[0].Data.Vertices[1].Position, FVector3(0, 100, 0), Tol);
	E_EXPECT_EQ(Model.Animations[0].Channels[0].Node, 1);

	// 기본 설정이면 구조가 바뀌지 않는다
	FModelData Untouched = MakeModel();
	FModelImportSettings{}.Apply(Untouched);
	E_EXPECT_EQ(Untouched.Nodes.size(), static_cast<size_t>(2));
}

E_TEST(ModelImport_ApplyDropsParts)
{
	FModelData           Model = MakeModel();
	FModelImportSettings Settings;
	Settings.bImportMaterials  = false;
	Settings.bImportSkin       = false;
	Settings.bImportAnimations = false;
	Settings.bRecomputeNormals = true;
	Settings.Apply(Model);
	E_EXPECT_TRUE(Model.Materials.empty() && Model.Images.empty());
	E_EXPECT_EQ(Model.Meshes[0].Material, -1);
	E_EXPECT_TRUE(Model.Skins.empty() && Model.Meshes[0].SkinVertices.empty());
	E_EXPECT_EQ(Model.Nodes[0].Skin, -1);
	E_EXPECT_TRUE(Model.Animations.empty());
	// 다시 계산한 법선: 삼각형 (0,0,0)-(0,100,0)-(0,0,100)의 면 법선은 +X 또는 -X
	E_EXPECT_NEAR(std::abs(Model.Meshes[0].Data.Vertices[0].Normal.X), 1.0f, 1.0e-3f);
}

E_TEST(ModelImport_MergeAnimationsByNodeName)
{
	FModelData Target = MakeModel();
	FModelData Source;
	Source.Nodes.resize(3);
	Source.Nodes[0].Name = "Other";
	Source.Nodes[1].Name = "Spine";
	Source.Nodes[2].Name = "Hips";
	FAnimationClip Run;
	Run.Name     = "Take 001";
	Run.Duration = 1.0f;
	for (const int32 Node : { 0, 1, 2 })
	{
		FAnimationChannel Channel;
		Channel.Node   = Node;
		Channel.Times  = { 0.0f, 1.0f };
		Channel.Values = { FVector4(), FVector4(1, 0, 0, 0) };
		Run.Channels.push_back(Channel);
	}
	Source.Animations.push_back(Run);

	E_EXPECT_EQ(FModelImportSettings::MergeAnimations(Target, Source, "Run"), 1u);
	E_EXPECT_EQ(Target.Animations.size(), static_cast<size_t>(2));
	const FAnimationClip& Merged = Target.Animations[1];
	E_EXPECT_TRUE(Merged.Name == "Run");
	// "Other"는 대상에 없어 빠지고, Spine(1) / Hips(0)으로 다시 매핑
	E_EXPECT_EQ(Merged.Channels.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Merged.Channels[0].Node, 1);
	E_EXPECT_EQ(Merged.Channels[1].Node, 0);
}
