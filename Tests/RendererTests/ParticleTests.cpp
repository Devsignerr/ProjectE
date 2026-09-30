#include "Core/Testing/TestFramework.h"
#include "Renderer/ParticleRenderer.h"
#include "Scene/Components.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"
#include "Scene/SceneSerializer.h"

#include <bit>
#include <cmath>
#include <memory>

namespace
{
	constexpr float Tol = 1.0e-4f;

	FVector4 V(float X, float Y = 0.0f, float Z = 0.0f, float W = 0.0f) { return FVector4(X, Y, Z, W); }

	FParticleModule& Add(FParticleEmitter& Emitter, EParticleModuleType Type)
	{
		const FParticleModuleInfo& Info  = GetParticleModuleInfos()[static_cast<size_t>(Type)];
		auto&                      Stage = Emitter.GetStage(Info.Stage);
		Stage.push_back(FParticleModule::Make(Type));
		return Stage.back();
	}

	// 초당 10개, 수명 0.5초, 위로 100cm/초, 주기 1초
	FParticleEmitter MakeEmitter()
	{
		FParticleEmitter Emitter;
		Emitter.Duration     = 1.0f;
		Emitter.MaxParticles = 1000;
		Add(Emitter, EParticleModuleType::SpawnRate).Inputs[0] = FParticleValue::Constant(V(10));
		FParticleModule& Init = Add(Emitter, EParticleModuleType::InitializeParticle);
		Init.Inputs[0]        = FParticleValue::Constant(V(0.5f));
		Init.Inputs[3]        = FParticleValue::Constant(V(0));
		FParticleModule& Cone = Add(Emitter, EParticleModuleType::AddVelocityInCone);
		Cone.Inputs[1]        = FParticleValue::Constant(V(0));
		Cone.Inputs[2]        = FParticleValue::Constant(V(100));
		return Emitter;
	}

	void Simulate(const FParticleEmitter& Emitter, FParticleEmitterInstance& Instance, const FMatrix4x4& World, float Seconds, float Step)
	{
		const int32 Steps = static_cast<int32>(std::lround(Seconds / Step));
		for (int32 Index = 0; Index < Steps; ++Index)
		{
			FParticleSimulation::Update(Emitter, Instance, World, Step);
		}
	}
} // namespace

E_TEST(Particle_ValueModes)
{
	E_EXPECT_NEAR(FParticleValue::Constant(V(3)).Evaluate(V(0.9f), 0.5f).X, 3.0f, Tol);
	const FParticleValue Range = FParticleValue::Range(V(0, 10), V(10, 20));
	const FVector4       Mid   = Range.Evaluate(V(0.5f, 0.25f), 0.0f);
	E_EXPECT_NEAR(Mid.X, 5.0f, Tol);
	E_EXPECT_NEAR(Mid.Y, 12.5f, Tol);

	// 곡선: 구간 선형 보간, 양 끝은 첫/마지막 키
	const FParticleValue Curve = FParticleValue::MakeCurve({ { 1.0f, V(0) }, { 0.0f, V(10) }, { 0.5f, V(20) } }); // 정렬 안 된 입력
	E_EXPECT_NEAR(Curve.Evaluate(V(0), 0.0f).X, 10.0f, Tol);
	E_EXPECT_NEAR(Curve.Evaluate(V(0), 0.25f).X, 15.0f, Tol);
	E_EXPECT_NEAR(Curve.Evaluate(V(0), 0.75f).X, 10.0f, Tol);
	E_EXPECT_NEAR(Curve.Evaluate(V(0), 2.0f).X, 0.0f, Tol);

	// 난수: 결정적이고 [0,1)
	for (uint32 Index = 0; Index < 1000; ++Index)
	{
		const float R = FParticleSimulation::Random01(Index, 7, 3);
		E_EXPECT_TRUE(R >= 0.0f && R < 1.0f);
		E_EXPECT_EQ(R, FParticleSimulation::Random01(Index, 7, 3));
	}
	E_EXPECT_TRUE(FParticleSimulation::Random01(1, 7, 3) != FParticleSimulation::Random01(1, 8, 3));
}

