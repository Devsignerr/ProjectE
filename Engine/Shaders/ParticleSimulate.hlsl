#include "ParticleCommon.hlsli"

// GPU 파티클 계산 (이미터 하나, 한 단계). 입자 풀 = 고리 버퍼(Capacity).
//   생성 구간 [SpawnStart, SpawnStart + SpawnCount) mod Capacity의 스레드는 새 입자를 만들고, 나머지는 살아 있으면 갱신한다.
//   모듈 프로그램(Program)은 CPU가 이미터 설정을 굽는다: 모듈 머리 float4(종류, 입력 수, 단계 안 순번, 0) + 입력마다 float4 3개
//   (방식/키 수/키 위치, A, B). 곡선 키는 float4 2개(시간, 값). 식은 Scene/Particles.cpp와 같다.

cbuffer SimConstants : register(b0)
{
	float4x4 EmitterWorld;
	float    DeltaSeconds;
	float    EmitterAlpha;
	float    Time;
	uint     SpawnStart;
	uint     SpawnCount;
	uint     Capacity;
	uint     Seed;
	uint     bLocalSpace;
	uint     SpawnModuleCount;
	uint     SpawnOffset;
	uint     UpdateModuleCount;
	uint     UpdateOffset;
};

StructuredBuffer<float4>          Program   : register(t0);
RWStructuredBuffer<FParticleData> Particles : register(u0);

// EParticleModuleType 번호
#define MODULE_INITIALIZE_PARTICLE     2
#define MODULE_SHAPE_LOCATION          3
#define MODULE_ADD_VELOCITY            4
#define MODULE_ADD_VELOCITY_IN_CONE    5
#define MODULE_ADD_VELOCITY_FROM_POINT 6
#define MODULE_GRAVITY                 7
#define MODULE_DRAG                    8
#define MODULE_ACCELERATION            9
#define MODULE_CURL_NOISE              10
#define MODULE_VORTEX                  11
#define MODULE_POINT_ATTRACTION        12
#define MODULE_SCALE_COLOR             13
#define MODULE_SCALE_SPRITE_SIZE       14
#define MODULE_SPRITE_ROTATION_RATE    15
#define MODULE_SUBUV                   16
#define MODULE_COLLISION               17

float4 SampleCurve(uint KeyOffset, uint KeyCount, float Alpha)
{
	if (KeyCount == 0)
	{
		return float4(0, 0, 0, 0);
	}
	if (Alpha <= Program[KeyOffset].x)
	{
		return Program[KeyOffset + 1];
	}
	[loop] for (uint Key = 1; Key < KeyCount; ++Key)
	{
		const float RightTime = Program[KeyOffset + Key * 2].x;
		if (Alpha <= RightTime)
		{
			const float LeftTime = Program[KeyOffset + (Key - 1) * 2].x;
			const float T        = (Alpha - LeftTime) / max(RightTime - LeftTime, 1e-6f);
			return lerp(Program[KeyOffset + (Key - 1) * 2 + 1], Program[KeyOffset + Key * 2 + 1], T);
		}
	}
	return Program[KeyOffset + (KeyCount - 1) * 2 + 1];
}

float4 EvalInput(uint InputBase, uint ModuleIndex, uint InputIndex, uint SpawnIndex, float Alpha)
{
	const float4 Header = Program[InputBase + InputIndex * 3];
	const uint   Mode   = asuint(Header.x);
	const float4 A      = Program[InputBase + InputIndex * 3 + 1];
	if (Mode == 1)
	{
		const float4 B = Program[InputBase + InputIndex * 3 + 2];
		return A + (B - A) * ParticleRandom4(SpawnIndex, Seed, ModuleIndex * 16 + InputIndex);
	}
	if (Mode == 2)
	{
		return SampleCurve(asuint(Header.z), asuint(Header.y), saturate(Alpha));
	}
	return A;
}

float3 ToSim(float3 Local)
{
	return bLocalSpace != 0 ? Local : mul(float4(Local, 1.0f), EmitterWorld).xyz;
}

float3 AxisToSim(float3 Local)
{
	const float3 V = bLocalSpace != 0 ? Local : mul(float4(Local, 0.0f), EmitterWorld).xyz;
	return SafeNormalize(V, float3(0, 0, 1));
}

