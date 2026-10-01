#include "Renderer/ParticleBounds.h"

#include "Scene/Particles.h"

namespace ParticleBounds
{
	namespace
	{
		FVector4 AbsVector(const FVector4& V)
		{
			return FVector4(FMath::Abs(V.X), FMath::Abs(V.Y), FMath::Abs(V.Z), FMath::Abs(V.W));
		}
		FVector4 MaxVector(const FVector4& A, const FVector4& B)
		{
			return FVector4(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y), FMath::Max(A.Z, B.Z), FMath::Max(A.W, B.W));
		}
		float Length3(const FVector4& V)
		{
			return FMath::Sqrt(V.X * V.X + V.Y * V.Y + V.Z * V.Z);
		}
		// 켜진 모듈 입력의 최대 절댓값 (모듈이 없으면 0)
		FVector4 ModuleInput(const FParticleModule& Module, size_t Input)
		{
			return Input < Module.Inputs.size() ? MaxAbs(Module.Inputs[Input]) : FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
	} // namespace

	FVector4 MaxAbs(const FParticleValue& Value)
	{
		switch (Value.Mode)
		{
		case EParticleValueMode::Random:
			return MaxVector(AbsVector(Value.A), AbsVector(Value.B));
		case EParticleValueMode::Curve:
		{
			FVector4 Result = Value.Curve.empty() ? AbsVector(Value.A) : FVector4(0.0f, 0.0f, 0.0f, 0.0f);
			for (const FParticleCurveKey& Key : Value.Curve)
			{
				Result = MaxVector(Result, AbsVector(Key.Value));
			}
			return Result;
		}
		default:
			return AbsVector(Value.A);
		}
	}

	float MaxLifetime(const FParticleEmitter& Emitter)
	{
		const FParticleModule* Init = Emitter.FindModule(EParticleModuleType::InitializeParticle);
		return Init != nullptr ? ModuleInput(*Init, 0).X : FParticle{}.Lifetime;
	}

	float RenderSizeFactor(const FParticleEmitter& Emitter, float MaxMeshRadius, float& OutStretchSeconds)
	{
		float Factor      = 0.0f;
		OutStretchSeconds = 0.0f;
		for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
		{
			if (!Renderer.bEnabled)
			{
				continue;
			}
			switch (Renderer.Type)
			{
			case EParticleRendererType::Mesh:
				Factor = FMath::Max(Factor, MaxMeshRadius / 100.0f);
				break;
			case EParticleRendererType::Ribbon:
				Factor = FMath::Max(Factor, FMath::Abs(Renderer.RibbonWidthScale) * 0.5f);
				break;
			default:
				Factor = FMath::Max(Factor, 0.70710678f);
				if (Renderer.Alignment == EParticleSpriteAlignment::Velocity)
				{
					OutStretchSeconds = FMath::Max(OutStretchSeconds, FMath::Abs(Renderer.VelocityStretch) * 0.5f);
				}
				break;
			}
		}
		return Factor;
	}

	float EstimateLocalRadius(const FParticleEmitter& Emitter, float MaxMeshRadius)
	{
		float SpawnRadius  = 0.0f;
		float Speed        = 0.0f;
		float Acceleration = 0.0f;
		float MaxSize      = FMath::Max(FParticle{}.BaseSize.X, FParticle{}.BaseSize.Y);
		float SizeScale    = 1.0f;
		float Lifetime     = FParticle{}.Lifetime;
		for (const auto& Stage : Emitter.Stages)
		{
			for (const FParticleModule& Module : Stage)
			{
				if (!Module.bEnabled)
				{
					continue;
				}
				switch (Module.Type)
				{
				case EParticleModuleType::InitializeParticle:
				{
					Lifetime              = ModuleInput(Module, 0).X;
					const FVector4 Size   = ModuleInput(Module, 2);
					MaxSize               = FMath::Max(Size.X, Size.Y);
					break;
				}
				case EParticleModuleType::ShapeLocation:
				{
					// 모양마다 다른 입력을 쓰지만 모두 더하면 어느 모양이든 포함한다 (보수적)
					const float Radius = ModuleInput(Module, 1).X + ModuleInput(Module, 4).X;
					const float Box    = Length3(ModuleInput(Module, 2));
					const float Height = ModuleInput(Module, 3).X;
					SpawnRadius        = FMath::Max(SpawnRadius, FMath::Max(Box, Radius + Height) + Length3(ModuleInput(Module, 6)));
					break;
				}
				case EParticleModuleType::AddVelocity:
					Speed += Length3(ModuleInput(Module, 0));
					break;
				case EParticleModuleType::AddVelocityInCone:
					Speed += ModuleInput(Module, 2).X;
					break;
				case EParticleModuleType::AddVelocityFromPoint:
					Speed += ModuleInput(Module, 1).X;
					break;
				case EParticleModuleType::GravityForce:
				case EParticleModuleType::AccelerationForce:
					Acceleration += Length3(ModuleInput(Module, 0));
					break;
				case EParticleModuleType::CurlNoiseForce:
					Acceleration += ModuleInput(Module, 0).X;
					break;
				case EParticleModuleType::VortexForce:
					Acceleration += ModuleInput(Module, 2).X + ModuleInput(Module, 3).X;
					break;
				case EParticleModuleType::PointAttractionForce:
					Acceleration += ModuleInput(Module, 1).X;
					break;
				case EParticleModuleType::ScaleSpriteSize:
				{
					const FVector4 Scale = ModuleInput(Module, 0);
					SizeScale            = FMath::Max(Scale.X, Scale.Y);
					break;
				}
				default:
					break;
				}
			}
		}
		const float MaxSpeed       = Speed + Acceleration * Lifetime;
		const float Travel         = Speed * Lifetime + 0.5f * Acceleration * Lifetime * Lifetime;
		float       StretchSeconds = 0.0f;
		const float SizeFactor     = RenderSizeFactor(Emitter, MaxMeshRadius, StretchSeconds);
		return SpawnRadius + Travel + MaxSize * SizeScale * SizeFactor + MaxSpeed * StretchSeconds;
	}