E_TEST(Particle_SystemJsonRoundTrip)
{
	FParticleSystemAsset Asset = FParticleSystemAsset::MakeDefault("Fire");
	FParticleEmitter&    First = Asset.Emitters[0];
	First.Name                 = "Flames";
	First.SimTarget            = EParticleSimTarget::GPU;
	First.bLocalSpace          = true;
	First.Seed                 = 99;
	First.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[1] = FParticleValue::Range(V(4, 2, 1, 1), V(8, 4, 2, 1));
	First.GetStage(EParticleStage::ParticleUpdate)[0].bEnabled = false;
	First.Renderers[0].TexturePath                             = "../Smoke.png";
	First.Renderers[0].SubImageColumns                         = 4;
	First.Renderers[0].Alignment                               = EParticleSpriteAlignment::Velocity;
	FParticleEmitter Second                                    = FParticleEmitter::MakeDefault();
	Second.Name                                                = "Ribbon";
	Second.Renderers[0].Type                                   = EParticleRendererType::Ribbon;
	Asset.Emitters.push_back(Second);

	FParticleSystemAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	E_EXPECT_EQ(Loaded.Emitters.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Loaded.Name == "Fire");
	const FParticleEmitter& L = Loaded.Emitters[0];
	E_EXPECT_TRUE(L.Name == "Flames" && L.SimTarget == EParticleSimTarget::GPU && L.bLocalSpace);
	E_EXPECT_EQ(L.Seed, 99u);
	E_EXPECT_EQ(L.GetStage(EParticleStage::ParticleSpawn).size(), Asset.Emitters[0].GetStage(EParticleStage::ParticleSpawn).size());
	const FParticleValue& Color = L.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[1];
	E_EXPECT_TRUE(Color.Mode == EParticleValueMode::Random);
	E_EXPECT_NEAR(Color.B.X, 8.0f, Tol);
	E_EXPECT_FALSE(L.GetStage(EParticleStage::ParticleUpdate)[0].bEnabled);
	const FParticleValue& ScaleColor = L.GetStage(EParticleStage::ParticleUpdate)[1].Inputs[0];
	E_EXPECT_TRUE(ScaleColor.Mode == EParticleValueMode::Curve);
	E_EXPECT_EQ(ScaleColor.Curve.size(), Asset.Emitters[0].GetStage(EParticleStage::ParticleUpdate)[1].Inputs[0].Curve.size());
	E_EXPECT_TRUE(L.Renderers[0].TexturePath == "../Smoke.png" && L.Renderers[0].SubImageColumns == 4);
	E_EXPECT_TRUE(L.Renderers[0].Alignment == EParticleSpriteAlignment::Velocity);
	E_EXPECT_TRUE(Loaded.Emitters[1].Renderers[0].Type == EParticleRendererType::Ribbon);

	// 모르는 모듈은 건너뛰고, 깨진 JSON은 실패
	FParticleSystemAsset Unknown;
	E_EXPECT_TRUE(Unknown.FromJsonString("{ \"Version\": 2, \"Emitters\": [ { \"ParticleUpdate\": [ { \"Module\": \"NoSuchModule\" }, "
	                                     "{ \"Module\": \"Drag\" } ] } ] }"));
	E_EXPECT_EQ(Unknown.Emitters[0].GetStage(EParticleStage::ParticleUpdate).size(), static_cast<size_t>(1));
	E_EXPECT_FALSE(Unknown.FromJsonString("{ 깨진"));
}