void SpawnParticle(inout FParticleData P, uint SpawnIndex)
{
	P = (FParticleData)0;
	P.BaseColor  = float4(1, 1, 1, 1);
	P.Color      = P.BaseColor;
	P.BaseSize   = float2(10, 10);
	P.Size       = P.BaseSize;
	P.Lifetime   = 1.0f;
	P.Mass       = 1.0f;
	P.SpawnIndex = SpawnIndex;

	float3 LocalPosition = float3(0, 0, 0);
	float3 LocalVelocity = float3(0, 0, 0);
	uint   Cursor        = SpawnOffset;
	[loop] for (uint Module = 0; Module < SpawnModuleCount; ++Module)
	{
		const float4 Header      = Program[Cursor];
		const uint   Type        = asuint(Header.x);
		const uint   InputCount  = asuint(Header.y);
		const uint   ModuleIndex = asuint(Header.z);
		const uint   Base        = Cursor + 1;
		Cursor += 1 + InputCount * 3;

#define IN(Index) EvalInput(Base, ModuleIndex, Index, SpawnIndex, EmitterAlpha)
		if (Type == MODULE_INITIALIZE_PARTICLE)
		{
			P.Lifetime  = max(IN(0).x, 0.01f);
			P.BaseColor = IN(1);
			P.Color     = P.BaseColor;
			P.BaseSize  = IN(2).xy;
			P.Size      = P.BaseSize;
			P.Rotation  = IN(3).x;
			P.Mass      = max(IN(4).x, 0.001f);
		}
		else if (Type == MODULE_SHAPE_LOCATION)
		{
			const float4 R = ParticleRandom4(SpawnIndex, Seed, ModuleIndex * 16 + 15);
			LocalPosition += ShapePosition(int(IN(0).x), IN(1).x, IN(2).xyz, IN(3).x, IN(4).x, IN(5).x != 0.0f, R) + IN(6).xyz;
		}
		else if (Type == MODULE_ADD_VELOCITY)
		{
			LocalVelocity += IN(0).xyz;
		}
		else if (Type == MODULE_ADD_VELOCITY_IN_CONE)
		{
			const float4 R = ParticleRandom4(SpawnIndex, Seed, ModuleIndex * 16 + 15);
			LocalVelocity += ConeDirection(IN(0).xyz, IN(1).x, R.x, R.y) * IN(2).x;
		}
		else if (Type == MODULE_ADD_VELOCITY_FROM_POINT)
		{
			const float3 Offset = LocalPosition - IN(0).xyz;
			const float4 R      = ParticleRandom4(SpawnIndex, Seed, ModuleIndex * 16 + 15);
			const float3 Dir    = dot(Offset, Offset) > 1e-6f ? normalize(Offset) : ConeDirection(float3(0, 0, 1), 180.0f, R.x, R.y);
			LocalVelocity += Dir * IN(1).x;
		}
#undef IN
	}

	P.Position = ToSim(LocalPosition);
	P.Velocity = bLocalSpace != 0 ? LocalVelocity : mul(float4(LocalVelocity, 0.0f), EmitterWorld).xyz;
}

