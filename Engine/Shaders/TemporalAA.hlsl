#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// TAA 해상 (FTemporalAA, 톤매핑 전 HDR). 식은 Renderer/TemporalMath.h와 같다
//   1) 3x3 이웃에서 가장 가까운 깊이의 움직임 벡터 (윤곽선 고스팅 감소). 기하가 없으면(깊이 1) 카메라 재투영
//   2) 이전 UV = UV - 움직임. 이력을 Catmull-Rom(5탭)으로 읽는다
//   3) 이웃 색 분산(YCoCg, 톤매핑 공간) 상자로 이력을 클립 → 고스팅/가려짐 해제 대응
//   4) 현재 비중 = 기본값, 반응형 마스크(씬 컬러 알파 = 파티클 덮임)와 큰 움직임에서 키운다. 이력 없음이면 현재 그대로

cbuffer TaaConstants : register(b0)
{
	float4x4 Reprojection;    // 현재 클립(지터 없음) → 이전 클립, 카메라만 (기하 없는 픽셀)
	float2   TexelSize;       // 1 / 화면 크기
	float    CurrentWeight;   // 기본 현재 프레임 비중
	uint     bHistoryValid;
	float    ReactiveWeight;  // 반응형 마스크가 1일 때 현재 비중
	float    VarianceGamma;   // 이웃 상자 폭 (표준편차 배수)
	// ---- TAAU (PSResolveUpsample / PSUpscaleDepth만 읽는다 — PSResolve는 쓰지 않아 네이티브 결과가 그대로)
	float2   InputSize;       // 내부(씬) 해상도 픽셀 크기
	float2   InputTexelSize;  // 1 / InputSize
	float2   JitterUv;        // 이번 프레임 지터 (UV, FUpscaleMath::JitterNdcToUv)
	float    UpsampleScale;   // 출력 / 내부 (1보다 크다)
	float    Padding1;
	float2   Padding2;
};

Texture2D<float4> SceneColor    : register(t0); // 알파 = 반응형 마스크 (불투명 0, 파티클 덮임)
Texture2D<float4> History       : register(t1);
Texture2D<float2> Velocity      : register(t2);
Texture2D<float>  Depth         : register(t3);
SamplerState      LinearSampler : register(s0);
SamplerState      PointSampler  : register(s1);

float MaxComponent(float3 C)
{
	return max(C.r, max(C.g, C.b));
}

// 밝은 픽셀 하나가 이력을 지배하지 않게 톤매핑 공간에서 섞는다 (가역)
float3 TonemapForTaa(float3 C)
{
	return C / (1.0f + MaxComponent(C));
}

float3 InverseTonemapForTaa(float3 C)
{
	return C / max(1.0f - MaxComponent(C), 1.0e-4f);
}

float3 RgbToYCoCg(float3 C)
{
	return float3(0.25f * C.r + 0.5f * C.g + 0.25f * C.b, 0.5f * C.r - 0.5f * C.b, -0.25f * C.r + 0.5f * C.g - 0.25f * C.b);
}

float3 YCoCgToRgb(float3 C)
{
	return float3(C.x + C.y - C.z, C.x + C.z, C.x - C.y - C.z);
}

