// 초기화 전용 GPU 적분. CPU 참조 식은 Renderer/IblMath.h.
cbuffer BakeConstants : register(b0)
{
	uint Size;
	uint SampleCount;
	float Roughness;
	float Rotation; // EquirectCS: 환경맵 Z축 회전 (라디안, +면 오른쪽으로 돈다)
};

TextureCube<float4> Environment : register(t0);
Texture2D<float4> EquirectSource : register(t1); // 등장방형 HDR (Phase 33-7)
RWTexture2DArray<float4> CubeOutput : register(u0);
RWTexture2D<float4> LutOutput : register(u1);
RWTexture2DArray<float4> CubeSource : register(u2); // DownsampleCS: 윗 밉
SamplerState LinearSampler : register(s0);
SamplerState WrapSampler : register(s1);

static const float Pi = 3.14159265358979323846;

float3 FaceDirection(uint Face, float2 UV)
{
	float U = UV.x;
	float V = UV.y;

	if (Face == 0) return normalize(float3(1, -V, -U));
	if (Face == 1) return normalize(float3(-1, -V, U));
	if (Face == 2) return normalize(float3(U, 1, V));
	if (Face == 3) return normalize(float3(U, -1, -V));
	if (Face == 4) return normalize(float3(U, -V, 1));
	return normalize(float3(-U, -V, -1));
}

float2 Hammersley(uint Index)
{
	return float2(
		float(Index) / float(SampleCount),
		float(reversebits(Index)) * 2.3283064365386963e-10);
}

float3 ToWorld(float3 H, float3 N)
{
	float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
	float3 T = normalize(cross(Up, N));
	return T * H.x + cross(N, T) * H.y + N * H.z;
}

float3 SampleGGX(float2 Xi, float SurfaceRoughness)
{
	float Alpha = SurfaceRoughness * SurfaceRoughness;
	float Phi = 2 * Pi * Xi.x;
	float CosTheta = sqrt(
		(1 - Xi.y) / max(1 + (Alpha * Alpha - 1) * Xi.y, 1e-7));
	float SinTheta = sqrt(max(0, 1 - CosTheta * CosTheta));

	return float3(
		SinTheta * cos(Phi),
		SinTheta * sin(Phi),
		CosTheta);
}

// GGX 법선 분포 D (IblMath::GgxDistribution과 같은 식)
float GgxDistribution(float NdotH, float SurfaceRoughness)
{
	float Alpha2 = SurfaceRoughness * SurfaceRoughness * SurfaceRoughness * SurfaceRoughness;
	float Denom = NdotH * NdotH * (Alpha2 - 1) + 1;
	return Alpha2 / max(Pi * Denom * Denom, 1e-8);
}

// 필터드 중요도 샘플링 (IblMath::ComputeFilteredSampleLod와 같은 식): 표본 하나가 대표하는 입체각만큼 흐린 원본 밉에서 읽는다.
//   원본 밉 0만 읽으면 HDR 하늘의 해처럼 밝고 작은 광원이 표본 방향마다 따로 찍혀 반사/조도가 점박이가 된다.
//   원본 밉이 하나뿐이면(반사 캡처 원본 큐브) SampleLevel이 밉 0으로 자른다
float FilteredSampleLod(float Pdf, uint SourceSize)
{
	float SampleSolidAngle = 1 / max(float(SampleCount) * Pdf, 1e-8);
	float TexelSolidAngle = 4 * Pi / (6 * float(SourceSize) * float(SourceSize));
	return max(0.5 * log2(SampleSolidAngle / TexelSolidAngle) + 1, 0);
}

uint EnvironmentSize()
{
	uint Width, Height, Mips;
	Environment.GetDimensions(0, Width, Height, Mips);
	return Width;
}

float GeometrySmithIbl(float NdotV, float NdotL, float SurfaceRoughness)
{
	float K = SurfaceRoughness * SurfaceRoughness / 2;
	float GV = NdotV / max(NdotV * (1 - K) + K, 1e-6);
	float GL = NdotL / max(NdotL * (1 - K) + K, 1e-6);
	return GV * GL;
}

[numthreads(8, 8, 1)]
void SkyCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size) || Id.z >= 6) return;

	float3 N = FaceDirection(
		Id.z, (float2(Id.xy) + 0.5) / float(Size) * 2 - 1);

	// Z-up, 선형 HDR 색상. 태양 직접광은 씬 방향광에서 계산한다.
	float3 Color = N.z >= 0
		? lerp(float3(0.55, 0.65, 0.8),
			   float3(0.08, 0.22, 0.55), pow(N.z, 0.45))
		: lerp(float3(0.55, 0.65, 0.8),
			   float3(0.035, 0.03, 0.025), pow(-N.z, 0.25));

	CubeOutput[Id] = float4(Color, 1);
}