	float MaxAxisScale(const FMatrix4x4& M)
	{
		float MaxSquared = 0.0f;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			MaxSquared = FMath::Max(MaxSquared, M.M[Row][0] * M.M[Row][0] + M.M[Row][1] * M.M[Row][1] + M.M[Row][2] * M.M[Row][2]);
		}
		return FMath::Sqrt(MaxSquared);
	}

	void TrimDeferredSteps(std::vector<FParticleGpuStep>& Steps, std::vector<float>& EndTimes, float Lifetime, size_t MaxSteps, uint32 MaxParticles)
	{
		size_t Drop       = 0;
		float  KeptLength = 0.0f; // 뒤에서부터 남길 요청들의 시간 합
		for (size_t Step = Steps.size(); Step-- > 0;)
		{
			if (KeptLength >= Lifetime)
			{
				Drop = Step + 1;
				break;
			}
			KeptLength += Steps[Step].DeltaSeconds;
		}
		Steps.erase(Steps.begin(), Steps.begin() + static_cast<std::ptrdiff_t>(Drop));
		EndTimes.erase(EndTimes.begin(), EndTimes.begin() + static_cast<std::ptrdiff_t>(Drop));

		while (MaxSteps > 0 && Steps.size() > MaxSteps)
		{
			size_t Write = 0;
			for (size_t Read = 0; Read < Steps.size(); Read += 2, ++Write)
			{
				FParticleGpuStep Merged = Steps[Read];
				float            End    = EndTimes[Read];
				if (Read + 1 < Steps.size())
				{
					const FParticleGpuStep& Next = Steps[Read + 1];
					Merged.DeltaSeconds += Next.DeltaSeconds;
					Merged.SpawnCount   = FMath::Min(Merged.SpawnCount + Next.SpawnCount, MaxParticles);
					Merged.EmitterAlpha = Next.EmitterAlpha;
					Merged.EmitterWorld = Next.EmitterWorld;
					End                 = EndTimes[Read + 1];
				}
				Steps[Write]    = Merged;
				EndTimes[Write] = End;
			}
			Steps.resize(Write);
			EndTimes.resize(Write);
		}
	}

	FBox ComputeCpuBounds(const std::vector<FParticle>& Particles, const FMatrix4x4& LocalToWorld, float SizeFactor, float StretchSeconds)
	{
		FBox  Local;
		float MaxSize  = 0.0f;
		float MaxSpeed = 0.0f;
		for (const FParticle& Particle : Particles)
		{
			Local.AddPoint(Particle.Position);
			MaxSize  = FMath::Max(MaxSize, FMath::Max(FMath::Abs(Particle.Size.X), FMath::Abs(Particle.Size.Y)));
			MaxSpeed = FMath::Max(MaxSpeed, Particle.Velocity.LengthSquared());
		}
		if (!Local.IsValid())
		{
			return Local;
		}
		const float Scale  = MaxAxisScale(LocalToWorld);
		const FBox  World  = Local.TransformBy(LocalToWorld);
		const float Margin = (MaxSize * SizeFactor + FMath::Sqrt(MaxSpeed) * StretchSeconds) * Scale;
		return FBox(World.Min - FVector3(Margin), World.Max + FVector3(Margin));
	}
} // namespace ParticleBounds