// Catmull-Rom 9탭을 쌍선형 5탭으로 (모서리 4개 생략)
float3 SampleHistoryCatmullRom(float2 UV)
{
	const float2 SamplePos = UV / TexelSize;
	const float2 TexPos1   = floor(SamplePos - 0.5f) + 0.5f;
	const float2 F         = SamplePos - TexPos1;
	const float2 W0        = F * (-0.5f + F * (1.0f - 0.5f * F));
	const float2 W1        = 1.0f + F * F * (-2.5f + 1.5f * F);
	const float2 W2        = F * (0.5f + F * (2.0f - 1.5f * F));
	const float2 W3        = F * F * (-0.5f + 0.5f * F);
	const float2 W12       = W1 + W2;
	const float2 Offset12  = W2 / W12;
	const float2 P0        = (TexPos1 - 1.0f) * TexelSize;
	const float2 P3        = (TexPos1 + 2.0f) * TexelSize;
	const float2 P12       = (TexPos1 + Offset12) * TexelSize;

	float3 Result = 0.0f;
	float  Weight = 0.0f;
	const float W[5]  = { W12.x * W0.y, W0.x * W12.y, W12.x * W12.y, W3.x * W12.y, W12.x * W3.y };
	const float2 P[5] = { float2(P12.x, P0.y), float2(P0.x, P12.y), P12, float2(P3.x, P12.y), float2(P12.x, P3.y) };
	[unroll]
	for (int Index = 0; Index < 5; ++Index)
	{
		Result += History.SampleLevel(LinearSampler, P[Index], 0.0f).rgb * W[Index];
		Weight += W[Index];
	}
	return max(Result / max(Weight, 1.0e-4f), 0.0f);
}

// 이력을 상자 중심 방향으로 상자 안까지 당긴다 (AABB 클립)
float3 ClipToBox(float3 HistoryValue, float3 BoxMin, float3 BoxMax)
{
	const float3 Center = 0.5f * (BoxMax + BoxMin);
	const float3 Extent = 0.5f * (BoxMax - BoxMin) + 1.0e-5f;
	const float3 Offset = HistoryValue - Center;
	const float3 Units  = abs(Offset / Extent);
	const float  Scale  = max(Units.x, max(Units.y, Units.z));
	return Scale > 1.0f ? Center + Offset / Scale : HistoryValue;
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSResolve(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel   = int2(Input.Position.xy);
	const float4 Center  = SceneColor.Load(int3(Pixel, 0));
	const float3 Current = TonemapForTaa(max(Center.rgb, 0.0f));

	// 이웃 통계 + 가장 가까운 깊이
	float3 Mean        = 0.0f;
	float3 MeanSquared = 0.0f;
	float  Closest     = 1.0f;
	int2   ClosestPixel = Pixel;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			const int2   P     = Pixel + int2(X, Y);
			const float3 Value = RgbToYCoCg(TonemapForTaa(max(SceneColor.Load(int3(P, 0)).rgb, 0.0f)));
			Mean += Value;
			MeanSquared += Value * Value;
			const float D = Depth.Load(int3(P, 0));
			if (D < Closest)
			{
				Closest      = D;
				ClosestPixel = P;
			}
		}
	}
	Mean /= 9.0f;
	const float3 Sigma  = sqrt(max(MeanSquared / 9.0f - Mean * Mean, 0.0f));
	const float3 BoxMin = Mean - VarianceGamma * Sigma;
	const float3 BoxMax = Mean + VarianceGamma * Sigma;

	if (bHistoryValid == 0)
	{
		return float4(InverseTonemapForTaa(Current), 0.0f);
	}

	// 움직임 벡터: 기하가 없으면 카메라 재투영 (먼 평면)
	float2 Motion;
	if (Closest >= 1.0f)
	{
		const float2 Ndc  = float2(Input.UV.x * 2.0f - 1.0f, 1.0f - Input.UV.y * 2.0f);
		const float4 Prev = mul(float4(Ndc, 1.0f, 1.0f), Reprojection);
		const float2 PrevUV = Prev.w > 1.0e-6f ? float2(Prev.x / Prev.w * 0.5f + 0.5f, 0.5f - Prev.y / Prev.w * 0.5f) : Input.UV;
		Motion            = Input.UV - PrevUV;
	}
	else
	{
		Motion = Velocity.Load(int3(ClosestPixel, 0));
	}
	const float2 PrevUV = Input.UV - Motion;
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return float4(InverseTonemapForTaa(Current), 0.0f); // 화면 밖에서 들어온 픽셀
	}

	const float3 HistoryYCoCg = ClipToBox(RgbToYCoCg(TonemapForTaa(SampleHistoryCatmullRom(PrevUV))), BoxMin, BoxMax);
	const float3 Clipped      = YCoCgToRgb(HistoryYCoCg);

	// 현재 비중: 반응형(파티클) + 빠른 움직임(픽셀 단위)에서 키운다
	const float Reactive    = saturate(Center.a);
	const float SpeedPixels = length(Motion / TexelSize);
	float       Weight      = lerp(CurrentWeight, ReactiveWeight, Reactive);
	Weight                  = lerp(Weight, max(Weight, 0.25f), saturate(SpeedPixels / 32.0f));

	const float3 Result = lerp(Clipped, Current, Weight);
	return float4(InverseTonemapForTaa(max(Result, 0.0f)), 0.0f);
}