void UpdateParticle(inout FParticleData P)
{
	P.Age += DeltaSeconds;
	if (P.Age >= P.Lifetime)
	{
		return;
	}
	const float Alpha = P.Age / P.Lifetime;

	float3 Acceleration   = float3(0, 0, 0);
	float  DragAmount     = 0.0f;
	uint   CollisionBase  = 0;
	uint   CollisionIndex = 0;
	bool   bCollision     = false;
	uint   Cursor         = UpdateOffset;
	[loop] for (uint Module = 0; Module < UpdateModuleCount; ++Module)
	{
		const float4 Header      = Program[Cursor];
		const uint   Type        = asuint(Header.x);
		const uint   InputCount  = asuint(Header.y);
		const uint   ModuleIndex = asuint(Header.z);
		const uint   Base        = Cursor + 1;
		Cursor += 1 + InputCount * 3;

#define IN(Index) EvalInput(Base, ModuleIndex, Index, P.SpawnIndex, Alpha)
		if (Type == MODULE_GRAVITY || Type == MODULE_ACCELERATION)
		{
			Acceleration += IN(0).xyz;
		}
		else if (Type == MODULE_DRAG)
		{
			DragAmount += max(IN(0).x, 0.0f);
		}
		else if (Type == MODULE_CURL_NOISE)
		{
			Acceleration += CurlNoise((P.Position - IN(2).xyz * Time) * IN(1).x) * IN(0).x;
		}
		else if (Type == MODULE_VORTEX)
		{
			const float3 Axis   = AxisToSim(IN(0).xyz);
			const float3 Offset = P.Position - ToSim(IN(1).xyz);
			const float3 Radial = Offset - Axis * dot(Offset, Axis);
			if (dot(Radial, Radial) > 1e-6f)
			{
				Acceleration += normalize(cross(Axis, Radial)) * IN(2).x - normalize(Radial) * IN(3).x;
			}
		}
		else if (Type == MODULE_POINT_ATTRACTION)
		{
			const float3 ToPoint = ToSim(IN(0).xyz) - P.Position;
			const float  Dist    = length(ToPoint);
			if (IN(3).x > 0.0f && Dist < IN(3).x)
			{
				P.Age = P.Lifetime;
				return;
			}
			const float Radius = max(IN(2).x, 1.0f);
			if (Dist > 1e-3f && Dist < Radius)
			{
				Acceleration += ToPoint / Dist * (IN(1).x * (1.0f - Dist / Radius));
			}
		}
		else if (Type == MODULE_SCALE_COLOR)
		{
			P.Color = P.BaseColor * IN(0);
		}
		else if (Type == MODULE_SCALE_SPRITE_SIZE)
		{
			P.Size = P.BaseSize * IN(0).xy;
		}
		else if (Type == MODULE_SPRITE_ROTATION_RATE)
		{
			P.Rotation += IN(0).x * DeltaSeconds;
		}
		else if (Type == MODULE_SUBUV)
		{
			const int   Mode   = int(IN(0).x);
			const float Frames = max(IN(1).x, 1.0f);
			if (Mode == 0) P.SubImage = Alpha * Frames;
			else if (Mode == 1) P.SubImage = P.Age * IN(2).x;
			else P.SubImage = floor(ParticleRandom01(P.SpawnIndex, Seed, ModuleIndex * 64 + 7) * Frames);
		}
		else if (Type == MODULE_COLLISION)
		{
			bCollision     = true;
			CollisionBase  = Base;
			CollisionIndex = ModuleIndex;
		}
#undef IN
	}

	P.Velocity = (P.Velocity + Acceleration * DeltaSeconds) * (DragAmount > 0.0f ? exp(-DragAmount * DeltaSeconds) : 1.0f);
	P.Position += P.Velocity * DeltaSeconds;

	if (bCollision)
	{
		const float Height = EvalInput(CollisionBase, CollisionIndex, 0, P.SpawnIndex, Alpha).x;
		const float Radius = P.Size.x * 0.5f;
		if (P.Position.z - Radius < Height && P.Velocity.z < 0.0f)
		{
			if (EvalInput(CollisionBase, CollisionIndex, 3, P.SpawnIndex, Alpha).x != 0.0f)
			{
				P.Age = P.Lifetime;
				return;
			}
			P.Position.z         = Height + Radius;
			const float Friction = saturate(EvalInput(CollisionBase, CollisionIndex, 2, P.SpawnIndex, Alpha).x);
			const float Bounce   = saturate(EvalInput(CollisionBase, CollisionIndex, 1, P.SpawnIndex, Alpha).x);
			P.Velocity           = float3(P.Velocity.x * (1.0f - Friction), P.Velocity.y * (1.0f - Friction), -P.Velocity.z * Bounce);
		}
	}
}

[numthreads(64, 1, 1)]
void CSMain(uint3 ThreadId : SV_DispatchThreadID)
{
	const uint Slot = ThreadId.x;
	if (Slot >= Capacity)
	{
		return;
	}
	// 이 칸이 이번 생성 구간에 들어가면 새 입자 (고리 버퍼라 가장 오래된 입자를 덮어쓴다)
	const uint Offset = (Slot + Capacity - (SpawnStart % Capacity)) % Capacity;
	FParticleData P   = Particles[Slot];
	if (Offset < SpawnCount)
	{
		SpawnParticle(P, SpawnStart + Offset);
	}
	else if (P.Age < P.Lifetime)
	{
		UpdateParticle(P);
	}
	else
	{
		return;
	}
	Particles[Slot] = P;
}
