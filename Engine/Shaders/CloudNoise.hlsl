// 볼류메트릭 구름 노이즈 생성 (Phase 49, FVolumetricCloudRenderer — 시작 때 한 번, 결정적 해시)
//   ShapeNoiseCS   128^3 RGBA8: R = 펄린-워리 (펄린 FBM을 워리 FBM 위로 다시 매핑), G/B/A = 워리 FBM (주기 8/16/32 칸)
//   DetailNoiseCS   32^3 RGBA8: R/G/B = 워리 FBM (주기 4/8/16 칸)
//   WeatherCS      256^2 RG8:  R = 덮임 변화 (펄린 FBM), G = 구름 종류 변화
//   모든 노이즈는 텍스처 한 장 안에서 주기적이라 반복 타일링에 이음매가 없다 (Wrap 샘플러)

cbuffer NoiseConstants : register(b0)
{
	uint NoiseSize;
	uint NoiseKind;
	uint NoisePadding0;
	uint NoisePadding1;
};

RWTexture3D<float4> Output3D : register(u0);
RWTexture2D<float2> Output2D : register(u1);

float3 Hash33(float3 P)
{
	uint3 Q = uint3(int3(P) * int3(1597334673, 3812015801, 2798796415));
	Q       = (Q.x ^ Q.y ^ Q.z) * uint3(1597334673, 3812015801, 2798796415);
	return float3(Q) * (1.0f / 4294967295.0f);
}

// 주기 Period 칸 워리 (F1, 0~1, 칸 크기로 정규화). 반환 = 1 - F1 (구름용 뒤집은 값)
float WorleyTiled(float3 P, float Period)
{
	const float3 Cell    = floor(P);
	const float3 Fract   = frac(P);
	float        MinDist = 10.0f;
	[unroll]
	for (int Z = -1; Z <= 1; ++Z)
	{
		[unroll]
		for (int Y = -1; Y <= 1; ++Y)
		{
			[unroll]
			for (int X = -1; X <= 1; ++X)
			{
				const float3 Offset = float3(X, Y, Z);
				const float3 Wrapped = fmod(Cell + Offset + Period, Period);
				const float3 Point   = Offset + Hash33(Wrapped) - Fract;
				MinDist              = min(MinDist, dot(Point, Point));
			}
		}
	}
	return 1.0f - saturate(sqrt(MinDist));
}

float3 GradientTiled(float3 Cell, float Period)
{
	return normalize(Hash33(fmod(Cell + Period * 4.0f, Period)) * 2.0f - 1.0f + 1.0e-4f);
}

// 주기 Period 칸 그레이디언트 노이즈 (-1~1 근방)
float PerlinTiled(float3 P, float Period)
{
	const float3 Cell = floor(P);
	const float3 F    = frac(P);
	const float3 U    = F * F * F * (F * (F * 6.0f - 15.0f) + 10.0f);
	float        Result = 0.0f;
	float        Corners[8];
	[unroll]
	for (int Index = 0; Index < 8; ++Index)
	{
		const float3 Corner = float3(Index & 1, (Index >> 1) & 1, (Index >> 2) & 1);
		Corners[Index]      = dot(GradientTiled(Cell + Corner, Period), F - Corner);
	}
	const float X00 = lerp(Corners[0], Corners[1], U.x);
	const float X10 = lerp(Corners[2], Corners[3], U.x);
	const float X01 = lerp(Corners[4], Corners[5], U.x);
	const float X11 = lerp(Corners[6], Corners[7], U.x);
	Result          = lerp(lerp(X00, X10, U.y), lerp(X01, X11, U.y), U.z);
	return Result;
}

float WorleyFbm(float3 UVW, float Period)
{
	return WorleyTiled(UVW * Period, Period) * 0.625f + WorleyTiled(UVW * Period * 2.0f, Period * 2.0f) * 0.25f +
	       WorleyTiled(UVW * Period * 4.0f, Period * 4.0f) * 0.125f;
}

float PerlinFbm(float3 UVW, float Period, uint Octaves)
{
	float Sum       = 0.0f;
	float Amplitude = 0.5f;
	float Frequency = Period;
	float Norm      = 0.0f;
	for (uint Octave = 0; Octave < Octaves; ++Octave)
	{
		Sum += PerlinTiled(UVW * Frequency, Frequency) * Amplitude;
		Norm += Amplitude;
		Amplitude *= 0.5f;
		Frequency *= 2.0f;
	}
	return Sum / Norm;
}

float RemapNoise(float Value, float OldMin, float OldMax, float NewMin, float NewMax)
{
	return NewMin + (Value - OldMin) / max(OldMax - OldMin, 1.0e-6f) * (NewMax - NewMin);
}

[numthreads(4, 4, 4)]
void ShapeNoiseCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id >= NoiseSize))
	{
		return;
	}
	const float3 UVW    = (float3(Id) + 0.5f) / float(NoiseSize);
	const float  Perlin = saturate(PerlinFbm(UVW, 4.0f, 4) * 0.5f + 0.5f);
	const float  Worley = WorleyFbm(UVW, 4.0f);
	// 펄린-워리: 펄린을 [워리 - 1, 1] → [0, 1]로 (뭉게진 덩어리 + 둥근 가장자리)
	const float PerlinWorley = saturate(RemapNoise(Perlin, Worley - 1.0f, 1.0f, 0.0f, 1.0f));
	Output3D[Id] = float4(PerlinWorley, WorleyFbm(UVW, 8.0f), WorleyFbm(UVW, 16.0f), WorleyFbm(UVW, 32.0f));
}

[numthreads(4, 4, 4)]
void DetailNoiseCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id >= NoiseSize))
	{
		return;
	}
	const float3 UVW = (float3(Id) + 0.5f) / float(NoiseSize);
	Output3D[Id]     = float4(WorleyFbm(UVW, 4.0f), WorleyFbm(UVW, 8.0f), WorleyFbm(UVW, 16.0f), 1.0f);
}

[numthreads(8, 8, 1)]
void WeatherCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= NoiseSize))
	{
		return;
	}
	const float3 UVW      = float3((float2(Id.xy) + 0.5f) / float(NoiseSize), 0.37f);
	const float  Coverage = saturate(PerlinFbm(UVW, 4.0f, 5) * 0.75f + 0.5f);
	const float  Type     = saturate(PerlinFbm(UVW + 0.31f, 3.0f, 3) * 0.75f + 0.5f);
	Output2D[Id.xy]       = float2(Coverage, Type);
}