// 등장방형 HDR → 하늘 큐브 (Renderer/IblMath.h DirectionToEquirectUV와 같은 식): 경도 = atan2(y, x), U = 0.5 + 경도 / 2π, V = 0.5 - 위도 / π
[numthreads(8, 8, 1)]
void EquirectCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size) || Id.z >= 6) return;

	float3 N = FaceDirection(
		Id.z, (float2(Id.xy) + 0.5) / float(Size) * 2 - 1);

	// 환경을 +Rotation만큼 돌리면 방향 N은 원본의 -Rotation 방향을 본다
	float S, C;
	sincos(-Rotation, S, C);
	float3 D = float3(N.x * C - N.y * S, N.x * S + N.y * C, N.z);
	float2 UV = float2(0.5 + atan2(D.y, D.x) / (2 * Pi), 0.5 - asin(clamp(D.z, -1, 1)) / Pi);

	CubeOutput[Id] = float4(max(EquirectSource.SampleLevel(WrapSampler, UV, 0).rgb, 0), 1);
}

// 하늘 큐브 밉 체인 (필터드 중요도 샘플링용): 윗 밉(u2) 2x2 평균 → 이번 밉(u0). Size = 이번 밉 크기
[numthreads(8, 8, 1)]
void DownsampleCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size) || Id.z >= 6) return;

	uint2 Base = Id.xy * 2;
	float4 Sum = CubeSource[uint3(Base, Id.z)] + CubeSource[uint3(Base + uint2(1, 0), Id.z)]
		+ CubeSource[uint3(Base + uint2(0, 1), Id.z)] + CubeSource[uint3(Base + uint2(1, 1), Id.z)];
	CubeOutput[Id] = Sum * 0.25;
}

[numthreads(8, 8, 1)]
void IrradianceCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size) || Id.z >= 6) return;

	float3 N = FaceDirection(
		Id.z, (float2(Id.xy) + 0.5) / float(Size) * 2 - 1);
	float3 Sum = 0;
	uint SourceSize = EnvironmentSize();

	for (uint I = 0; I < SampleCount; ++I)
	{
		float2 Xi = Hammersley(I);
		float Phi = 2 * Pi * Xi.x;
		float R = sqrt(Xi.y);
		float CosTheta = sqrt(1 - Xi.y);
		float3 L = ToWorld(
			float3(R * cos(Phi), R * sin(Phi), CosTheta), N);

		// 코사인 샘플링 pdf = cos / pi
		float Lod = FilteredSampleLod(CosTheta / Pi, SourceSize);
		Sum += Environment.SampleLevel(LinearSampler, L, Lod).rgb;
	}

	// 코사인 중요도 샘플링 평균 = irradiance / pi.
	CubeOutput[Id] = float4(Sum / float(SampleCount), 1);
}

[numthreads(8, 8, 1)]
void PrefilterCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size) || Id.z >= 6) return;

	float3 N = FaceDirection(
		Id.z, (float2(Id.xy) + 0.5) / float(Size) * 2 - 1);

	uint SourceSize = EnvironmentSize();
	if (Roughness <= 0)
	{
		// 거울 밉: 출력 텍셀 하나가 덮는 만큼의 원본 밉 (512 원본 → 128 출력이면 밉 2). 밉 0 한 점만 읽으면 해 가장자리가 계단·누락된다
		CubeOutput[Id] = float4(
			Environment.SampleLevel(LinearSampler, N, max(log2(float(SourceSize) / float(Size)), 0)).rgb, 1);
		return;
	}

	float3 Sum = 0;
	float Weight = 0;

	for (uint I = 0; I < SampleCount; ++I)
	{
		float3 LocalH = SampleGGX(Hammersley(I), Roughness);
		float3 H = ToWorld(LocalH, N);
		float3 L = normalize(2 * dot(N, H) * H - N);
		float NdotL = saturate(dot(N, L));

		if (NdotL > 0)
		{
			// N = V 가정에서 반사 방향 pdf = D · NdotH / (4 · VdotH) = D / 4
			float Lod = FilteredSampleLod(GgxDistribution(LocalH.z, Roughness) * 0.25, SourceSize);
			Sum += Environment.SampleLevel(LinearSampler, L, Lod).rgb * NdotL;
			Weight += NdotL;
		}
	}

	CubeOutput[Id] = float4(Sum / max(Weight, 1e-6), 1);
}

[numthreads(8, 8, 1)]
void BrdfCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size)) return;

	float NdotV = (float(Id.x) + 0.5) / float(Size);
	float SurfaceRoughness = (float(Id.y) + 0.5) / float(Size);
	float3 V = float3(sqrt(1 - NdotV * NdotV), 0, NdotV);
	float2 Sum = 0;

	for (uint I = 0; I < SampleCount; ++I)
	{
		float3 H = SampleGGX(Hammersley(I), SurfaceRoughness);
		float RawVdotH = dot(V, H);
		float VdotH = saturate(RawVdotH);
		float3 L = 2 * RawVdotH * H - V;
		float NdotL = saturate(L.z);

		if (NdotL > 0)
		{
			float G = GeometrySmithIbl(NdotV, NdotL, SurfaceRoughness);
			float GVis = G * VdotH / max(H.z * NdotV, 1e-6);
			float Fc = pow(1 - VdotH, 5);

			Sum += float2(1 - Fc, Fc) * GVis;
		}
	}

	LutOutput[Id.xy] = float4(Sum / float(SampleCount), 0, 1);
}
