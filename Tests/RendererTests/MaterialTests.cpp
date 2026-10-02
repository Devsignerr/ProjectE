#include "Core/Testing/TestFramework.h"
#include "Renderer/InstanceBatching.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/PrimitiveShapes.h"

#include <map>
#include <string>
#include <vector>

// Phase 36: 머티리얼 블렌드 모드/양면/인스턴스 (순수 로직 — JSON, 부모 체인 해석, 묶음 키, 반투명 정렬)

namespace
{
	constexpr float Tol = 1.0e-5f;

	// 메모리 .emat 저장소 (경로 키 = MakePathKey)
	struct FMemoryMaterials
	{
		std::map<std::wstring, std::string> Files;

		void Add(const std::filesystem::path& Path, const std::string& Json) { Files[FMaterialAsset::MakePathKey(Path)] = Json; }

		FMaterialAsset::FLoader MakeLoader() const
		{
			return [this](const std::filesystem::path& Path, FMaterialAsset& Out) {
				const auto Found = Files.find(FMaterialAsset::MakePathKey(Path));
				return Found != Files.end() && Out.FromJsonString(Found->second);
			};
		}
	};

	FMaterialAsset Parse(const std::string& Json)
	{
		FMaterialAsset Asset;
		E_EXPECT_TRUE(Asset.FromJsonString(Json));
		return Asset;
	}

	FMeshInstance MakeInstance(uint32 Mesh, uint32 Material, bool bSkinned, EMaterialBlendMode Mode, bool bTwoSided = false)
	{
		FMeshInstance Instance;
		Instance.MeshHandle.Index     = Mesh;
		Instance.MaterialHandle.Index = Material;
		Instance.bSkinned             = bSkinned;
		Instance.BlendMode            = Mode;
		Instance.bTwoSided            = bTwoSided;
		return Instance;
	}
} // namespace

E_TEST(Material_JsonRoundTripRenderState)
{
	FMaterialAsset Asset;
	Asset.Name                  = "Fence";
	Asset.BlendMode             = EMaterialBlendMode::Masked;
	Asset.Constants.AlphaCutoff = 0.3f;
	Asset.bTwoSided             = true;
	Asset.TexturePaths[MaterialSlot_BaseColor] = "Fence.png";

	const FMaterialAsset Loaded = Parse(Asset.ToJsonString());
	E_EXPECT_TRUE(Loaded.BlendMode == EMaterialBlendMode::Masked);
	E_EXPECT_NEAR(Loaded.Constants.AlphaCutoff, 0.3f, Tol);
	E_EXPECT_TRUE(Loaded.bTwoSided);
	E_EXPECT_FALSE(Loaded.IsInstance());
	E_EXPECT_EQ(Loaded.OverrideMask, static_cast<uint32>(FMaterialAsset::Field_All));
	E_EXPECT_TRUE(Loaded.TexturePaths[MaterialSlot_BaseColor] == "Fence.png");

	for (const EMaterialBlendMode Mode : { EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent, EMaterialBlendMode::Additive })
	{
		E_EXPECT_TRUE(FMaterialAsset::ParseBlendMode(FMaterialAsset::GetBlendModeName(Mode)) == Mode);
	}
	E_EXPECT_TRUE(FMaterialAsset::ParseBlendMode("Unknown", EMaterialBlendMode::Additive) == EMaterialBlendMode::Additive);
}

E_TEST(Material_LegacyJsonUsesDefaults)
{
	// 블렌드 키가 없는 기존 .emat: 불투명, 컷오프 0.5, 한 면
	const FMaterialAsset Asset = Parse(R"({ "Name": "Old", "BaseColorFactor": [1, 0, 0, 1], "Roughness": 0.25 })");
	E_EXPECT_TRUE(Asset.BlendMode == EMaterialBlendMode::Opaque);
	E_EXPECT_NEAR(Asset.Constants.AlphaCutoff, 0.5f, Tol);
	E_EXPECT_FALSE(Asset.bTwoSided);
	E_EXPECT_NEAR(Asset.Constants.Roughness, 0.25f, Tol);
}

