#include "Core/Testing/TestFramework.h"
#include "Renderer/InstanceBatching.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/SkinnedMeshPalette.h"

#include <vector>

namespace
{
	FMeshInstance MakeInstance(uint32 Mesh, uint32 Material, uint32 Lod, bool bSkinned, uint32 BoneOffset = 0)
	{
		FMeshInstance Instance;
		Instance.MeshHandle.Index     = Mesh;
		Instance.MaterialHandle.Index = Material;
		Instance.Lod                  = Lod;
		Instance.bSkinned             = bSkinned;
		Instance.BoneOffset           = BoneOffset;
		return Instance;
	}

	// 조인트마다 다른 회전/이동/스케일 (테스트용 포즈)
	FMatrix4x4 MakePose(int32 Joint, float Phase)
	{
		const FQuat Rotation = FQuat::FromEuler(20.0f * static_cast<float>(Joint) + Phase, 35.0f * Phase, -15.0f * static_cast<float>(Joint));
		return FMatrix4x4::MakeTransform(FVector3(30.0f * static_cast<float>(Joint), 10.0f * Phase, 5.0f * static_cast<float>(Joint)), Rotation,
		                                 FVector3(1.0f + 0.25f * static_cast<float>(Joint)));
	}
} // namespace

E_TEST(SkinInstancing_SameMeshAndMaterialMergeAfterStatic)
{
	// 메인 패스 키: 스킨 = 종류 1 (정적 뒤), 같은 메시·머티리얼 스킨 인스턴스는 한 묶음 (팔레트는 인스턴스별 BoneOffset)
	const uint64 SkinKey   = InstanceBatching::MakeKey(1, 3, 5, 0);
	const uint64 OtherSkin = InstanceBatching::MakeKey(1, 4, 5, 0);
	const uint64 StaticKey = InstanceBatching::MakeKey(0, 0xFFFFFF, 0xFFFFFF, 15);
	std::vector<FInstanceSortItem> Items = {
		{ SkinKey, 3.0f, 0 }, { StaticKey, 1.0f, 1 }, { SkinKey, 1.0f, 2 }, { OtherSkin, 0.0f, 3 }, { SkinKey, 2.0f, 4 },
	};
	std::vector<uint32>         Indices;
	std::vector<FInstanceBatch> Batches;
	InstanceBatching::Build(Items, Indices, Batches);

	E_EXPECT_EQ(Batches.size(), size_t(3));
	E_EXPECT_EQ(Batches[0].Instance, 1u); // 정적 먼저
	E_EXPECT_EQ(Batches[1].Count, 3u);    // 스킨 3개 한 드로우
	E_EXPECT_EQ(Batches[1].First, 1u);
	E_EXPECT_EQ(Batches[2].Count, 1u);
	const std::vector<uint32> Expected = { 1, 2, 4, 0, 3 };
	E_EXPECT_TRUE(Indices == Expected);
}

E_TEST(SkinInstancing_DepthKeyIgnoresMaterialAndSkinLod)
{
	// 그림자 키: 머티리얼 무관, 정적은 LOD별, 스킨은 LOD 없음(같은 메시면 같은 키), 정적과 스킨은 다른 키
	E_EXPECT_EQ(MakeDepthBatchKey(MakeInstance(5, 1, 0, false)), MakeDepthBatchKey(MakeInstance(5, 9, 0, false)));
	E_EXPECT_TRUE(MakeDepthBatchKey(MakeInstance(5, 1, 0, false)) != MakeDepthBatchKey(MakeInstance(5, 1, 2, false)));
	E_EXPECT_EQ(MakeDepthBatchKey(MakeInstance(5, 1, 0, true, 0)), MakeDepthBatchKey(MakeInstance(5, 2, 3, true, 48)));
	E_EXPECT_TRUE(MakeDepthBatchKey(MakeInstance(5, 1, 0, true)) != MakeDepthBatchKey(MakeInstance(5, 1, 0, false)));
	E_EXPECT_TRUE(MakeDepthBatchKey(MakeInstance(5, 1, 0, true)) > MakeDepthBatchKey(MakeInstance(0xFFFFFF, 1, 15, false)));
}

E_TEST(SkinInstancing_JointRadiiAreCornerDistances)
{
	const FBox              Bounds(FVector3(-1.0f, -2.0f, -2.0f), FVector3(1.0f, 2.0f, 2.0f));
	std::vector<FMatrix4x4> InverseBind = { FMatrix4x4::Identity, FMatrix4x4::MakeTranslation(FVector3(-1.0f, 0.0f, 0.0f)) };
	std::vector<float>      Radii;
	FSkinnedMeshPalette::ComputeJointRadii(Bounds, InverseBind, Radii);
	E_EXPECT_EQ(Radii.size(), size_t(2));
	E_EXPECT_NEAR(Radii[0], 3.0f, 1.0e-4f);                 // (1, 2, 2)
	E_EXPECT_NEAR(Radii[1], FMath::Sqrt(4.0f + 8.0f), 1.0e-4f); // (-1-1, 2, 2) → (-2, 2, 2)
}

E_TEST(SkinInstancing_ConservativeBoundsContainPaletteBounds)
{
	// 가시성 판정용 경계(조인트 위치 + 반경)는 팔레트로 계산한 정확한 경계를 항상 포함해야 한다 (화면 밖 그림자 누락 방지)
	const FBox LocalBounds(FVector3(-20.0f, -10.0f, 0.0f), FVector3(40.0f, 10.0f, 60.0f));
	std::vector<FMatrix4x4> InverseBind;
	for (int32 Joint = 0; Joint < 5; ++Joint)
	{
		InverseBind.push_back(MakePose(Joint, 0.0f).GetInverse()); // 바인드 포즈의 역
	}
	std::vector<float> Radii;
	FSkinnedMeshPalette::ComputeJointRadii(LocalBounds, InverseBind, Radii);

	for (int32 Step = 0; Step < 8; ++Step)
	{
		std::vector<FMatrix4x4> JointWorld;
		for (int32 Joint = 0; Joint < 5; ++Joint)
		{
			JointWorld.push_back(MakePose(Joint, static_cast<float>(Step) * 0.7f) * FMatrix4x4::MakeTranslation(FVector3(500.0f, -200.0f, 30.0f)));
		}
		std::vector<FMatrix4x4> Palette;
		FSkinnedMeshPalette::ComputePalette(InverseBind, JointWorld, Palette);
		FBox Exact;
		for (const FMatrix4x4& Bone : Palette)
		{
			Exact.AddBox(LocalBounds.TransformBy(Bone));
		}
		const FBox Conservative = FSkinnedMeshPalette::ComputeConservativeBounds(JointWorld, Radii);
		E_EXPECT_TRUE(Conservative.IsValid());
		E_EXPECT_TRUE(Conservative.Min.X <= Exact.Min.X + 1.0e-3f && Conservative.Min.Y <= Exact.Min.Y + 1.0e-3f &&
		              Conservative.Min.Z <= Exact.Min.Z + 1.0e-3f);
		E_EXPECT_TRUE(Conservative.Max.X >= Exact.Max.X - 1.0e-3f && Conservative.Max.Y >= Exact.Max.Y - 1.0e-3f &&
		              Conservative.Max.Z >= Exact.Max.Z - 1.0e-3f);
	}
	// 조인트가 없으면 빈 경계
	E_EXPECT_FALSE(FSkinnedMeshPalette::ComputeConservativeBounds({}, {}).IsValid());
}
