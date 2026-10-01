#include "Core/Testing/TestFramework.h"
#include "Renderer/ParticleBounds.h"
#include "Scene/Particles.h"

namespace
{
	// 추정 반경이 같은 이미터의 CPU 시뮬레이션 입자(+ 그려지는 크기)를 모두 포함하는지
	void ExpectRadiusContainsSimulation(const FParticleEmitter& Emitter, const FMatrix4x4& World)
	{
		const float Radius = ParticleBounds::EstimateLocalRadius(Emitter, 0.0f) * ParticleBounds::MaxAxisScale(World);
		float       StretchSeconds = 0.0f;
		const float SizeFactor     = ParticleBounds::RenderSizeFactor(Emitter, 0.0f, StretchSeconds);
		const FVector3 Origin      = World.TransformPosition(FVector3::ZeroVector);

		FParticleEmitterInstance Instance;
		float                    Worst = 0.0f;
		for (int32 Frame = 0; Frame < 240; ++Frame)
		{
			FParticleSimulation::Update(Emitter, Instance, World, 1.0f / 60.0f);
			for (const FParticle& Particle : Instance.Particles)
			{
				const float Reach = FVector3::Distance(Particle.Position, Origin) + FMath::Max(Particle.Size.X, Particle.Size.Y) * SizeFactor;
				Worst             = FMath::Max(Worst, Reach);
			}
		}
		E_EXPECT_TRUE(!Instance.Particles.empty());
		E_EXPECT_TRUE(Worst <= Radius);
	}
} // namespace

E_TEST(ParticleBounds_MaxAbsOfValueModes)
{
	E_EXPECT_NEAR(ParticleBounds::MaxAbs(FParticleValue::Constant(FVector4(-3.0f, 2.0f, 0.0f, 0.0f))).X, 3.0f, 1.0e-6f);
	const FVector4 Range = ParticleBounds::MaxAbs(FParticleValue::Range(FVector4(-5.0f, 1.0f, 0.0f, 0.0f), FVector4(2.0f, -4.0f, 0.0f, 0.0f)));
	E_EXPECT_NEAR(Range.X, 5.0f, 1.0e-6f);
	E_EXPECT_NEAR(Range.Y, 4.0f, 1.0e-6f);
	const FVector4 Curve = ParticleBounds::MaxAbs(FParticleValue::MakeCurve({ { 0.0f, FVector4(1.0f, 0.0f, 0.0f, 0.0f) }, { 1.0f, FVector4(-7.0f, 0.0f, 0.0f, 0.0f) } }));
	E_EXPECT_NEAR(Curve.X, 7.0f, 1.0e-6f);
}

E_TEST(ParticleBounds_EstimateContainsSimulatedParticles)
{
	// 기본 이미터 (원뿔 속도 + 크기 곡선) — 원점, 그리고 이동/회전/배율 이미터
	FParticleEmitter Emitter = FParticleEmitter::MakeDefault();
	ExpectRadiusContainsSimulation(Emitter, FMatrix4x4::Identity);
	ExpectRadiusContainsSimulation(Emitter, FMatrix4x4::MakeTransform(FVector3(300.0f, -50.0f, 20.0f), FQuat::FromEuler(30.0f, 60.0f, 0.0f), FVector3(2.0f)));

	// 힘 모듈 (중력 + 흐름 노이즈 + 가속) + 상자 모양
	Emitter.GetStage(EParticleStage::ParticleUpdate).push_back(FParticleModule::Make(EParticleModuleType::GravityForce));
	Emitter.GetStage(EParticleStage::ParticleUpdate).push_back(FParticleModule::Make(EParticleModuleType::CurlNoiseForce));
	Emitter.GetStage(EParticleStage::ParticleUpdate).push_back(FParticleModule::Make(EParticleModuleType::AccelerationForce));
	FParticleModule Shape = FParticleModule::Make(EParticleModuleType::ShapeLocation);
	Shape.Inputs[0]       = FParticleValue::Constant(FVector4(2.0f, 0.0f, 0.0f, 0.0f)); // 상자
	Emitter.GetStage(EParticleStage::ParticleSpawn).push_back(Shape);
	ExpectRadiusContainsSimulation(Emitter, FMatrix4x4::Identity);

	// 끈 모듈은 무시
	const float Before = ParticleBounds::EstimateLocalRadius(Emitter, 0.0f);
	Emitter.GetStage(EParticleStage::ParticleUpdate).back().bEnabled = false;
	E_EXPECT_TRUE(ParticleBounds::EstimateLocalRadius(Emitter, 0.0f) < Before);
}