E_TEST(Material_InstanceWritesOnlyOverrides)
{
	const FMaterialAsset Instance = Parse(R"({ "Name": "Red", "Parent": "Base.emat", "BaseColorFactor": [1, 0, 0, 1], "TwoSided": true })");
	E_EXPECT_TRUE(Instance.IsInstance());
	E_EXPECT_EQ(Instance.OverrideMask, static_cast<uint32>(FMaterialAsset::Field_BaseColorFactor | FMaterialAsset::Field_TwoSided));
	E_EXPECT_FALSE(Instance.Overrides(FMaterialAsset::Field_Roughness));

	const std::string Json = Instance.ToJsonString();
	E_EXPECT_TRUE(Json.find("\"Parent\"") != std::string::npos);
	E_EXPECT_TRUE(Json.find("\"BaseColorFactor\"") != std::string::npos);
	E_EXPECT_TRUE(Json.find("\"TwoSided\"") != std::string::npos);
	E_EXPECT_TRUE(Json.find("\"Roughness\"") == std::string::npos);
	E_EXPECT_TRUE(Json.find("\"BaseColorTexture\"") == std::string::npos);
	E_EXPECT_EQ(Parse(Json).OverrideMask, Instance.OverrideMask);
}

E_TEST(Material_ResolveParentChain)
{
	// 조부모(Masters/Leaf.emat) ← 부모(Masters/LeafDark.emat) ← 자식(Props/Bush/LeafBush.emat)
	FMemoryMaterials Files;
	Files.Add(L"C:/Content/Masters/Leaf.emat", R"({ "Name": "Leaf", "BlendMode": "Masked", "AlphaCutoff": 0.4, "TwoSided": true,
	    "Roughness": 0.7, "Metallic": 0.1, "BaseColorTexture": "../Textures/Leaf.png", "NormalTexture": "LeafNormal.png" })");
	Files.Add(L"C:/Content/Masters/LeafDark.emat", R"({ "Name": "LeafDark", "Parent": "Leaf.emat", "BaseColorFactor": [0.2, 0.3, 0.1, 1], "Roughness": 0.9 })");
	const std::filesystem::path ChildPath = L"C:/Content/Props/Bush/LeafBush.emat";
	const FMaterialAsset        Child     = Parse(R"({ "Name": "LeafBush", "Parent": "../../Masters/LeafDark.emat", "AlphaCutoff": 0.6 })");

	FMaterialAsset                     Resolved;
	std::vector<std::filesystem::path> Chain;
	E_EXPECT_TRUE(FMaterialAsset::Resolve(Child, ChildPath, Files.MakeLoader(), Resolved, &Chain));
	E_EXPECT_EQ(Chain.size(), size_t(2));
	E_EXPECT_TRUE(FMaterialAsset::MakePathKey(Chain[0]) == FMaterialAsset::MakePathKey(L"C:/Content/Masters/LeafDark.emat"));
	E_EXPECT_TRUE(FMaterialAsset::MakePathKey(Chain[1]) == FMaterialAsset::MakePathKey(L"C:/Content/Masters/Leaf.emat"));

	E_EXPECT_TRUE(Resolved.Name == "LeafBush");       // 이름은 상속하지 않는다
	E_EXPECT_FALSE(Resolved.IsInstance());
	E_EXPECT_TRUE(Resolved.BlendMode == EMaterialBlendMode::Masked); // 조부모
	E_EXPECT_TRUE(Resolved.bTwoSided);
	E_EXPECT_NEAR(Resolved.Constants.AlphaCutoff, 0.6f, Tol);        // 자식
	E_EXPECT_NEAR(Resolved.Constants.Roughness, 0.9f, Tol);          // 부모
	E_EXPECT_NEAR(Resolved.Constants.Metallic, 0.1f, Tol);           // 조부모
	E_EXPECT_NEAR(Resolved.Constants.BaseColorFactor.Y, 0.3f, Tol);  // 부모
	// 조상 폴더 기준 텍스처 경로 → 자식 폴더 기준
	E_EXPECT_TRUE(Resolved.TexturePaths[MaterialSlot_BaseColor] == "../../Textures/Leaf.png");
	E_EXPECT_TRUE(Resolved.TexturePaths[MaterialSlot_Normal] == "../../Masters/LeafNormal.png");
	E_EXPECT_TRUE(Resolved.TexturePaths[MaterialSlot_Emissive].empty());
}

