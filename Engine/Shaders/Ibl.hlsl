// 초기화 전용 GPU 적분. CPU 참조 식은 Renderer/IblMath.h.
cbuffer BakeConstants : register(b0)
{
	uint Size;
	uint SampleCount;
	float Roughness;
	uint Padding;
};

TextureCube<float4> Environment : register(t0);
RWTexture2DArray<float4> CubeOutput : register(u0);
RWTexture2D<float4> LutOutput : register(u1);
SamplerState LinearSampler : register(s0);

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

[numthreads(8, 8, 1)]
void IrradianceCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= Size) || Id.z >= 6) return;

	float3 N = FaceDirection(
		Id.z, (float2(Id.xy) + 0.5) / float(Size) * 2 - 1);
	float3 Sum = 0;

	for (uint I = 0; I < SampleCount; ++I)
	{
		float2 Xi = Hammersley(I);
		float Phi = 2 * Pi * Xi.x;
		float R = sqrt(Xi.y);
		float3 L = ToWorld(
			float3(R * cos(Phi), R * sin(Phi), sqrt(1 - Xi.y)), N);

		Sum += Environment.SampleLevel(LinearSampler, L, 0).rgb;
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

	if (Roughness <= 0)
	{
		CubeOutput[Id] = float4(
			Environment.SampleLevel(LinearSampler, N, 0).rgb, 1);
		return;
	}

	float3 Sum = 0;
	float Weight = 0;

	for (uint I = 0; I < SampleCount; ++I)
	{
		float3 H = ToWorld(SampleGGX(Hammersley(I), Roughness), N);
		float3 L = normalize(2 * dot(N, H) * H - N);
		float NdotL = saturate(dot(N, L));

		if (NdotL > 0)
		{
			Sum += Environment.SampleLevel(LinearSampler, L, 0).rgb * NdotL;
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