E_TEST(ParticleBounds_CpuBoundsAndSizeFactor)
{
	std::vector<FParticle> Particles(2);
	Particles[0].Position = FVector3(0.0f, 0.0f, 0.0f);
	Particles[0].Size     = FVector2(10.0f, 20.0f);
	Particles[1].Position = FVector3(100.0f, -50.0f, 30.0f);
	Particles[1].Size     = FVector2(4.0f, 4.0f);
	const FBox Bounds     = ParticleBounds::ComputeCpuBounds(Particles, FMatrix4x4::Identity, 0.5f, 0.0f);
	E_EXPECT_TRUE(Bounds.Equals(FBox(FVector3(-10.0f, -60.0f, -10.0f), FVector3(110.0f, 10.0f, 40.0f)), 1.0e-4f));
	E_EXPECT_FALSE(ParticleBounds::ComputeCpuBounds({}, FMatrix4x4::Identity, 1.0f, 0.0f).IsValid());

	// 렌더러 종류별 배율
	FParticleEmitter Emitter = FParticleEmitter::MakeDefault();
	float            Stretch = 0.0f;
	E_EXPECT_NEAR(ParticleBounds::RenderSizeFactor(Emitter, 0.0f, Stretch), 0.70710678f, 1.0e-5f);
	Emitter.Renderers[0].Type = EParticleRendererType::Mesh;
	E_EXPECT_NEAR(ParticleBounds::RenderSizeFactor(Emitter, 150.0f, Stretch), 1.5f, 1.0e-5f);
}

E_TEST(ParticleBounds_FixedBoundsRoundTrip)
{
	FParticleSystemAsset Asset = FParticleSystemAsset::MakeDefault("Test");
	Asset.Emitters[0].bFixedBounds   = true;
	Asset.Emitters[0].FixedBoundsMin = FVector3(-1.0f, -2.0f, -3.0f);
	Asset.Emitters[0].FixedBoundsMax = FVector3(4.0f, 5.0f, 6.0f);
	FParticleSystemAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	E_EXPECT_TRUE(Loaded.Emitters[0].bFixedBounds);
	E_EXPECT_TRUE(Loaded.Emitters[0].FixedBoundsMax.Equals(FVector3(4.0f, 5.0f, 6.0f)));

	// 끄면 저장하지 않는다 (기존 파일과 같은 모양)
	Asset.Emitters[0].bFixedBounds = false;
	E_EXPECT_TRUE(Asset.ToJsonString().find("FixedBounds") == std::string::npos);
}

E_TEST(ParticleBounds_TrimDeferredStepsKeepsLifetimeAndMerges)
{
	// 0.1초 요청 30개 (생성 순번 이어짐), 수명 0.95초 → 뒤에서 합이 0.95초 이상이 되는 10개만 남는다
	std::vector<FParticleGpuStep> Steps;
	std::vector<float>            Times;
	for (uint32 Index = 0; Index < 30; ++Index)
	{
		FParticleGpuStep Step;
		Step.DeltaSeconds = 0.1f;
		Step.SpawnStart   = Index * 5;
		Step.SpawnCount   = 5;
		Steps.push_back(Step);
		Times.push_back(0.1f * static_cast<float>(Index + 1));
	}
	ParticleBounds::TrimDeferredSteps(Steps, Times, 0.95f, 64, 1000);
	E_EXPECT_EQ(Steps.size(), size_t(10));
	E_EXPECT_EQ(Steps.front().SpawnStart, 100u);
	E_EXPECT_NEAR(Times.back(), 3.0f, 1.0e-4f);

	// 상한 4개: 이웃끼리 합쳐 생성 구간이 이어진다 (10 → 5 → 3), 생성 수는 MaxParticles까지
	ParticleBounds::TrimDeferredSteps(Steps, Times, 100.0f, 4, 12);
	E_EXPECT_EQ(Steps.size(), size_t(3));
	E_EXPECT_EQ(Steps[0].SpawnStart, 100u);
	E_EXPECT_EQ(Steps[0].SpawnCount, 12u); // 5 × 4 = 20 → 12
	E_EXPECT_EQ(Steps[1].SpawnStart, 120u);
	E_EXPECT_NEAR(Steps[0].DeltaSeconds, 0.4f, 1.0e-4f);
	E_EXPECT_NEAR(Times[0], 2.4f, 1.0e-4f);
	E_EXPECT_NEAR(Steps[2].DeltaSeconds, 0.2f, 1.0e-4f);
	E_EXPECT_NEAR(Times[2], 3.0f, 1.0e-4f);
}