E_TEST(Material_ResolveDetectsCycleAndMissingParent)
{
	FMemoryMaterials Files;
	Files.Add(L"C:/M/A.emat", R"({ "Name": "A", "Parent": "B.emat", "Roughness": 0.2 })");
	Files.Add(L"C:/M/B.emat", R"({ "Name": "B", "Parent": "A.emat", "Metallic": 0.8 })");

	// 순환: A → B → A. 실패하지만 읽은 데까지의 값은 남는다 (나머지는 기본값)
	{
		FMaterialAsset Resolved;
		std::string    Error;
		const FMaterialAsset A = Parse(R"({ "Name": "A", "Parent": "B.emat", "Roughness": 0.2 })");
		E_EXPECT_FALSE(FMaterialAsset::Resolve(A, L"C:/M/A.emat", Files.MakeLoader(), Resolved, nullptr, &Error));
		E_EXPECT_TRUE(Error.find("순환") != std::string::npos);
		E_EXPECT_NEAR(Resolved.Constants.Roughness, 0.2f, Tol);
		E_EXPECT_NEAR(Resolved.Constants.Metallic, 0.8f, Tol);
		E_EXPECT_TRUE(Resolved.BlendMode == EMaterialBlendMode::Opaque);
	}
	// 자기 자신이 부모 (대소문자만 다른 경로도 같은 파일)
	{
		FMaterialAsset       Resolved;
		const FMaterialAsset Self = Parse(R"({ "Name": "S", "Parent": "./s.EMAT" })");
		E_EXPECT_FALSE(FMaterialAsset::Resolve(Self, L"C:/M/S.emat", Files.MakeLoader(), Resolved));
	}
	// 부모 파일 없음: 자식이 덮어쓴 값 + 기본값
	{
		FMaterialAsset       Resolved;
		std::string          Error;
		const FMaterialAsset Orphan = Parse(R"({ "Name": "O", "Parent": "Missing.emat", "BlendMode": "Additive" })");
		E_EXPECT_FALSE(FMaterialAsset::Resolve(Orphan, L"C:/M/O.emat", Files.MakeLoader(), Resolved, nullptr, &Error));
		E_EXPECT_TRUE(Error.find("Missing.emat") != std::string::npos);
		E_EXPECT_TRUE(Resolved.BlendMode == EMaterialBlendMode::Additive);
		E_EXPECT_NEAR(Resolved.Constants.Roughness, FMaterialConstants{}.Roughness, Tol);
	}
	// 일반 머티리얼은 로더 없이 그대로
	{
		FMaterialAsset       Resolved;
		const FMaterialAsset Plain = Parse(R"({ "Name": "P", "Roughness": 0.33, "BaseColorTexture": "T.png" })");
		E_EXPECT_TRUE(FMaterialAsset::Resolve(Plain, L"C:/M/P.emat", nullptr, Resolved));
		E_EXPECT_NEAR(Resolved.Constants.Roughness, 0.33f, Tol);
		E_EXPECT_TRUE(Resolved.TexturePaths[MaterialSlot_BaseColor] == "T.png");
	}
}

E_TEST(Material_PipelineVariantKeysOrderStaticBeforeSkinned)
{
	// 메인 묶음 키: 변형 비트(Masked 1 | 양면 2 | 스킨 4)가 최상위 → 정적 변형이 모두 스킨보다 앞
	const uint64 StaticMaskedTwoSided = MakeMainBatchKey(MakeInstance(0xFFFFFF, 0xFFFFFF, false, EMaterialBlendMode::Masked, true));
	const uint64 SkinnedOpaque        = MakeMainBatchKey(MakeInstance(0, 0, true, EMaterialBlendMode::Opaque));
	E_EXPECT_TRUE(StaticMaskedTwoSided < SkinnedOpaque);
	E_EXPECT_EQ(MakeInstance(1, 1, false, EMaterialBlendMode::Masked, true).GetPipelineVariant(), 3u);
	E_EXPECT_EQ(MakeInstance(1, 1, true, EMaterialBlendMode::Opaque).GetPipelineVariant(), MaterialRender::VariantSkinned);
	// 반투명 패스: bit0 = 가산
	E_EXPECT_EQ(MakeInstance(1, 1, false, EMaterialBlendMode::Translucent).GetPipelineVariant(), 0u);
	E_EXPECT_EQ(MakeInstance(1, 1, false, EMaterialBlendMode::Additive, true).GetPipelineVariant(), 3u);
	// 같은 메시·머티리얼이라도 변형이 다르면 다른 묶음
	E_EXPECT_TRUE(MakeMainBatchKey(MakeInstance(2, 3, false, EMaterialBlendMode::Opaque)) !=
	              MakeMainBatchKey(MakeInstance(2, 3, false, EMaterialBlendMode::Opaque, true)));
}