E_TEST(Particle_LegacyFormatConverts)
{
	// 이전 단일 이미터 형식 (버전 없음)
	FParticleSystemAsset Asset;
	E_EXPECT_TRUE(Asset.FromJsonString("{ \"Name\": \"Old\", \"SpawnRate\": 30, \"BurstCount\": 4, \"LifetimeMin\": 1, \"LifetimeMax\": 2, "
	                                   "\"Acceleration\": [0, 0, -980], \"Drag\": 0.5, \"SizeStart\": 20, \"SizeEnd\": 10, "
	                                   "\"ColorStart\": [2, 1, 1, 1], \"ColorEnd\": [1, 1, 1, 0], \"BlendMode\": 0, \"Texture\": \"Smoke.png\" }"));
	E_EXPECT_EQ(Asset.Emitters.size(), static_cast<size_t>(1));
	const FParticleEmitter& E = Asset.Emitters[0];
	E_EXPECT_TRUE(E.FindModule(EParticleModuleType::SpawnRate) != nullptr);
	E_EXPECT_TRUE(E.FindModule(EParticleModuleType::SpawnBurst) != nullptr);
	E_EXPECT_TRUE(E.FindModule(EParticleModuleType::AccelerationForce) != nullptr);
	E_EXPECT_TRUE(E.FindModule(EParticleModuleType::Drag) != nullptr);
	const FParticleModule* Init = E.FindModule(EParticleModuleType::InitializeParticle);
	E_EXPECT_TRUE(Init != nullptr);
	if (Init != nullptr)
	{
		E_EXPECT_NEAR(Init->Inputs[0].B.X, 2.0f, Tol);
		E_EXPECT_NEAR(Init->Inputs[1].A.X, 2.0f, Tol); // 시작 색
	}
	// 끝 크기/색 = 시작 × 곡선 끝값
	const FParticleModule* Size = E.FindModule(EParticleModuleType::ScaleSpriteSize);
	E_EXPECT_TRUE(Size != nullptr && FMath::IsNearlyEqual(Size->Inputs[0].SampleCurve(1.0f).X, 0.5f));
	const FParticleModule* Color = E.FindModule(EParticleModuleType::ScaleColor);
	E_EXPECT_TRUE(Color != nullptr && FMath::IsNearlyEqual(Color->Inputs[0].SampleCurve(1.0f).X, 0.5f));
	E_EXPECT_TRUE(E.Renderers.size() == 1 && E.Renderers[0].BlendMode == EParticleBlendMode::Alpha && E.Renderers[0].TexturePath == "Smoke.png");
}

E_TEST(Particle_SpawnRateLifetimeAndBurst)
{
	const FParticleEmitter   Emitter = MakeEmitter();
	FParticleEmitterInstance Instance;
	// 초당 10개, 수명 0.5초 → 안정 상태에서 약 5개
	Simulate(Emitter, Instance, FMatrix4x4::Identity, 0.3f, 0.01f);
	E_EXPECT_EQ(Instance.Particles.size(), static_cast<size_t>(3));
	Simulate(Emitter, Instance, FMatrix4x4::Identity, 2.0f, 0.01f);
	E_EXPECT_TRUE(Instance.Particles.size() >= 4 && Instance.Particles.size() <= 6);

	// 버스트: 주기마다 한 번, 반복 끄면 한 번만, 최대 개수 제한
	FParticleEmitter Burst = MakeEmitter();
	Burst.GetStage(EParticleStage::EmitterUpdate).clear();
	Add(Burst, EParticleModuleType::SpawnBurst).Inputs[0] = FParticleValue::Constant(V(5));
	Burst.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[0] = FParticleValue::Constant(V(10));
	FParticleEmitterInstance Looping;
	FParticleSimulation::Update(Burst, Looping, FMatrix4x4::Identity, 0.01f);
	E_EXPECT_EQ(Looping.Particles.size(), static_cast<size_t>(5));
	Simulate(Burst, Looping, FMatrix4x4::Identity, 1.0f, 0.01f);
	E_EXPECT_EQ(Looping.Particles.size(), static_cast<size_t>(10));

	Burst.bLoop = false;
	FParticleEmitterInstance Once;
	Simulate(Burst, Once, FMatrix4x4::Identity, 3.0f, 0.01f);
	E_EXPECT_EQ(Once.Particles.size(), static_cast<size_t>(5));
	E_EXPECT_TRUE(Once.bFinished);

	Burst.bLoop        = true;
	Burst.MaxParticles = 7;
	FParticleEmitterInstance Capped;
	Simulate(Burst, Capped, FMatrix4x4::Identity, 3.0f, 0.01f);
	E_EXPECT_EQ(Capped.Particles.size(), static_cast<size_t>(7));
}

