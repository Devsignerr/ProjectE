// 파티클 공용: 입자 구조 + 난수/노이즈/모양 함수 (Scene/Particles.cpp와 같은 식 — 한쪽을 고치면 다른 쪽도)

#ifndef PARTICLE_COMMON_HLSLI
#define PARTICLE_COMMON_HLSLI

// ShaderTypes.h FParticleGpuData와 일치 (96바이트). Age >= Lifetime이면 죽은 입자
struct FParticleData
{
	float3 Position;
	float  Age;
	float3 Velocity;
	float  Lifetime;
	float4 BaseColor;
	float4 Color;
	float2 BaseSize;
	float2 Size;
	float  Rotation; // 도
	float  Mass;
	float  SubImage;
	uint   SpawnIndex;
};

static const float GPi    = 3.14159265358979f;
static const float GTwoPi = 6.28318530717959f;

// PCG 해시 (FParticleSimulation::Hash)
uint ParticleHash(uint Value)
{
	const uint State = Value * 747796405u + 2891336453u;
	const uint Word  = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
	return (Word >> 22u) ^ Word;
}

float HashToFloat(uint Value)
{
	return float(ParticleHash(Value) >> 8) * (1.0f / 16777216.0f);
}

float ParticleRandom01(uint SpawnIndex, uint Seed, uint Stream)
{
	return HashToFloat(ParticleHash(SpawnIndex ^ ParticleHash(Seed + Stream * 0x9E3779B9u)));
}

float4 ParticleRandom4(uint SpawnIndex, uint Seed, uint Stream)
{
	return float4(ParticleRandom01(SpawnIndex, Seed, Stream * 4 + 0), ParticleRandom01(SpawnIndex, Seed, Stream * 4 + 1),
	              ParticleRandom01(SpawnIndex, Seed, Stream * 4 + 2), ParticleRandom01(SpawnIndex, Seed, Stream * 4 + 3));
}

float LatticeValue(int X, int Y, int Z, uint Channel)
{
	const uint Key = (asuint(X) * 73856093u) ^ (asuint(Y) * 19349663u) ^ (asuint(Z) * 83492791u) ^ (Channel * 2654435761u);
	return HashToFloat(Key) * 2.0f - 1.0f;
}

float SmoothCurve(float T)
{
	return T * T * (3.0f - 2.0f * T);
}

float ValueNoise(float3 P, uint Channel)
{
	const float3 F = floor(P);
	const int    X = int(F.x), Y = int(F.y), Z = int(F.z);
	const float  TX = SmoothCurve(P.x - F.x), TY = SmoothCurve(P.y - F.y), TZ = SmoothCurve(P.z - F.z);
	const float  X00 = lerp(LatticeValue(X, Y, Z, Channel), LatticeValue(X + 1, Y, Z, Channel), TX);
	const float  X10 = lerp(LatticeValue(X, Y + 1, Z, Channel), LatticeValue(X + 1, Y + 1, Z, Channel), TX);
	const float  X01 = lerp(LatticeValue(X, Y, Z + 1, Channel), LatticeValue(X + 1, Y, Z + 1, Channel), TX);
	const float  X11 = lerp(LatticeValue(X, Y + 1, Z + 1, Channel), LatticeValue(X + 1, Y + 1, Z + 1, Channel), TX);
	return lerp(lerp(X00, X10, TY), lerp(X01, X11, TY), TZ);
}

float3 CurlNoise(float3 P)
{
	const float E    = 0.5f;
	const float dZdY = ValueNoise(P + float3(0, E, 0), 2) - ValueNoise(P - float3(0, E, 0), 2);
	const float dYdZ = ValueNoise(P + float3(0, 0, E), 1) - ValueNoise(P - float3(0, 0, E), 1);
	const float dXdZ = ValueNoise(P + float3(0, 0, E), 0) - ValueNoise(P - float3(0, 0, E), 0);
	const float dZdX = ValueNoise(P + float3(E, 0, 0), 2) - ValueNoise(P - float3(E, 0, 0), 2);
	const float dYdX = ValueNoise(P + float3(E, 0, 0), 1) - ValueNoise(P - float3(E, 0, 0), 1);
	const float dXdY = ValueNoise(P + float3(0, E, 0), 0) - ValueNoise(P - float3(0, E, 0), 0);
	return float3(dZdY - dYdZ, dXdZ - dZdX, dYdX - dXdY) * (1.0f / (2.0f * E));
}

float3 SafeNormalize(float3 V, float3 Fallback)
{
	const float LenSq = dot(V, V);
	return LenSq > 1e-8f ? V * rsqrt(LenSq) : Fallback;
}

float3 ConeDirection(float3 Axis, float ConeDegrees, float R0, float R1)
{
	const float  CosMax   = cos(radians(clamp(ConeDegrees, 0.0f, 180.0f)));
	const float  CosTheta = lerp(1.0f, CosMax, R0);
	const float  SinTheta = sqrt(max(0.0f, 1.0f - CosTheta * CosTheta));
	const float  Phi      = R1 * GTwoPi;
	const float3 N        = SafeNormalize(Axis, float3(0, 0, 1));
	const float3 Helper   = abs(N.z) < 0.99f ? float3(0, 0, 1) : float3(1, 0, 0);
	const float3 T        = normalize(cross(Helper, N));
	const float3 B        = cross(N, T);
	return normalize(N * CosTheta + T * (SinTheta * cos(Phi)) + B * (SinTheta * sin(Phi)));
}

float3 ShapePosition(int Shape, float Radius, float3 BoxSize, float Height, float MinorRadius, bool bSurface, float4 R)
{
	if (Shape == 1)
	{
		return ConeDirection(float3(0, 0, 1), 180.0f, R.x, R.y) * (Radius * (bSurface ? 1.0f : pow(R.z, 1.0f / 3.0f)));
	}
	if (Shape == 2)
	{
		float3 P = (R.xyz * 2.0f - 1.0f) * BoxSize;
		if (bSurface)
		{
			const float3 Ratio = abs(P) / max(BoxSize, 1e-4f);
			if (Ratio.x >= Ratio.y && Ratio.x >= Ratio.z) P.x = P.x < 0.0f ? -BoxSize.x : BoxSize.x;
			else if (Ratio.y >= Ratio.z) P.y = P.y < 0.0f ? -BoxSize.y : BoxSize.y;
			else P.z = P.z < 0.0f ? -BoxSize.z : BoxSize.z;
		}
		return P;
	}
	if (Shape == 3)
	{
		const float Angle = R.x * GTwoPi;
		const float Dist  = Radius * (bSurface ? 1.0f : sqrt(R.y));
		return float3(cos(Angle) * Dist, sin(Angle) * Dist, R.z * Height);
	}
	if (Shape == 4)
	{
		const float T     = bSurface ? R.z : pow(R.z, 1.0f / 3.0f);
		const float Angle = R.x * GTwoPi;
		const float Dist  = Radius * T * (bSurface ? 1.0f : sqrt(R.y));
		return float3(cos(Angle) * Dist, sin(Angle) * Dist, T * Height);
	}
	if (Shape == 5)
	{
		const float Major = R.x * GTwoPi;
		const float Minor = R.y * GTwoPi;
		const float Tube  = MinorRadius * (bSurface ? 1.0f : sqrt(R.z));
		const float Ring  = Radius + cos(Minor) * Tube;
		return float3(cos(Major) * Ring, sin(Major) * Ring, sin(Minor) * Tube);
	}
	return float3(0, 0, 0);
}

#endif