E_TEST(Material_DepthKeyAndShadowCasting)
{
	// 그림자 키: 불투명은 머티리얼 무관(한 묶음), Masked는 머티리얼별 + 불투명 뒤
	E_EXPECT_EQ(MakeDepthBatchKey(MakeInstance(5, 1, false, EMaterialBlendMode::Opaque)),
	            MakeDepthBatchKey(MakeInstance(5, 9, false, EMaterialBlendMode::Opaque, true)));
	E_EXPECT_TRUE(MakeDepthBatchKey(MakeInstance(5, 1, false, EMaterialBlendMode::Masked)) !=
	              MakeDepthBatchKey(MakeInstance(5, 9, false, EMaterialBlendMode::Masked)));
	E_EXPECT_TRUE(MakeDepthBatchKey(MakeInstance(0, 0, false, EMaterialBlendMode::Masked)) >
	              MakeDepthBatchKey(MakeInstance(0xFFFFFF, 1, true, EMaterialBlendMode::Opaque)));
	E_EXPECT_EQ(GetDepthVariant(MakeInstance(5, 1, true, EMaterialBlendMode::Masked)), DepthVariantSkinned | DepthVariantMasked);

	// 반투명/가산은 그림자를 드리우지 않는다
	E_EXPECT_TRUE(MakeInstance(1, 1, false, EMaterialBlendMode::Masked).CastsShadow());
	E_EXPECT_FALSE(MakeInstance(1, 1, false, EMaterialBlendMode::Translucent).CastsShadow());
	E_EXPECT_FALSE(MakeInstance(1, 1, false, EMaterialBlendMode::Additive).CastsShadow());
	FMeshInstance NoShadow = MakeInstance(1, 1, false, EMaterialBlendMode::Opaque);
	NoShadow.bCastShadow   = false;
	E_EXPECT_FALSE(NoShadow.CastsShadow());
}

E_TEST(Material_CopyMaterialStateFromMaterial)
{
	FMaterial Material;
	Material.BlendMode = EMaterialBlendMode::Translucent;
	Material.bTwoSided = true;
	FMeshInstance Instance;
	Instance.Material = &Material;
	Instance.CopyMaterialState();
	E_EXPECT_TRUE(Instance.IsTranslucent());
	E_EXPECT_TRUE(Instance.bTwoSided);
	Instance.Material = nullptr;
	Instance.CopyMaterialState();
	E_EXPECT_TRUE(Instance.BlendMode == EMaterialBlendMode::Opaque);
	E_EXPECT_FALSE(Instance.bTwoSided);
}

E_TEST(Material_TranslucentBackToFront)
{
	// 먼 것(깊이 큰 것)부터. 정렬 결과에서 바로 이웃한 같은 키만 묶고, 사이에 다른 키가 끼면 따로 그린다
	const uint64 Glass = InstanceBatching::MakeKey(0, 1, 1, 0);
	const uint64 Smoke = InstanceBatching::MakeKey(1, 2, 1, 0);
	std::vector<FInstanceSortItem> Items = {
		{ Glass, 100.0f, 0 }, { Smoke, 900.0f, 1 }, { Glass, 500.0f, 2 }, { Glass, 400.0f, 3 }, { Smoke, 50.0f, 4 },
	};
	std::vector<uint32>         Indices;
	std::vector<FInstanceBatch> Batches;
	InstanceBatching::BuildBackToFront(Items, Indices, Batches);

	const std::vector<uint32> Expected = { 1, 2, 3, 0, 4 };
	E_EXPECT_TRUE(Indices == Expected);
	E_EXPECT_EQ(Batches.size(), size_t(3)); // Smoke(900) | Glass(500, 400, 100) | Smoke(50)
	E_EXPECT_EQ(Batches[1].First, 1u);
	E_EXPECT_EQ(Batches[1].Count, 3u);
	E_EXPECT_EQ(Batches[2].Instance, 4u);
}

E_TEST(Material_PlanePrimitiveIsSingleSidedCw)
{
	// 양면 머티리얼 데모용 사각형 한 장: 법선 +Z, 앞면 CW 규약 (Cross(P1 - P0, P2 - P0)·N > 0), 1m
	const FMeshData Plane = FPrimitiveShapes::MakePlane(100.0f);
	E_EXPECT_EQ(Plane.Vertices.size(), size_t(4));
	E_EXPECT_EQ(Plane.Indices.size(), size_t(6));
	for (size_t Tri = 0; Tri < Plane.Indices.size(); Tri += 3)
	{
		const FVector3& P0 = Plane.Vertices[Plane.Indices[Tri]].Position;
		const FVector3& P1 = Plane.Vertices[Plane.Indices[Tri + 1]].Position;
		const FVector3& P2 = Plane.Vertices[Plane.Indices[Tri + 2]].Position;
		E_EXPECT_TRUE(FVector3::Dot(FVector3::Cross(P1 - P0, P2 - P0), FVector3::UpVector) > 0.0f);
	}
	E_EXPECT_NEAR(Plane.Vertices[0].Position.Z, 0.0f, Tol);
	E_EXPECT_NEAR(FMath::Abs(Plane.Vertices[0].Position.X), 50.0f, Tol);
}