// ---- TAAU (Phase 48): 내부 해상도(지터된) 씬 → 출력 해상도 이력. 식은 Renderer/UpscaleMath.h
//   1) 출력 픽셀 중심(지터 없음) UV → 지터된 내부 영상에서의 연속 위치 InputPos = (UV + JitterUv) × InputSize
//   2) 내부 3x3 이웃: 분산 상자(YCoCg, 톤매핑 공간)·가장 가까운 깊이의 움직임 벡터는 기존 TAA와 같은 식 (내부 해상도 기준)
//   3) 현재 표본 = 출력 픽셀 단위 거리 가우시안(exp(-2.29 d²)) 가중 평균, 신뢰도 = 가장 가까운 표본의 가중치
//      → 이번 프레임 표본이 이 출력 픽셀 중심 가까이 떨어졌을 때만 크게 섞는다 (지터가 돌며 출력 격자를 채움)
//   4) 이력 없음/화면 밖이면 내부 픽셀 단위 가우시안으로 부드럽게 재구성 (공간 업스케일 — TAA를 끈 업스케일도 이 경로)
float4 PSResolveUpsample(FFullscreenVSOutput Input) : SV_Target
{
	const float2 InputPos = (Input.UV + JitterUv) * InputSize;
	const int2   Base     = int2(floor(InputPos));
	const int2   MaxPixel = int2(InputSize) - 1;

	float3 Mean         = 0.0f;
	float3 MeanSquared  = 0.0f;
	float  Closest      = 1.0f;
	int2   ClosestPixel = clamp(Base, 0, MaxPixel);
	float3 SharpSum     = 0.0f;
	float  SharpWeight  = 0.0f;
	float  Confidence   = 0.0f;
	float3 SmoothSum    = 0.0f;
	float  SmoothWeight = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			const int2   P      = clamp(Base + int2(X, Y), 0, MaxPixel);
			const float3 Value  = TonemapForTaa(max(SceneColor.Load(int3(P, 0)).rgb, 0.0f));
			const float3 YCoCg  = RgbToYCoCg(Value);
			Mean += YCoCg;
			MeanSquared += YCoCg * YCoCg;
			const float D = Depth.Load(int3(P, 0));
			if (D < Closest)
			{
				Closest      = D;
				ClosestPixel = P;
			}
			const float2 DeltaInput = float2(Base + int2(X, Y)) + 0.5f - InputPos; // 내부 픽셀 단위 (가장자리 복제 표본도 원래 자리로)
			const float2 DeltaOutput = DeltaInput * UpsampleScale;
			const float  Sharp       = exp(-2.29f * dot(DeltaOutput, DeltaOutput));
			const float  Smooth      = exp(-2.29f * dot(DeltaInput, DeltaInput));
			SharpSum += Value * Sharp;
			SharpWeight += Sharp;
			Confidence = max(Confidence, Sharp);
			SmoothSum += Value * Smooth;
			SmoothWeight += Smooth;
		}
	}
	Mean /= 9.0f;
	const float3 Sigma   = sqrt(max(MeanSquared / 9.0f - Mean * Mean, 0.0f));
	const float3 BoxMin  = Mean - VarianceGamma * Sigma;
	const float3 BoxMax  = Mean + VarianceGamma * Sigma;
	const float3 Smooth  = SmoothSum / max(SmoothWeight, 1.0e-6f);
	const float3 Current = SharpWeight > 1.0e-4f ? SharpSum / SharpWeight : Smooth;

	if (bHistoryValid == 0)
	{
		return float4(InverseTonemapForTaa(Smooth), 0.0f);
	}

	// 움직임 벡터 (UV 단위라 해상도 무관): 기하가 없으면 카메라 재투영
	float2 Motion;
	if (Closest >= 1.0f)
	{
		const float2 Ndc    = float2(Input.UV.x * 2.0f - 1.0f, 1.0f - Input.UV.y * 2.0f);
		const float4 Prev   = mul(float4(Ndc, 1.0f, 1.0f), Reprojection);
		const float2 PrevUV = Prev.w > 1.0e-6f ? float2(Prev.x / Prev.w * 0.5f + 0.5f, 0.5f - Prev.y / Prev.w * 0.5f) : Input.UV;
		Motion              = Input.UV - PrevUV;
	}
	else
	{
		Motion = Velocity.Load(int3(ClosestPixel, 0));
	}
	const float2 PrevUV = Input.UV - Motion;
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return float4(InverseTonemapForTaa(Smooth), 0.0f);
	}

	const float3 HistoryYCoCg = ClipToBox(RgbToYCoCg(TonemapForTaa(SampleHistoryCatmullRom(PrevUV))), BoxMin, BoxMax);
	const float3 Clipped      = YCoCgToRgb(HistoryYCoCg);

	// 현재 비중: 기존 식(반응형·빠른 움직임) × 표본 신뢰도. 반응형/빠른 움직임일수록 신뢰도 영향을 줄인다 (번짐보다 반응)
	const float Reactive    = saturate(SceneColor.Load(int3(clamp(Base, 0, MaxPixel), 0)).a);
	const float SpeedPixels = length(Motion / TexelSize);
	const float Fast        = saturate(SpeedPixels / 32.0f);
	float       Weight      = lerp(CurrentWeight, ReactiveWeight, Reactive);
	Weight                  = lerp(Weight, max(Weight, 0.25f), Fast);
	Weight *= lerp(Confidence, 1.0f, max(Reactive, Fast));

	const float3 Result = lerp(Clipped, Current, Weight);
	return float4(InverseTonemapForTaa(max(Result, 0.0f)), 0.0f);
}

