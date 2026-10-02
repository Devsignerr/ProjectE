#include "Scene/Particles.h"

#include "Core/Profiling.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;
	using EKind = EParticleInputKind;

	constexpr FVector4 V(float X, float Y = 0.0f, float Z = 0.0f, float W = 0.0f) { return FVector4(X, Y, Z, W); }

	// ---- 모듈 입력 목록 (순서 = 셰이더 입력 번호) -------------------------------------------------------------
	constexpr FParticleInputInfo GSpawnRateInputs[] = {
		{ "SpawnRate", "초당 개수", EKind::Float, V(20), true, true, nullptr, 0.5f },
	};
	constexpr FParticleInputInfo GSpawnBurstInputs[] = {
		{ "SpawnCount", "개수", EKind::Float, V(20), true, false, nullptr, 1.0f },
		{ "SpawnTime", "시각 (주기 안, 초)", EKind::Float, V(0), false, false, nullptr, 0.01f },
	};
	constexpr FParticleInputInfo GInitializeInputs[] = {
		{ "Lifetime", "수명 (초)", EKind::Float, V(1.5f), true, true, nullptr, 0.01f },
		{ "Color", "색", EKind::Color, V(1, 1, 1, 1), true, true, nullptr, 0.01f },
		{ "SpriteSize", "크기 (cm, 가로/세로)", EKind::Vector, V(20, 20, 0), true, true, nullptr, 0.5f },
		{ "SpriteRotation", "시작 회전 (도)", EKind::Float, V(0), true, true, nullptr, 1.0f },
		{ "Mass", "질량", EKind::Float, V(1), true, true, nullptr, 0.01f },
	};
	constexpr FParticleInputInfo GShapeInputs[] = {
		{ "Shape", "모양", EKind::Enum, V(1), false, false, "점\0구\0상자\0원기둥\0원뿔\0원환\0", 1.0f },
		{ "Radius", "반지름 (cm)", EKind::Float, V(20), true, false, nullptr, 0.5f },
		{ "BoxSize", "상자 반 크기 (cm)", EKind::Vector, V(50, 50, 50), false, false, nullptr, 0.5f },
		{ "Height", "높이 (cm)", EKind::Float, V(50), false, false, nullptr, 0.5f },
		{ "MinorRadius", "원환 두께 반지름 (cm)", EKind::Float, V(5), false, false, nullptr, 0.1f },
		{ "SurfaceOnly", "표면에서만", EKind::Bool, V(0), false, false, nullptr, 1.0f },
		{ "Offset", "위치 오프셋 (cm)", EKind::Vector, V(0, 0, 0), true, false, nullptr, 0.5f },
	};
	constexpr FParticleInputInfo GAddVelocityInputs[] = {
		{ "Velocity", "속도 (cm/초)", EKind::Vector, V(0, 0, 100), true, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GAddVelocityInConeInputs[] = {
		{ "ConeAxis", "원뿔 축", EKind::Vector, V(0, 0, 1), false, false, nullptr, 0.01f },
		{ "ConeAngle", "퍼짐 각도 (도)", EKind::Float, V(25), true, true, nullptr, 0.5f },
		{ "Speed", "속력 (cm/초)", EKind::Float, V(150), true, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GAddVelocityFromPointInputs[] = {
		{ "Origin", "중심 (로컬, cm)", EKind::Vector, V(0, 0, 0), false, false, nullptr, 0.5f },
		{ "Speed", "속력 (cm/초)", EKind::Float, V(150), true, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GGravityInputs[] = {
		{ "Gravity", "중력 (cm/초²)", EKind::Vector, V(0, 0, -980), false, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GDragInputs[] = {
		{ "Drag", "감속 (1/초)", EKind::Float, V(1), true, true, nullptr, 0.01f },
	};
	constexpr FParticleInputInfo GAccelerationInputs[] = {
		{ "Acceleration", "가속도 (cm/초²)", EKind::Vector, V(0, 0, 100), true, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GCurlNoiseInputs[] = {
		{ "Strength", "세기 (cm/초²)", EKind::Float, V(300), true, true, nullptr, 1.0f },
		{ "Frequency", "촘촘함 (1/cm)", EKind::Float, V(0.01f), false, false, nullptr, 0.0005f },
		{ "PanSpeed", "흐름 이동 (cm/초)", EKind::Vector, V(0, 0, 0), false, false, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GVortexInputs[] = {
		{ "Axis", "축", EKind::Vector, V(0, 0, 1), false, false, nullptr, 0.01f },
		{ "Center", "중심 (로컬, cm)", EKind::Vector, V(0, 0, 0), false, false, nullptr, 0.5f },
		{ "Amount", "세기 (cm/초²)", EKind::Float, V(300), true, true, nullptr, 1.0f },
		{ "PullIn", "안쪽으로 끌기 (cm/초²)", EKind::Float, V(0), true, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GPointAttractionInputs[] = {
		{ "Position", "위치 (로컬, cm)", EKind::Vector, V(0, 0, 100), false, false, nullptr, 0.5f },
		{ "Strength", "세기 (cm/초²)", EKind::Float, V(500), true, true, nullptr, 1.0f },
		{ "Radius", "영향 반지름 (cm)", EKind::Float, V(500), false, false, nullptr, 1.0f },
		{ "KillRadius", "도착하면 없애는 반지름 (cm)", EKind::Float, V(0), false, false, nullptr, 0.5f },
	};
	constexpr FParticleInputInfo GScaleColorInputs[] = {
		{ "Scale", "색 배율 (수명 곡선)", EKind::Color, V(1, 1, 1, 1), false, true, nullptr, 0.01f },
	};
	constexpr FParticleInputInfo GScaleSizeInputs[] = {
		{ "Scale", "크기 배율 (수명 곡선)", EKind::Vector, V(1, 1, 0), false, true, nullptr, 0.01f },
	};
	constexpr FParticleInputInfo GRotationRateInputs[] = {
		{ "RotationRate", "회전 속도 (도/초)", EKind::Float, V(90), true, true, nullptr, 1.0f },
	};
	constexpr FParticleInputInfo GSubUVInputs[] = {
		{ "Mode", "재생 방식", EKind::Enum, V(0), false, false, "수명에 맞춰\0초당 프레임\0무작위 한 장\0", 1.0f },
		{ "FrameCount", "프레임 수", EKind::Float, V(16), false, false, nullptr, 1.0f },
		{ "FrameRate", "초당 프레임", EKind::Float, V(30), false, false, nullptr, 0.5f },
	};
	constexpr FParticleInputInfo GCollisionInputs[] = {
		{ "PlaneHeight", "바닥 높이 Z (cm)", EKind::Float, V(0), false, false, nullptr, 0.5f },
		{ "Restitution", "튀는 정도", EKind::Float, V(0.4f), true, false, nullptr, 0.01f },
		{ "Friction", "마찰", EKind::Float, V(0.2f), true, false, nullptr, 0.01f },
		{ "KillOnCollide", "닿으면 없애기", EKind::Bool, V(0), false, false, nullptr, 1.0f },
	};

	constexpr FParticleModuleInfo GModuleInfos[] = {
		{ EParticleModuleType::SpawnRate, "SpawnRate", "초당 생성", EParticleStage::EmitterUpdate, GSpawnRateInputs, "주기 동안 초당 개수만큼 만든다" },
		{ EParticleModuleType::SpawnBurst, "SpawnBurstInstantaneous", "한 번에 생성", EParticleStage::EmitterUpdate, GSpawnBurstInputs,
		  "주기마다 지정한 시각에 한꺼번에 만든다" },
		{ EParticleModuleType::InitializeParticle, "InitializeParticle", "입자 초기화", EParticleStage::ParticleSpawn, GInitializeInputs,
		  "수명, 색, 크기, 회전, 질량" },
		{ EParticleModuleType::ShapeLocation, "ShapeLocation", "모양 안 위치", EParticleStage::ParticleSpawn, GShapeInputs, "점/구/상자/원기둥/원뿔/원환" },
		{ EParticleModuleType::AddVelocity, "AddVelocity", "속도 더하기", EParticleStage::ParticleSpawn, GAddVelocityInputs, "고정 방향 (이미터 로컬)" },
		{ EParticleModuleType::AddVelocityInCone, "AddVelocityInCone", "원뿔 속도", EParticleStage::ParticleSpawn, GAddVelocityInConeInputs,
		  "축 주위 원뿔 안의 무작위 방향" },
		{ EParticleModuleType::AddVelocityFromPoint, "AddVelocityFromPoint", "점에서 퍼지는 속도", EParticleStage::ParticleSpawn,
		  GAddVelocityFromPointInputs, "중심에서 입자 위치 쪽으로" },
		{ EParticleModuleType::GravityForce, "GravityForce", "중력", EParticleStage::ParticleUpdate, GGravityInputs, "일정한 가속 (기본 -980)" },
		{ EParticleModuleType::Drag, "Drag", "감속", EParticleStage::ParticleUpdate, GDragInputs, "속도를 지수적으로 줄인다" },
		{ EParticleModuleType::AccelerationForce, "AccelerationForce", "가속", EParticleStage::ParticleUpdate, GAccelerationInputs, "임의 방향 가속" },
		{ EParticleModuleType::CurlNoiseForce, "CurlNoiseForce", "흐름 노이즈", EParticleStage::ParticleUpdate, GCurlNoiseInputs,
		  "연기/마법처럼 소용돌이치는 흐름" },
		{ EParticleModuleType::VortexForce, "VortexForce", "소용돌이", EParticleStage::ParticleUpdate, GVortexInputs, "축 둘레로 돌린다" },
		{ EParticleModuleType::PointAttractionForce, "PointAttractionForce", "점으로 끌림", EParticleStage::ParticleUpdate, GPointAttractionInputs,
		  "한 점으로 끌어당긴다" },
		{ EParticleModuleType::ScaleColor, "ScaleColor", "색 변화", EParticleStage::ParticleUpdate, GScaleColorInputs, "시작 색 × 수명 곡선" },
		{ EParticleModuleType::ScaleSpriteSize, "ScaleSpriteSize", "크기 변화", EParticleStage::ParticleUpdate, GScaleSizeInputs, "시작 크기 × 수명 곡선" },
		{ EParticleModuleType::SpriteRotationRate, "SpriteRotationRate", "회전", EParticleStage::ParticleUpdate, GRotationRateInputs, "초당 회전" },
		{ EParticleModuleType::SubUVAnimation, "SubUVAnimation", "플립북", EParticleStage::ParticleUpdate, GSubUVInputs,
		  "텍스처 칸을 차례로 (렌더러 가로/세로 칸 수와 맞출 것)" },
		{ EParticleModuleType::Collision, "Collision", "충돌", EParticleStage::ParticleUpdate, GCollisionInputs, "바닥 평면에서 튀기거나 없앤다" },
	};
	static_assert(std::size(GModuleInfos) == static_cast<size_t>(EParticleModuleType::Count));

	// ---- 난수 / 노이즈 (셰이더와 같은 식) ------------------------------------------------------------------
	float HashToFloat(uint32 Value) { return static_cast<float>(FParticleSimulation::Hash(Value) >> 8) * (1.0f / 16777216.0f); }

	FVector4 Random4(uint32 SpawnIndex, uint32 Seed, uint32 Stream)
	{
		return FVector4(FParticleSimulation::Random01(SpawnIndex, Seed, Stream * 4 + 0), FParticleSimulation::Random01(SpawnIndex, Seed, Stream * 4 + 1),
		                FParticleSimulation::Random01(SpawnIndex, Seed, Stream * 4 + 2), FParticleSimulation::Random01(SpawnIndex, Seed, Stream * 4 + 3));
	}

	// 격자 값 노이즈 (삼선형 보간). 격자점 난수는 좌표 해시
	float LatticeValue(int32 X, int32 Y, int32 Z, uint32 Channel)
	{
		const uint32 Key = static_cast<uint32>(X) * 73856093u ^ static_cast<uint32>(Y) * 19349663u ^ static_cast<uint32>(Z) * 83492791u ^ Channel * 2654435761u;
		return HashToFloat(Key) * 2.0f - 1.0f;
	}

	float ValueNoise(const FVector3& P, uint32 Channel)
	{
		const float  FX = std::floor(P.X), FY = std::floor(P.Y), FZ = std::floor(P.Z);
		const int32  X  = static_cast<int32>(FX), Y = static_cast<int32>(FY), Z = static_cast<int32>(FZ);
		const auto   Smooth = [](float T) { return T * T * (3.0f - 2.0f * T); };
		const float  TX = Smooth(P.X - FX), TY = Smooth(P.Y - FY), TZ = Smooth(P.Z - FZ);
		const auto   L  = [&](int32 DX, int32 DY, int32 DZ) { return LatticeValue(X + DX, Y + DY, Z + DZ, Channel); };
		const float  X00 = FMath::Lerp(L(0, 0, 0), L(1, 0, 0), TX), X10 = FMath::Lerp(L(0, 1, 0), L(1, 1, 0), TX);
		const float  X01 = FMath::Lerp(L(0, 0, 1), L(1, 0, 1), TX), X11 = FMath::Lerp(L(0, 1, 1), L(1, 1, 1), TX);
		return FMath::Lerp(FMath::Lerp(X00, X10, TY), FMath::Lerp(X01, X11, TY), TZ);
	}

	// 노이즈 벡터장의 회전(curl) — 발산이 없어 연기처럼 흐른다 (중심 차분)
	FVector3 CurlNoise(const FVector3& P)
	{
		constexpr float E = 0.5f;
		const auto      N = [](const FVector3& Q, uint32 C) { return ValueNoise(Q, C); };
		const float     dZdY = N(P + FVector3(0, E, 0), 2) - N(P - FVector3(0, E, 0), 2);
		const float     dYdZ = N(P + FVector3(0, 0, E), 1) - N(P - FVector3(0, 0, E), 1);
		const float     dXdZ = N(P + FVector3(0, 0, E), 0) - N(P - FVector3(0, 0, E), 0);
		const float     dZdX = N(P + FVector3(E, 0, 0), 2) - N(P - FVector3(E, 0, 0), 2);
		const float     dYdX = N(P + FVector3(E, 0, 0), 1) - N(P - FVector3(E, 0, 0), 1);
		const float     dXdY = N(P + FVector3(0, E, 0), 0) - N(P - FVector3(0, E, 0), 0);
		return FVector3(dZdY - dYdZ, dXdZ - dZdX, dYdX - dXdY) * (1.0f / (2.0f * E));
	}

	// 축 주위 원뿔 안의 균일한 방향 (난수 두 개)
	FVector3 ConeDirection(const FVector3& Axis, float ConeDegrees, float R0, float R1)
	{
		const float    CosMax   = std::cos(FMath::DegreesToRadians(FMath::Clamp(ConeDegrees, 0.0f, 180.0f)));
		const float    CosTheta = FMath::Lerp(1.0f, CosMax, R0);
		const float    SinTheta = std::sqrt(FMath::Max(0.0f, 1.0f - CosTheta * CosTheta));
		const float    Phi      = R1 * FMath::TwoPi;
		const FVector3 N        = Axis.LengthSquared() > FMath::SmallNumber ? Axis.GetNormalized() : FVector3::UpVector;
		const FVector3 Helper   = std::abs(N.Z) < 0.99f ? FVector3::UpVector : FVector3::ForwardVector;
		const FVector3 T        = FVector3::Cross(Helper, N).GetNormalized();
		const FVector3 B        = FVector3::Cross(N, T);
		return (N * CosTheta + T * (SinTheta * std::cos(Phi)) + B * (SinTheta * std::sin(Phi))).GetNormalized();
	}

	// 모양 안 위치 (로컬). R = 난수 4개
	FVector3 ShapePosition(int32 Shape, float Radius, const FVector3& BoxSize, float Height, float MinorRadius, bool bSurface, const FVector4& R)
	{
		switch (Shape)
		{
		case 1: // 구: 방향 균일 + 반지름 (부피 균일은 세제곱근)
		{
			const FVector3 Direction = ConeDirection(FVector3::UpVector, 180.0f, R.X, R.Y);
			return Direction * (Radius * (bSurface ? 1.0f : std::cbrt(R.Z)));
		}
		case 2: // 상자
		{
			FVector3 P((R.X * 2.0f - 1.0f) * BoxSize.X, (R.Y * 2.0f - 1.0f) * BoxSize.Y, (R.Z * 2.0f - 1.0f) * BoxSize.Z);
			if (bSurface) // 가장 가까운 면으로 밀어낸다
			{
				const FVector3 Ratio(std::abs(P.X) / FMath::Max(BoxSize.X, 1e-4f), std::abs(P.Y) / FMath::Max(BoxSize.Y, 1e-4f),
				                     std::abs(P.Z) / FMath::Max(BoxSize.Z, 1e-4f));
				if (Ratio.X >= Ratio.Y && Ratio.X >= Ratio.Z) P.X = std::copysign(BoxSize.X, P.X);
				else if (Ratio.Y >= Ratio.Z) P.Y = std::copysign(BoxSize.Y, P.Y);
				else P.Z = std::copysign(BoxSize.Z, P.Z);
			}
			return P;
		}
		case 3: // 원기둥 (Z축, 바닥 0 ~ 높이)
		{
			const float Angle = R.X * FMath::TwoPi;
			const float Dist  = Radius * (bSurface ? 1.0f : std::sqrt(R.Y));
			return FVector3(std::cos(Angle) * Dist, std::sin(Angle) * Dist, R.Z * Height);
		}
		case 4: // 원뿔 (꼭짓점 0, 높이에서 반지름)
		{
			const float T     = bSurface ? R.Z : std::cbrt(R.Z);
			const float Angle = R.X * FMath::TwoPi;
			const float Dist  = Radius * T * (bSurface ? 1.0f : std::sqrt(R.Y));
			return FVector3(std::cos(Angle) * Dist, std::sin(Angle) * Dist, T * Height);
		}
		case 5: // 원환 (XY 평면)
		{
			const float Major = R.X * FMath::TwoPi;
			const float Minor = R.Y * FMath::TwoPi;
			const float Tube  = MinorRadius * (bSurface ? 1.0f : std::sqrt(R.Z));
			const float Ring  = Radius + std::cos(Minor) * Tube;
			return FVector3(std::cos(Major) * Ring, std::sin(Major) * Ring, std::sin(Minor) * Tube);
		}
		default:
			return FVector3::ZeroVector;
		}
	}

	// ---- JSON --------------------------------------------------------------------------------------------------
	json ToJson(const FVector4& Value) { return json::array({ Value.X, Value.Y, Value.Z, Value.W }); }
	FVector4 ReadVector4(const json& Node, const FVector4& Fallback)
	{
		if (!Node.is_array() || Node.empty())
		{
			return Fallback;
		}
		FVector4 Result = Fallback;
		for (size_t Index = 0; Index < 4 && Index < Node.size(); ++Index)
		{
			if (Node[Index].is_number())
			{
				Result[static_cast<int32>(Index)] = Node[Index].get<float>();
			}
		}
		return Result;
	}

	json ValueToJson(const FParticleValue& Value)
	{
		json Node;
		Node["Mode"] = static_cast<int32>(Value.Mode);
		Node["A"]    = ToJson(Value.A);
		if (Value.Mode == EParticleValueMode::Random)
		{
			Node["B"] = ToJson(Value.B);
		}
		if (Value.Mode == EParticleValueMode::Curve)
		{
			json Keys = json::array();
			for (const FParticleCurveKey& Key : Value.Curve)
			{
				Keys.push_back({ { "T", Key.Time }, { "V", ToJson(Key.Value) } });
			}
			Node["Curve"] = Keys;
		}
		return Node;
	}

	FParticleValue ValueFromJson(const json& Node, const FVector4& Default)
	{
		FParticleValue Value = FParticleValue::Constant(Default);
		if (!Node.is_object())
		{
			return Value;
		}
		Value.Mode = static_cast<EParticleValueMode>(std::clamp(Node.value("Mode", 0), 0, 2));
		Value.A    = ReadVector4(Node.value("A", json()), Default);
		Value.B    = ReadVector4(Node.value("B", json()), Value.A);
		if (const auto It = Node.find("Curve"); It != Node.end() && It->is_array())
		{
			for (const json& Key : *It)
			{
				Value.Curve.push_back({ Key.value("T", 0.0f), ReadVector4(Key.value("V", json()), Default) });
			}
			std::sort(Value.Curve.begin(), Value.Curve.end(), [](const auto& L, const auto& R) { return L.Time < R.Time; });
		}
		if (Value.Mode == EParticleValueMode::Curve && Value.Curve.empty())
		{
			Value.Mode = EParticleValueMode::Constant;
		}
		return Value;
	}

	const char* StageKey(EParticleStage Stage)
	{
		switch (Stage)
		{
		case EParticleStage::EmitterUpdate:  return "EmitterUpdate";
		case EParticleStage::ParticleSpawn:  return "ParticleSpawn";
		default:                             return "ParticleUpdate";
		}
	}

	// 이전 형식(버전 없음, 단일 이미터 평면 설정) → 새 구조
	FParticleEmitter ConvertLegacy(const json& D)
	{
		const auto F  = [&](const char* Key, float Fallback) { return D.value(Key, Fallback); };
		const auto V3 = [&](const char* Key, const FVector4& Fallback) { return ReadVector4(D.value(Key, json()), Fallback); };

		FParticleEmitter Emitter;
		Emitter.Name         = D.value("Name", std::string("Emitter"));
		Emitter.Duration     = F("Duration", 2.0f);
		Emitter.bLoop        = D.value("Loop", true);
		Emitter.MaxParticles = D.value("MaxParticles", 500u);
		Emitter.Seed         = D.value("Seed", 1u);

		auto& EmitterStage = Emitter.GetStage(EParticleStage::EmitterUpdate);
		if (F("SpawnRate", 20.0f) > 0.0f)
		{
			FParticleModule Rate = FParticleModule::Make(EParticleModuleType::SpawnRate);
			Rate.Inputs[0]       = FParticleValue::Constant(V(F("SpawnRate", 20.0f)));
			EmitterStage.push_back(Rate);
		}
		if (D.value("BurstCount", 0u) > 0)
		{
			FParticleModule Burst = FParticleModule::Make(EParticleModuleType::SpawnBurst);
			Burst.Inputs[0]       = FParticleValue::Constant(V(static_cast<float>(D.value("BurstCount", 0u))));
			EmitterStage.push_back(Burst);
		}

		auto&           Spawn = Emitter.GetStage(EParticleStage::ParticleSpawn);
		FParticleModule Init  = FParticleModule::Make(EParticleModuleType::InitializeParticle);
		Init.Inputs[0]        = FParticleValue::Range(V(F("LifetimeMin", 1.0f)), V(F("LifetimeMax", 2.0f)));
		Init.Inputs[1]        = FParticleValue::Constant(V(1, 1, 1, 1));
		Init.Inputs[2]        = FParticleValue::Constant(V(F("SizeStart", 20.0f), F("SizeStart", 20.0f)));
		Init.Inputs[3]        = D.value("RandomRotation", true) ? FParticleValue::Range(V(0), V(360)) : FParticleValue::Constant(V(0));
		Spawn.push_back(Init);

		FParticleModule Shape = FParticleModule::Make(EParticleModuleType::ShapeLocation);
		const int32     OldShape = D.value("Shape", 0);
		Shape.Inputs[0]          = FParticleValue::Constant(V(OldShape == 1 ? 1.0f : (OldShape == 2 ? 2.0f : 0.0f)));
		Shape.Inputs[1]          = FParticleValue::Constant(V(F("ShapeRadius", 20.0f)));
		Shape.Inputs[2]          = FParticleValue::Constant(V3("ShapeExtent", V(50, 50, 50)));
		Spawn.push_back(Shape);

		FParticleModule Cone = FParticleModule::Make(EParticleModuleType::AddVelocityInCone);
		Cone.Inputs[0]       = FParticleValue::Constant(V3("Direction", V(0, 0, 1)));
		Cone.Inputs[1]       = FParticleValue::Constant(V(F("ConeAngle", 20.0f)));
		Cone.Inputs[2]       = FParticleValue::Range(V(F("SpeedMin", 100.0f)), V(F("SpeedMax", 200.0f)));
		Spawn.push_back(Cone);

		auto&          Update       = Emitter.GetStage(EParticleStage::ParticleUpdate);
		const FVector4 Acceleration = V3("Acceleration", V(0, 0, 0));
		if (Acceleration.X != 0.0f || Acceleration.Y != 0.0f || Acceleration.Z != 0.0f)
		{
			FParticleModule Accel = FParticleModule::Make(EParticleModuleType::AccelerationForce);
			Accel.Inputs[0]       = FParticleValue::Constant(Acceleration);
			Update.push_back(Accel);
		}
		if (F("Drag", 0.0f) > 0.0f)
		{
			FParticleModule Drag = FParticleModule::Make(EParticleModuleType::Drag);
			Drag.Inputs[0]       = FParticleValue::Constant(V(F("Drag", 0.0f)));
			Update.push_back(Drag);
		}
		if (F("RotationSpeedMin", 0.0f) != 0.0f || F("RotationSpeedMax", 0.0f) != 0.0f)
		{
			FParticleModule Rotation = FParticleModule::Make(EParticleModuleType::SpriteRotationRate);
			Rotation.Inputs[0]       = FParticleValue::Range(V(F("RotationSpeedMin", 0.0f)), V(F("RotationSpeedMax", 0.0f)));
			Update.push_back(Rotation);
		}
		// 시작 → 끝 색/크기: 초기화는 시작값, 변화 모듈은 1 → 끝/시작 비율
		const FVector4  ColorStart = V3("ColorStart", V(1, 1, 1, 1));
		const FVector4  ColorEnd   = V3("ColorEnd", V(1, 1, 1, 0));
		FParticleModule ScaleColor = FParticleModule::Make(EParticleModuleType::ScaleColor);
		Init.Inputs[1]             = FParticleValue::Constant(ColorStart);
		Spawn[0]                   = Init;
		const auto Ratio           = [](float End, float Start) { return std::abs(Start) > 1e-5f ? End / Start : 0.0f; };
		ScaleColor.Inputs[0]       = FParticleValue::MakeCurve({ { 0.0f, V(1, 1, 1, 1) },
		                                                         { 1.0f, V(Ratio(ColorEnd.X, ColorStart.X), Ratio(ColorEnd.Y, ColorStart.Y), Ratio(ColorEnd.Z, ColorStart.Z),
		                                                                   Ratio(ColorEnd.W, ColorStart.W)) } });
		Update.push_back(ScaleColor);
		const float     SizeRatio = Ratio(F("SizeEnd", 10.0f), F("SizeStart", 20.0f));
		FParticleModule ScaleSize = FParticleModule::Make(EParticleModuleType::ScaleSpriteSize);
		ScaleSize.Inputs[0]       = FParticleValue::MakeCurve({ { 0.0f, V(1, 1) }, { 1.0f, V(SizeRatio, SizeRatio) } });
		Update.push_back(ScaleSize);

		FParticleRendererSettings Sprite;
		Sprite.BlendMode   = static_cast<EParticleBlendMode>(std::clamp(D.value("BlendMode", 1), 0, 1));
		Sprite.TexturePath = D.value("Texture", std::string());
		Emitter.Renderers.push_back(Sprite);
		return Emitter;
	}
} // namespace

// ---- 동적 입력 -------------------------------------------------------------------------------------------------

FParticleValue FParticleValue::Constant(const FVector4& Value)
{
	FParticleValue Result;
	Result.A = Result.B = Value;
	return Result;
}

FParticleValue FParticleValue::Range(const FVector4& Min, const FVector4& Max)
{
	FParticleValue Result;
	Result.Mode = EParticleValueMode::Random;
	Result.A    = Min;
	Result.B    = Max;
	return Result;
}

FParticleValue FParticleValue::MakeCurve(std::vector<FParticleCurveKey> Keys)
{
	FParticleValue Result;
	Result.Mode  = EParticleValueMode::Curve;
	Result.Curve = std::move(Keys);
	std::sort(Result.Curve.begin(), Result.Curve.end(), [](const auto& L, const auto& R) { return L.Time < R.Time; });
	Result.A = Result.B = Result.Curve.empty() ? FVector4() : Result.Curve.front().Value;
	return Result;
}

FVector4 FParticleValue::SampleCurve(float Alpha) const
{
	if (Curve.empty())
	{
		return A;
	}
	if (Alpha <= Curve.front().Time)
	{
		return Curve.front().Value;
	}
	for (size_t Index = 1; Index < Curve.size(); ++Index)
	{
		if (Alpha <= Curve[Index].Time)
		{
			const FParticleCurveKey& L = Curve[Index - 1];
			const FParticleCurveKey& R = Curve[Index];
			const float              T = (Alpha - L.Time) / FMath::Max(R.Time - L.Time, 1.0e-6f);
			return FMath::Lerp(L.Value, R.Value, T);
		}
	}
	return Curve.back().Value;
}

FVector4 FParticleValue::Evaluate(const FVector4& Random01, float Alpha) const
{
	switch (Mode)
	{
	case EParticleValueMode::Random: return A + (B - A) * Random01;
	case EParticleValueMode::Curve:  return SampleCurve(FMath::Clamp(Alpha, 0.0f, 1.0f));
	default:                         return A;
	}
}

// ---- 모듈 / 이미터 / 시스템 ------------------------------------------------------------------------------------

std::span<const FParticleModuleInfo> GetParticleModuleInfos()
{
	return GModuleInfos;
}

const FParticleModuleInfo* FindParticleModuleInfo(std::string_view Id)
{
	for (const FParticleModuleInfo& Info : GModuleInfos)
	{
		if (Id == Info.Id)
		{
			return &Info;
		}
	}
	return nullptr;
}

const FParticleModuleInfo& FParticleModule::GetInfo() const
{
	return GModuleInfos[static_cast<size_t>(Type)];
}

FParticleModule FParticleModule::Make(EParticleModuleType Type)
{
	FParticleModule Module;
	Module.Type = Type;
	for (const FParticleInputInfo& Input : Module.GetInfo().Inputs)
	{
		Module.Inputs.push_back(FParticleValue::Constant(Input.Default));
	}
	// 곡선이 기본인 모듈: 수명 동안 사라지거나 커지게
	if (Type == EParticleModuleType::ScaleColor)
	{
		Module.Inputs[0] = FParticleValue::MakeCurve({ { 0.0f, V(1, 1, 1, 0) }, { 0.1f, V(1, 1, 1, 1) }, { 1.0f, V(1, 1, 1, 0) } });
	}
	else if (Type == EParticleModuleType::ScaleSpriteSize)
	{
		Module.Inputs[0] = FParticleValue::MakeCurve({ { 0.0f, V(0.5f, 0.5f) }, { 1.0f, V(1.5f, 1.5f) } });
	}
	else if (Type == EParticleModuleType::InitializeParticle)
	{
		Module.Inputs[0] = FParticleValue::Range(V(1.0f), V(2.0f));
		Module.Inputs[3] = FParticleValue::Range(V(0.0f), V(360.0f));
	}
	else if (Type == EParticleModuleType::AddVelocityInCone)
	{
		Module.Inputs[2] = FParticleValue::Range(V(100.0f), V(200.0f));
	}
	return Module;
}

const FParticleModule* FParticleEmitter::FindModule(EParticleModuleType Type) const
{
	for (const auto& Stage : Stages)
	{
		for (const FParticleModule& Module : Stage)
		{
			if (Module.Type == Type && Module.bEnabled)
			{
				return &Module;
			}
		}
	}
	return nullptr;
}

FParticleEmitter FParticleEmitter::MakeDefault()
{
	FParticleEmitter Emitter;
	Emitter.GetStage(EParticleStage::EmitterUpdate).push_back(FParticleModule::Make(EParticleModuleType::SpawnRate));
	Emitter.GetStage(EParticleStage::ParticleSpawn).push_back(FParticleModule::Make(EParticleModuleType::InitializeParticle));
	Emitter.GetStage(EParticleStage::ParticleSpawn).push_back(FParticleModule::Make(EParticleModuleType::ShapeLocation));
	Emitter.GetStage(EParticleStage::ParticleSpawn).push_back(FParticleModule::Make(EParticleModuleType::AddVelocityInCone));
	Emitter.GetStage(EParticleStage::ParticleUpdate).push_back(FParticleModule::Make(EParticleModuleType::Drag));
	Emitter.GetStage(EParticleStage::ParticleUpdate).push_back(FParticleModule::Make(EParticleModuleType::ScaleColor));
	Emitter.GetStage(EParticleStage::ParticleUpdate).push_back(FParticleModule::Make(EParticleModuleType::ScaleSpriteSize));
	Emitter.Renderers.push_back(FParticleRendererSettings{});
	return Emitter;
}

FParticleSystemAsset FParticleSystemAsset::MakeDefault(const std::string& InName)
{
	FParticleSystemAsset Asset;
	Asset.Name = InName;
	Asset.Emitters.push_back(FParticleEmitter::MakeDefault());
	return Asset;
}

std::string FParticleSystemAsset::ToJsonString() const
{
	json Document;
	Document["Version"] = Version;
	Document["Name"]    = Name;
	json EmitterArray   = json::array();
	for (const FParticleEmitter& Emitter : Emitters)
	{
		json Node;
		Node["Name"]         = Emitter.Name;
		Node["Enabled"]      = Emitter.bEnabled;
		Node["SimTarget"]    = Emitter.SimTarget == EParticleSimTarget::GPU ? "GPU" : "CPU";
		Node["LocalSpace"]   = Emitter.bLocalSpace;
		Node["Duration"]     = Emitter.Duration;
		Node["Loop"]         = Emitter.bLoop;
		Node["MaxParticles"] = Emitter.MaxParticles;
		Node["Seed"]         = Emitter.Seed;
		if (Emitter.bFixedBounds)
		{
			const FVector3& Min = Emitter.FixedBoundsMin;
			const FVector3& Max = Emitter.FixedBoundsMax;
			Node["FixedBounds"] = { { "Min", json::array({ Min.X, Min.Y, Min.Z }) }, { "Max", json::array({ Max.X, Max.Y, Max.Z }) } };
		}
		for (size_t Stage = 0; Stage < static_cast<size_t>(EParticleStage::Count); ++Stage)
		{
			json Modules = json::array();
			for (const FParticleModule& Module : Emitter.Stages[Stage])
			{
				const FParticleModuleInfo& Info = Module.GetInfo();
				json                       ModuleNode;
				ModuleNode["Module"]  = Info.Id;
				ModuleNode["Enabled"] = Module.bEnabled;
				json Inputs;
				for (size_t Input = 0; Input < Info.Inputs.size() && Input < Module.Inputs.size(); ++Input)
				{
					Inputs[Info.Inputs[Input].Id] = ValueToJson(Module.Inputs[Input]);
				}
				ModuleNode["Inputs"] = Inputs;
				Modules.push_back(ModuleNode);
			}
			Node[StageKey(static_cast<EParticleStage>(Stage))] = Modules;
		}
		json Renderers = json::array();
		for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
		{
			json R;
			R["Type"]            = static_cast<int32>(Renderer.Type);
			R["Enabled"]         = Renderer.bEnabled;
			R["BlendMode"]       = static_cast<int32>(Renderer.BlendMode);
			R["Texture"]         = Renderer.TexturePath;
			R["SubImageColumns"] = Renderer.SubImageColumns;
			R["SubImageRows"]    = Renderer.SubImageRows;
			R["Alignment"]       = static_cast<int32>(Renderer.Alignment);
			R["VelocityStretch"] = Renderer.VelocityStretch;
			R["Mesh"]            = Renderer.MeshAsset;
			R["RibbonWidth"]     = Renderer.RibbonWidthScale;
			Renderers.push_back(R);
		}
		Node["Renderers"] = Renderers;
		EmitterArray.push_back(Node);
	}
	Document["Emitters"] = EmitterArray;
	return Document.dump(2);
}

bool FParticleSystemAsset::FromJsonString(const std::string& JsonText)
{
	const json Document = json::parse(JsonText, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogScene, Error, "파티클 JSON 파싱 실패");
		return false;
	}

	// 런타임 핸들은 같은 자리의 렌더러에서 유지 (리소스 관리자가 다시 해석할 때까지)
	std::vector<std::vector<std::pair<FTextureHandle, FMeshHandle>>> KeptHandles;
	for (const FParticleEmitter& Emitter : Emitters)
	{
		auto& List = KeptHandles.emplace_back();
		for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
		{
			List.emplace_back(Renderer.Texture, Renderer.Mesh);
		}
	}

	Emitters.clear();
	Name = Document.value("Name", std::string());
	if (Document.value("Version", 0) < 2)
	{
		Emitters.push_back(ConvertLegacy(Document)); // 이전 단일 이미터 형식
	}
	else if (const auto It = Document.find("Emitters"); It != Document.end() && It->is_array())
	{
		for (const json& Node : *It)
		{
			FParticleEmitter Emitter;
			Emitter.Name         = Node.value("Name", std::string("Emitter"));
			Emitter.bEnabled     = Node.value("Enabled", true);
			Emitter.SimTarget    = Node.value("SimTarget", std::string("CPU")) == "GPU" ? EParticleSimTarget::GPU : EParticleSimTarget::CPU;
			Emitter.bLocalSpace  = Node.value("LocalSpace", false);
			Emitter.Duration     = Node.value("Duration", 2.0f);
			Emitter.bLoop        = Node.value("Loop", true);
			Emitter.MaxParticles = std::clamp(Node.value("MaxParticles", 1000u), 1u, 1000000u);
			Emitter.Seed         = Node.value("Seed", 1u);
			if (const auto Bounds = Node.find("FixedBounds"); Bounds != Node.end() && Bounds->is_object())
			{
				const FVector4 Min     = ReadVector4(Bounds->value("Min", json()), FVector4(-200.0f, -200.0f, -200.0f, 0.0f));
				const FVector4 Max     = ReadVector4(Bounds->value("Max", json()), FVector4(200.0f, 200.0f, 200.0f, 0.0f));
				Emitter.bFixedBounds   = true;
				Emitter.FixedBoundsMin = FVector3(Min.X, Min.Y, Min.Z);
				Emitter.FixedBoundsMax = FVector3(Max.X, Max.Y, Max.Z);
			}
			for (size_t Stage = 0; Stage < static_cast<size_t>(EParticleStage::Count); ++Stage)
			{
				const auto Modules = Node.find(StageKey(static_cast<EParticleStage>(Stage)));
				if (Modules == Node.end() || !Modules->is_array())
				{
					continue;
				}
				for (const json& ModuleNode : *Modules)
				{
					const FParticleModuleInfo* Info = FindParticleModuleInfo(ModuleNode.value("Module", std::string()));
					if (Info == nullptr)
					{
						E_LOG(LogScene, Warning, "알 수 없는 파티클 모듈을 건너뜁니다: {}", ModuleNode.value("Module", std::string()));
						continue;
					}
					FParticleModule Module = FParticleModule::Make(Info->Type);
					Module.bEnabled        = ModuleNode.value("Enabled", true);
					const json Inputs      = ModuleNode.value("Inputs", json::object());
					for (size_t Input = 0; Input < Info->Inputs.size(); ++Input)
					{
						if (const auto Found = Inputs.find(Info->Inputs[Input].Id); Found != Inputs.end())
						{
							Module.Inputs[Input] = ValueFromJson(*Found, Info->Inputs[Input].Default);
						}
					}
					Emitter.Stages[Stage].push_back(std::move(Module));
				}
			}
			if (const auto Renderers = Node.find("Renderers"); Renderers != Node.end() && Renderers->is_array())
			{
				for (const json& R : *Renderers)
				{
					FParticleRendererSettings Renderer;
					Renderer.Type             = static_cast<EParticleRendererType>(std::clamp(R.value("Type", 0), 0, 2));
					Renderer.bEnabled         = R.value("Enabled", true);
					Renderer.BlendMode        = static_cast<EParticleBlendMode>(std::clamp(R.value("BlendMode", 1), 0, 1));
					Renderer.TexturePath      = R.value("Texture", std::string());
					Renderer.SubImageColumns  = std::max(1, R.value("SubImageColumns", 1));
					Renderer.SubImageRows     = std::max(1, R.value("SubImageRows", 1));
					Renderer.Alignment        = static_cast<EParticleSpriteAlignment>(std::clamp(R.value("Alignment", 0), 0, 1));
					Renderer.VelocityStretch  = R.value("VelocityStretch", Renderer.VelocityStretch);
					Renderer.MeshAsset        = R.value("Mesh", Renderer.MeshAsset);
					Renderer.RibbonWidthScale = R.value("RibbonWidth", 1.0f);
					Emitter.Renderers.push_back(std::move(Renderer));
				}
			}
			Emitters.push_back(std::move(Emitter));
		}
	}

	for (size_t E = 0; E < Emitters.size() && E < KeptHandles.size(); ++E)
	{
		for (size_t R = 0; R < Emitters[E].Renderers.size() && R < KeptHandles[E].size(); ++R)
		{
			Emitters[E].Renderers[R].Texture = KeptHandles[E][R].first;
			Emitters[E].Renderers[R].Mesh    = KeptHandles[E][R].second;
		}
	}
	return true;
}

bool FParticleSystemAsset::LoadFromFile(const std::filesystem::path& Path)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		E_LOG(LogScene, Error, "파티클 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	if (!FromJsonString(Text))
	{
		return false;
	}
	if (Name.empty())
	{
		Name = FStringConv::ToUtf8(Path.stem().wstring());
	}
	return true;
}

bool FParticleSystemAsset::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogScene, Error, "파티클 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString();
	return true;
}

// ---- 시뮬레이션 ------------------------------------------------------------------------------------------------

uint32 FParticleSimulation::Hash(uint32 Value)
{
	// PCG 해시 (셰이더와 같은 식)
	const uint32 State = Value * 747796405u + 2891336453u;
	const uint32 Word  = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
	return (Word >> 22u) ^ Word;
}

float FParticleSimulation::Random01(uint32 SpawnIndex, uint32 Seed, uint32 Stream)
{
	return HashToFloat(Hash(SpawnIndex ^ Hash(Seed + Stream * 0x9E3779B9u)));
}

void FParticleRuntime::Restart()
{
	Emitters.clear();
}

uint32 FParticleRuntime::CountAlive() const
{
	uint32 Count = 0;
	for (const FParticleEmitterInstance& Instance : Emitters)
	{
		Count += static_cast<uint32>(Instance.Particles.size()) + Instance.GpuEstimatedAlive;
	}
	return Count;
}

namespace
{
	// 입력 평가 도우미: 모듈 순번/입력 순번으로 난수 흐름을 나눠 CPU·GPU가 같은 값을 뽑게 한다
	FVector4 EvaluateInput(const FParticleModule& Module, size_t ModuleIndex, size_t InputIndex, uint32 SpawnIndex, uint32 Seed, float Alpha)
	{
		const FParticleValue& Value = Module.Inputs[InputIndex];
		const FVector4        R     = Value.Mode == EParticleValueMode::Random ? Random4(SpawnIndex, Seed, static_cast<uint32>(ModuleIndex * 16 + InputIndex)) : FVector4();
		return Value.Evaluate(R, Alpha);
	}

	void SpawnParticle(const FParticleEmitter& Emitter, FParticleEmitterInstance& Instance, const FMatrix4x4& EmitterWorld, float EmitterAlpha)
	{
		FParticle Particle;
		Particle.SpawnIndex = Instance.SpawnCounter++;
		const uint32 Seed   = Emitter.Seed;
		FVector3     LocalPosition;
		FVector3     LocalVelocity;

		const auto& Modules = Emitter.GetStage(EParticleStage::ParticleSpawn);
		for (size_t Index = 0; Index < Modules.size(); ++Index)
		{
			const FParticleModule& M = Modules[Index];
			if (!M.bEnabled)
			{
				continue;
			}
			const auto In = [&](size_t Input) { return EvaluateInput(M, Index, Input, Particle.SpawnIndex, Seed, EmitterAlpha); };
			switch (M.Type)
			{
			case EParticleModuleType::InitializeParticle:
			{
				Particle.Lifetime = FMath::Max(In(0).X, 0.01f);
				Particle.BaseColor = Particle.Color = In(1);
				const FVector4 Size = In(2);
				Particle.BaseSize = Particle.Size = FVector2(Size.X, Size.Y);
				Particle.Rotation = In(3).X;
				Particle.Mass     = FMath::Max(In(4).X, 0.001f);
				break;
			}
			case EParticleModuleType::ShapeLocation:
			{
				const FVector4 ShapeRandom = Random4(Particle.SpawnIndex, Seed, static_cast<uint32>(Index * 16 + 15));
				LocalPosition += ShapePosition(static_cast<int32>(In(0).X), In(1).X, FVector3(In(2).X, In(2).Y, In(2).Z), In(3).X, In(4).X, In(5).X != 0.0f,
				                               ShapeRandom) + FVector3(In(6).X, In(6).Y, In(6).Z);
				break;
			}
			case EParticleModuleType::AddVelocity:
				LocalVelocity += FVector3(In(0).X, In(0).Y, In(0).Z);
				break;
			case EParticleModuleType::AddVelocityInCone:
			{
				const FVector4 R = Random4(Particle.SpawnIndex, Seed, static_cast<uint32>(Index * 16 + 15));
				LocalVelocity += ConeDirection(FVector3(In(0).X, In(0).Y, In(0).Z), In(1).X, R.X, R.Y) * In(2).X;
				break;
			}
			case EParticleModuleType::AddVelocityFromPoint:
			{
				const FVector3 Offset = LocalPosition - FVector3(In(0).X, In(0).Y, In(0).Z);
				const FVector4 R      = Random4(Particle.SpawnIndex, Seed, static_cast<uint32>(Index * 16 + 15));
				const FVector3 Dir    = Offset.LengthSquared() > 1e-6f ? Offset.GetNormalized() : ConeDirection(FVector3::UpVector, 180.0f, R.X, R.Y);
				LocalVelocity += Dir * In(1).X;
				break;
			}
			default:
				break;
			}
		}

		if (Emitter.bLocalSpace)
		{
			Particle.Position = LocalPosition;
			Particle.Velocity = LocalVelocity;
		}
		else
		{
			Particle.Position = EmitterWorld.TransformPosition(LocalPosition);
			Particle.Velocity = EmitterWorld.TransformVector(LocalVelocity);
		}
		Instance.Particles.push_back(Particle);
	}

	// 입자 갱신: 모듈이 힘/감속을 모으고, 끝에서 속도·위치 적분(Solve Forces and Velocity), 충돌은 적분 뒤
	bool UpdateParticle(const FParticleEmitter& Emitter, FParticle& P, const FMatrix4x4& EmitterWorld, float Time, float Dt)
	{
		P.Age += Dt;
		if (P.Age >= P.Lifetime)
		{
			return false;
		}
		const float Alpha = P.Age / P.Lifetime;
		const uint32 Seed = Emitter.Seed;
		// 로컬 기준 입력(점/중심)은 월드 공간 이미터면 월드로 옮겨 쓴다
		const auto ToSim = [&](const FVector3& Local) { return Emitter.bLocalSpace ? Local : EmitterWorld.TransformPosition(Local); };
		const auto AxisToSim = [&](const FVector3& Local) {
			const FVector3 V = Emitter.bLocalSpace ? Local : EmitterWorld.TransformVector(Local);
			return V.LengthSquared() > 1e-8f ? V.GetNormalized() : FVector3::UpVector;
		};

		FVector3 Acceleration;
		float    DragAmount = 0.0f;
		const FParticleModule* Collision = nullptr;
		size_t   CollisionIndex = 0;
		const auto& Modules = Emitter.GetStage(EParticleStage::ParticleUpdate);
		for (size_t Index = 0; Index < Modules.size(); ++Index)
		{
			const FParticleModule& M = Modules[Index];
			if (!M.bEnabled)
			{
				continue;
			}
			const auto In = [&](size_t Input) { return EvaluateInput(M, Index, Input, P.SpawnIndex, Seed, Alpha); };
			switch (M.Type)
			{
			case EParticleModuleType::GravityForce:
			case EParticleModuleType::AccelerationForce:
				Acceleration += FVector3(In(0).X, In(0).Y, In(0).Z);
				break;
			case EParticleModuleType::Drag:
				DragAmount += FMath::Max(In(0).X, 0.0f);
				break;
			case EParticleModuleType::CurlNoiseForce:
			{
				const float    Frequency = In(1).X;
				const FVector3 Pan       = FVector3(In(2).X, In(2).Y, In(2).Z) * Time;
				Acceleration += CurlNoise((P.Position - Pan) * Frequency) * In(0).X;
				break;
			}
			case EParticleModuleType::VortexForce:
			{
				const FVector3 Axis    = AxisToSim(FVector3(In(0).X, In(0).Y, In(0).Z));
				const FVector3 Offset  = P.Position - ToSim(FVector3(In(1).X, In(1).Y, In(1).Z));
				const FVector3 Radial  = Offset - Axis * FVector3::Dot(Offset, Axis);
				const FVector3 Tangent = FVector3::Cross(Axis, Radial);
				if (Radial.LengthSquared() > 1e-6f)
				{
					Acceleration += Tangent.GetNormalized() * In(2).X - Radial.GetNormalized() * In(3).X;
				}
				break;
			}
			case EParticleModuleType::PointAttractionForce:
			{
				const FVector3 ToPoint = ToSim(FVector3(In(0).X, In(0).Y, In(0).Z)) - P.Position;
				const float    Dist    = ToPoint.Length();
				if (In(3).X > 0.0f && Dist < In(3).X)
				{
					return false;
				}
				const float Radius = FMath::Max(In(2).X, 1.0f);
				if (Dist > 1e-3f && Dist < Radius)
				{
					Acceleration += ToPoint / Dist * (In(1).X * (1.0f - Dist / Radius));
				}
				break;
			}
			case EParticleModuleType::ScaleColor:
				P.Color = P.BaseColor * In(0);
				break;
			case EParticleModuleType::ScaleSpriteSize:
				P.Size = FVector2(P.BaseSize.X * In(0).X, P.BaseSize.Y * In(0).Y);
				break;
			case EParticleModuleType::SpriteRotationRate:
				P.Rotation += In(0).X * Dt;
				break;
			case EParticleModuleType::SubUVAnimation:
			{
				const int32 Mode   = static_cast<int32>(In(0).X);
				const float Frames = FMath::Max(In(1).X, 1.0f);
				if (Mode == 0) P.SubImage = Alpha * Frames;
				else if (Mode == 1) P.SubImage = P.Age * In(2).X;
				else P.SubImage = std::floor(FParticleSimulation::Random01(P.SpawnIndex, Seed, static_cast<uint32>(Index * 64 + 7)) * Frames);
				break;
			}
			case EParticleModuleType::Collision:
				Collision      = &M;
				CollisionIndex = Index;
				break;
			default:
				break;
			}
		}

		P.Velocity = (P.Velocity + Acceleration * Dt) * (DragAmount > 0.0f ? std::exp(-DragAmount * Dt) : 1.0f);
		P.Position += P.Velocity * Dt;

		if (Collision != nullptr)
		{
			const auto  In     = [&](size_t Input) { return EvaluateInput(*Collision, CollisionIndex, Input, P.SpawnIndex, Seed, Alpha); };
			const float Height = In(0).X;
			const float Radius = P.Size.X * 0.5f;
			// 평면은 월드 Z 기준 (로컬 공간 이미터는 로컬 Z)
			if (P.Position.Z - Radius < Height && P.Velocity.Z < 0.0f)
			{
				if (In(3).X != 0.0f)
				{
					return false;
				}
				P.Position.Z = Height + Radius;
				const float Friction = FMath::Clamp(In(2).X, 0.0f, 1.0f);
				P.Velocity           = FVector3(P.Velocity.X * (1.0f - Friction), P.Velocity.Y * (1.0f - Friction), -P.Velocity.Z * FMath::Clamp(In(1).X, 0.0f, 1.0f));
			}
		}
		P.RotationRate = 0.0f;
		return true;
	}
} // namespace

void FParticleSimulation::Update(const FParticleEmitter& Emitter, FParticleEmitterInstance& Instance, const FMatrix4x4& EmitterWorld, float DeltaSeconds)
{
	const float Duration = FMath::Max(Emitter.Duration, 0.01f);
	const bool  bGpu     = Emitter.SimTarget == EParticleSimTarget::GPU;

	// 계산 방식을 바꾸면 반대쪽 상태는 버린다
	if (bGpu)
	{
		Instance.Particles.clear();
	}
	else if (Instance.GpuState || !Instance.PendingGpuSteps.empty())
	{
		Instance.GpuState.reset();
		Instance.PendingGpuSteps.clear();
		Instance.GpuEstimatedAlive = 0;
	}

	// 1) 기존 입자 (CPU)
	if (!bGpu)
	{
		const float Time = Instance.EmitterTime + static_cast<float>(Instance.LoopIndex) * Duration;
		for (size_t Index = 0; Index < Instance.Particles.size();)
		{
			if (!UpdateParticle(Emitter, Instance.Particles[Index], EmitterWorld, Time, DeltaSeconds))
			{
				Instance.Particles[Index] = Instance.Particles.back();
				Instance.Particles.pop_back();
				continue;
			}
			++Index;
		}
	}

	// 2) 이미터 갱신: 이번 프레임 생성 수 (곡선은 주기 안 시간 비율)
	uint32 SpawnCount = 0;
	if (!Instance.bFinished)
	{
		const auto& Modules = Emitter.GetStage(EParticleStage::EmitterUpdate);
		// 초당 생성: 프레임 전체 시간만큼 누적
		const float StartAlpha = FMath::Clamp(Instance.EmitterTime / Duration, 0.0f, 1.0f);
		for (size_t Index = 0; Index < Modules.size(); ++Index)
		{
			const FParticleModule& M = Modules[Index];
			if (M.bEnabled && M.Type == EParticleModuleType::SpawnRate)
			{
				Instance.SpawnAccumulator += FMath::Max(EvaluateInput(M, Index, 0, Instance.LoopIndex, Emitter.Seed, StartAlpha).X, 0.0f) * DeltaSeconds;
			}
		}
		const uint32 FromRate = static_cast<uint32>(Instance.SpawnAccumulator);
		Instance.SpawnAccumulator -= static_cast<float>(FromRate);
		SpawnCount += FromRate;

		// 버스트: 주기 경계를 넘는 프레임은 구간을 나눠 새 주기 시작(0초)의 버스트도 놓치지 않는다
		float Start     = Instance.EmitterTime;
		float Remaining = DeltaSeconds;
		for (int32 Segment = 0; Segment < 64; ++Segment)
		{
			const float End        = Start + Remaining;
			const float SegmentEnd = FMath::Min(End, Duration);
			const float Alpha      = FMath::Clamp(Start / Duration, 0.0f, 1.0f);
			for (size_t Index = 0; Index < Modules.size() && Index < 32; ++Index)
			{
				const FParticleModule& M = Modules[Index];
				if (!M.bEnabled || M.Type != EParticleModuleType::SpawnBurst || (Instance.FiredBurstMask & (1u << Index)) != 0)
				{
					continue;
				}
				// 버스트 개수 무작위는 주기마다 새로 (난수 순번 = 주기 번호)
				const float BurstTime = EvaluateInput(M, Index, 1, Instance.LoopIndex, Emitter.Seed, Alpha).X;
				if (BurstTime < SegmentEnd || BurstTime <= Start)
				{
					SpawnCount += static_cast<uint32>(FMath::Max(EvaluateInput(M, Index, 0, Instance.LoopIndex, Emitter.Seed, Alpha).X, 0.0f));
					Instance.FiredBurstMask |= 1u << Index;
				}
			}
			if (End < Duration)
			{
				Instance.EmitterTime = End;
				break;
			}
			if (!Emitter.bLoop)
			{
				Instance.EmitterTime = Duration;
				Instance.bFinished   = true;
				break;
			}
			Remaining = End - Duration;
			Start     = 0.0f;
			++Instance.LoopIndex;
			Instance.FiredBurstMask = 0;
			Instance.EmitterTime    = Remaining;
		}
	}

	// 3) 생성
	const float SpawnAlpha = FMath::Clamp(Instance.EmitterTime / Duration, 0.0f, 1.0f);
	if (bGpu)
	{
		SpawnCount = std::min(SpawnCount, Emitter.MaxParticles);
		FParticleGpuStep Step;
		Step.DeltaSeconds = DeltaSeconds;
		Step.EmitterAlpha = SpawnAlpha;
		Step.SpawnStart   = Instance.SpawnCounter;
		Step.SpawnCount   = SpawnCount;
		Step.EmitterWorld = EmitterWorld;
		Instance.SpawnCounter += SpawnCount;
		// 렌더되지 않는 동안 쌓이지 않게 최근 몇 개만 (그 이상은 한 번에 합친다)
		if (Instance.PendingGpuSteps.size() >= 8)
		{
			FParticleGpuStep& Last = Instance.PendingGpuSteps.back();
			Last.DeltaSeconds += Step.DeltaSeconds;
			Last.SpawnCount = std::min(Last.SpawnCount + Step.SpawnCount, Emitter.MaxParticles);
			Last.EmitterWorld = Step.EmitterWorld;
		}
		else
		{
			Instance.PendingGpuSteps.push_back(Step);
		}
		// 표시용 추정: 초기화 모듈의 평균 수명 × 생성률
		const FParticleModule* Init        = Emitter.FindModule(EParticleModuleType::InitializeParticle);
		const float            AvgLifetime = Init ? (Init->Inputs[0].A.X + Init->Inputs[0].B.X) * 0.5f : 1.0f;
		const float            Rate        = DeltaSeconds > 0.0f ? static_cast<float>(SpawnCount) / DeltaSeconds : 0.0f;
		const float            Estimate    = FMath::Lerp(static_cast<float>(Instance.GpuEstimatedAlive), Rate * AvgLifetime, 0.05f);
		Instance.GpuEstimatedAlive         = static_cast<uint32>(FMath::Min(Estimate, static_cast<float>(Emitter.MaxParticles)));
		return;
	}
	for (uint32 Index = 0; Index < SpawnCount && Instance.Particles.size() < Emitter.MaxParticles; ++Index)
	{
		SpawnParticle(Emitter, Instance, EmitterWorld, SpawnAlpha);
	}
}

void FParticleSystem::Update(FScene& Scene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("파티클 갱신");
	Scene.GetRegistry().View<FTransformComponent, FParticleSystemComponent>().Each([&](FEntity, FTransformComponent& Transform, FParticleSystemComponent& Component) {
		FParticleRuntime& Runtime = Component.Runtime;
		if (!Runtime.System || !Component.bPlaying)
		{
			return;
		}
		const FParticleSystemAsset& System = *Runtime.System;
		// 편집기가 이미터를 추가/삭제하면 인스턴스 수를 맞춘다 (새 이미터는 처음부터)
		if (Runtime.Emitters.size() != System.Emitters.size())
		{
			Runtime.Emitters.resize(System.Emitters.size());
		}
		Runtime.LastWorld = Transform.WorldMatrix;
		const float Dt    = DeltaSeconds * Component.Speed;
		for (size_t Index = 0; Index < System.Emitters.size(); ++Index)
		{
			const FParticleEmitter& Emitter = System.Emitters[Index];
			if (Emitter.bEnabled)
			{
				FParticleSimulation::Update(Emitter, Runtime.Emitters[Index], Transform.WorldMatrix, Dt);
			}
			else
			{
				Runtime.Emitters[Index] = FParticleEmitterInstance{};
			}
		}
	});
}

void FParticleSystem::Restart(FScene& Scene, FEntity Entity)
{
	if (FParticleSystemComponent* Component = Scene.GetRegistry().TryGet<FParticleSystemComponent>(Entity))
	{
		Component->Runtime.Restart();
	}
}

uint32 FParticleSystem::CountParticles(FScene& Scene)
{
	uint32 Count = 0;
	Scene.GetRegistry().View<FParticleSystemComponent>().Each([&](FEntity, FParticleSystemComponent& Component) { Count += Component.Runtime.CountAlive(); });
	return Count;
}