E_TEST(Particle_ForcesAndWorldTransform)
{
	FParticleEmitter Emitter = MakeEmitter();
	Emitter.GetStage(EParticleStage::EmitterUpdate).clear();
	Add(Emitter, EParticleModuleType::SpawnBurst).Inputs[0] = FParticleValue::Constant(V(1));
	Emitter.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[0] = FParticleValue::Constant(V(10));
	Add(Emitter, EParticleModuleType::AccelerationForce).Inputs[0] = FParticleValue::Constant(V(0, 0, -100));

	// 이미터가 X로 90° 회전 + 이동: 로컬 +Z 방향이 월드 방향으로 바뀌고 위치도 따라간다
	const FQuat              Rotation = FQuat::FromAxisAngle(FVector3::ForwardVector, FMath::DegreesToRadians(90.0f));
	const FMatrix4x4         World    = FMatrix4x4::MakeTransform(FVector3(100.0f, 0.0f, 0.0f), Rotation, FVector3::OneVector);
	FParticleEmitterInstance Instance;
	FParticleSimulation::Update(Emitter, Instance, World, 0.0f);
	E_EXPECT_EQ(Instance.Particles.size(), static_cast<size_t>(1));
	const FVector3 ExpectedDirection = Rotation.RotateVector(FVector3::UpVector);
	E_EXPECT_EQUALS(Instance.Particles[0].Position, FVector3(100.0f, 0.0f, 0.0f), 1.0e-3f);
	E_EXPECT_EQUALS(Instance.Particles[0].Velocity, ExpectedDirection * 100.0f, 1.0e-2f);
	Simulate(Emitter, Instance, World, 1.0f, 0.01f);
	E_EXPECT_EQUALS(Instance.Particles[0].Velocity, ExpectedDirection * 100.0f + FVector3(0.0f, 0.0f, -100.0f), 1.0e-2f);

	// 원뿔 퍼짐: 모든 방향이 반각 안
	FParticleEmitter Cone = Emitter;
	Cone.GetStage(EParticleStage::EmitterUpdate)[0].Inputs[0] = FParticleValue::Constant(V(200));
	Cone.GetStage(EParticleStage::ParticleSpawn)[1].Inputs[1] = FParticleValue::Constant(V(30));
	FParticleEmitterInstance ConeInstance;
	FParticleSimulation::Update(Cone, ConeInstance, FMatrix4x4::Identity, 0.0f);
	E_EXPECT_EQ(ConeInstance.Particles.size(), static_cast<size_t>(200));
	const float CosLimit = std::cos(FMath::DegreesToRadians(30.0f)) - 1.0e-4f;
	for (const FParticle& Particle : ConeInstance.Particles)
	{
		E_EXPECT_TRUE(FVector3::Dot(Particle.Velocity.GetNormalized(), FVector3::UpVector) >= CosLimit);
	}

	// 바닥 충돌: 아래로 떨어지는 입자가 바닥(Z=0) 아래로 가지 않는다
	FParticleEmitter Drop = Emitter;
	Drop.GetStage(EParticleStage::ParticleSpawn)[1].Inputs[2] = FParticleValue::Constant(V(0));
	Drop.GetStage(EParticleStage::ParticleUpdate)[0].Inputs[0] = FParticleValue::Constant(V(0, 0, -980));
	Add(Drop, EParticleModuleType::Collision);
	Drop.bLoop = false;
	FParticleEmitterInstance DropInstance;
	Simulate(Drop, DropInstance, FMatrix4x4::MakeTranslation(FVector3(0, 0, 100)), 2.0f, 0.01f);
	E_EXPECT_EQ(DropInstance.Particles.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(DropInstance.Particles[0].Position.Z >= 0.0f);
}

E_TEST(Particle_ShapesAndUpdateModules)
{
	FParticleEmitter Emitter = MakeEmitter();
	Emitter.GetStage(EParticleStage::EmitterUpdate)[0].Inputs[0] = FParticleValue::Constant(V(0));
	Add(Emitter, EParticleModuleType::SpawnBurst).Inputs[0] = FParticleValue::Constant(V(300));
	Emitter.GetStage(EParticleStage::ParticleSpawn)[1].Inputs[2] = FParticleValue::Constant(V(0));
	FParticleModule& Shape = Add(Emitter, EParticleModuleType::ShapeLocation);

	const auto SpawnAll = [&](int32 ShapeIndex, bool bSurface) {
		Shape.Inputs[0] = FParticleValue::Constant(V(static_cast<float>(ShapeIndex)));
		Shape.Inputs[5] = FParticleValue::Constant(V(bSurface ? 1.0f : 0.0f));
		FParticleEmitterInstance Instance;
		FParticleSimulation::Update(Emitter, Instance, FMatrix4x4::Identity, 0.0f);
		return Instance.Particles;
	};
	for (const FParticle& P : SpawnAll(1, false)) // 구 (반지름 20) 안
	{
		E_EXPECT_TRUE(P.Position.Length() <= 20.0f + 1e-3f);
	}
	for (const FParticle& P : SpawnAll(1, true)) // 구 표면
	{
		E_EXPECT_NEAR(P.Position.Length(), 20.0f, 1e-2f);
	}
	for (const FParticle& P : SpawnAll(2, false)) // 상자 (반 크기 50)
	{
		E_EXPECT_TRUE(std::abs(P.Position.X) <= 50.0f + 1e-3f && std::abs(P.Position.Y) <= 50.0f + 1e-3f && std::abs(P.Position.Z) <= 50.0f + 1e-3f);
	}
	for (const FParticle& P : SpawnAll(3, false)) // 원기둥 (반지름 20, 높이 50)
	{
		E_EXPECT_TRUE(std::sqrt(P.Position.X * P.Position.X + P.Position.Y * P.Position.Y) <= 20.0f + 1e-3f);
		E_EXPECT_TRUE(P.Position.Z >= 0.0f && P.Position.Z <= 50.0f + 1e-3f);
	}

	// 색/크기 곡선은 시작 값 × 곡선, 감속은 속도를 줄인다, 플립북은 수명에 맞춰 진행
	FParticleEmitter Visual = MakeEmitter();
	Visual.GetStage(EParticleStage::EmitterUpdate)[0].Inputs[0] = FParticleValue::Constant(V(0));
	Add(Visual, EParticleModuleType::SpawnBurst).Inputs[0] = FParticleValue::Constant(V(1));
	Visual.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[0] = FParticleValue::Constant(V(2.0f));
	Visual.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[1] = FParticleValue::Constant(V(2, 2, 2, 1));
	Visual.GetStage(EParticleStage::ParticleSpawn)[0].Inputs[2] = FParticleValue::Constant(V(10, 20));
	Add(Visual, EParticleModuleType::ScaleColor).Inputs[0]       = FParticleValue::MakeCurve({ { 0.0f, V(1, 1, 1, 1) }, { 1.0f, V(0, 0, 0, 0) } });
	Add(Visual, EParticleModuleType::ScaleSpriteSize).Inputs[0]  = FParticleValue::MakeCurve({ { 0.0f, V(1, 1) }, { 1.0f, V(3, 3) } });
	Add(Visual, EParticleModuleType::Drag).Inputs[0]             = FParticleValue::Constant(V(1));
	FParticleModule& SubUV                                       = Add(Visual, EParticleModuleType::SubUVAnimation);
	SubUV.Inputs[1]                                              = FParticleValue::Constant(V(8));
	FParticleEmitterInstance Instance;
	FParticleSimulation::Update(Visual, Instance, FMatrix4x4::Identity, 0.0f);
	Simulate(Visual, Instance, FMatrix4x4::Identity, 1.0f, 0.01f); // 수명 절반
	E_EXPECT_EQ(Instance.Particles.size(), static_cast<size_t>(1));
	const FParticle& P = Instance.Particles[0];
	E_EXPECT_NEAR(P.Color.X, 1.0f, 2e-2f);
	E_EXPECT_NEAR(P.Size.X, 20.0f, 0.2f);
	E_EXPECT_NEAR(P.Size.Y, 40.0f, 0.4f);
	E_EXPECT_NEAR(P.Velocity.Length(), 100.0f * std::exp(-1.0f), 0.5f);
	E_EXPECT_NEAR(P.SubImage, 4.0f, 0.1f);
}

E_TEST(Particle_DeterministicRestart)
{
	FParticleEmitter Emitter = MakeEmitter();
	Emitter.GetStage(EParticleStage::ParticleSpawn)[1].Inputs[1] = FParticleValue::Constant(V(45));
	Emitter.GetStage(EParticleStage::ParticleSpawn)[1].Inputs[2] = FParticleValue::Range(V(50), V(150));
	Add(Emitter, EParticleModuleType::CurlNoiseForce);

	FParticleEmitterInstance A;
	Simulate(Emitter, A, FMatrix4x4::Identity, 0.4f, 0.02f);
	FParticleEmitterInstance B;
	Simulate(Emitter, B, FMatrix4x4::Identity, 0.4f, 0.02f);
	E_EXPECT_EQ(A.Particles.size(), B.Particles.size());
	for (size_t Index = 0; Index < A.Particles.size() && Index < B.Particles.size(); ++Index)
	{
		E_EXPECT_EQUALS(A.Particles[Index].Position, B.Particles[Index].Position, Tol);
	}
	// 시드가 다르면 다른 결과
	Emitter.Seed = 2;
	FParticleEmitterInstance C;
	Simulate(Emitter, C, FMatrix4x4::Identity, 0.4f, 0.02f);
	E_EXPECT_TRUE(!C.Particles.empty() && !A.Particles[0].Velocity.Equals(C.Particles[0].Velocity, Tol));
}

E_TEST(Particle_GpuEmitterQueuesSteps)
{
	FParticleEmitter Emitter = MakeEmitter();
	Emitter.SimTarget        = EParticleSimTarget::GPU;
	FParticleEmitterInstance Instance;
	Simulate(Emitter, Instance, FMatrix4x4::Identity, 1.0f, 0.1f);
	// CPU 입자는 만들지 않고, 계산 요청만 쌓는다 (렌더되지 않으면 8개에서 합쳐짐)
	E_EXPECT_TRUE(Instance.Particles.empty());
	E_EXPECT_TRUE(!Instance.PendingGpuSteps.empty() && Instance.PendingGpuSteps.size() <= 8);
	uint32 Spawned = 0;
	float  Seconds = 0.0f;
	for (const FParticleGpuStep& Step : Instance.PendingGpuSteps)
	{
		E_EXPECT_EQ(Step.SpawnStart, Spawned); // 생성 순번이 이어진다
		Spawned += Step.SpawnCount;
		Seconds += Step.DeltaSeconds;
	}
	E_EXPECT_EQ(Spawned, 10u);
	E_EXPECT_NEAR(Seconds, 1.0f, 1e-3f);

	// CPU로 되돌리면 GPU 상태는 버린다
	Emitter.SimTarget = EParticleSimTarget::CPU;
	FParticleSimulation::Update(Emitter, Instance, FMatrix4x4::Identity, 0.1f);
	E_EXPECT_TRUE(Instance.PendingGpuSteps.empty());

	// GPU 프로그램: 켜진 생성/갱신 모듈 순서대로, 곡선 키는 뒤에
	FParticleEmitter Gpu = FParticleEmitter::MakeDefault();
	std::vector<FVector4>  Program;
	FParticleSimConstants  Constants;
	FParticleRenderer::BuildGpuProgram(Gpu, Program, Constants);
	E_EXPECT_EQ(Constants.SpawnModuleCount, static_cast<uint32>(Gpu.GetStage(EParticleStage::ParticleSpawn).size()));
	E_EXPECT_EQ(Constants.UpdateModuleCount, static_cast<uint32>(Gpu.GetStage(EParticleStage::ParticleUpdate).size()));
	E_EXPECT_EQ(Constants.SpawnOffset, 0u);
	E_EXPECT_EQ(std::bit_cast<uint32>(Program[0].X), static_cast<uint32>(EParticleModuleType::InitializeParticle));
	E_EXPECT_EQ(std::bit_cast<uint32>(Program[Constants.UpdateOffset].X), static_cast<uint32>(Gpu.GetStage(EParticleStage::ParticleUpdate)[0].Type));
	E_EXPECT_EQ(Constants.Capacity, Gpu.MaxParticles);
}

E_TEST(Particle_ComponentSerializeAndCloneKeepsSystem)
{
	FScene        Scene;
	const FEntity Entity                = Scene.CreateEntity("Emitter");
	FParticleSystemComponent& Component = Scene.GetRegistry().Emplace<FParticleSystemComponent>(Entity);
	Component.Asset                     = "Particles/Fire.eparticle";
	Component.Speed                     = 2.0f;
	auto System                         = std::make_shared<FParticleSystemAsset>();
	System->Emitters                    = { MakeEmitter(), MakeEmitter() };
	Component.Runtime.System            = System;
	Component.Runtime.ResolvedAsset     = Component.Asset;
	Scene.UpdateTransforms();
	FParticleSystem::Update(Scene, 0.5f);
	E_EXPECT_EQ(Component.Runtime.Emitters.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(FParticleSystem::CountParticles(Scene) > 0);

	// 이미터를 끄면 그 이미터 입자는 사라진다
	System->Emitters[1].bEnabled = false;
	FParticleSystem::Update(Scene, 0.01f);
	E_EXPECT_TRUE(Component.Runtime.Emitters[1].Particles.empty());

	// 직렬화: 에셋 경로/설정만 저장 (런타임 제외)
	FScene Restored;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Restored, FSceneSerializer::ToJsonString(Scene)));
	bool bFound = false;
	Restored.GetRegistry().View<FParticleSystemComponent>().Each([&](FEntity, FParticleSystemComponent& Loaded) {
		bFound = Loaded.Asset == "Particles/Fire.eparticle" && FMath::IsNearlyEqual(Loaded.Speed, 2.0f) && !Loaded.Runtime.System;
	});
	E_EXPECT_TRUE(bFound);

	// 플레이 모드 복제: 시스템은 공유, 입자는 처음부터
	FScene Clone;
	FSceneCloner::Clone(Scene, Clone);
	bool bShared = false;
	Clone.GetRegistry().View<FParticleSystemComponent>().Each([&](FEntity, FParticleSystemComponent& Cloned) {
		bShared = Cloned.Runtime.System == Component.Runtime.System && Cloned.Runtime.Emitters.empty() && Cloned.Runtime.ResolvedAsset == Component.Asset;
	});
	E_EXPECT_TRUE(bShared);
}