// 오버레이용 출력 해상도 깊이 (에디터 그리드·디버그 선 깊이 테스트): 내부 깊이를 지터를 되돌려 쌍선형으로 (평면에서 장치 깊이는 화면 공간
// 아핀이라 정확). 4표본의 1/z 상대 차가 크면(윤곽) 가장 가까운 표본 그대로
float PSUpscaleDepth(FFullscreenVSOutput Input) : SV_Depth
{
	const float2 Position = (Input.UV + JitterUv) * InputSize - 0.5f;
	const int2   MaxPixel = int2(InputSize) - 1;
	const int2   P0       = int2(floor(Position));
	const float2 F        = Position - float2(P0);
	const float  D00      = Depth.Load(int3(clamp(P0, 0, MaxPixel), 0));
	const float  D10      = Depth.Load(int3(clamp(P0 + int2(1, 0), 0, MaxPixel), 0));
	const float  D01      = Depth.Load(int3(clamp(P0 + int2(0, 1), 0, MaxPixel), 0));
	const float  D11      = Depth.Load(int3(clamp(P0 + int2(1, 1), 0, MaxPixel), 0));
	const float  MinD     = min(min(D00, D10), min(D01, D11));
	const float  MaxD     = max(max(D00, D10), max(D01, D11));
	if ((MaxD - MinD) / max(1.0f - MinD, 1.0e-6f) < 0.05f)
	{
		return lerp(lerp(D00, D10, F.x), lerp(D01, D11, F.x), F.y);
	}
	const bool bRight = F.x >= 0.5f;
	const bool bDown  = F.y >= 0.5f;
	return bDown ? (bRight ? D11 : D01) : (bRight ? D10 : D00);
}