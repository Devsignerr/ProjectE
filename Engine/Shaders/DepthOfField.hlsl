#include "Common.hlsli"
#include "Fullscreen.hlsli"

// 피사계 심도 (반해상도 원반 보케 — Unity Post Processing v2 DiskKernel 방식, HDR 톤매핑 전)
//   착란원(CoC) = 화면 높이 비율, 음수 근경 / 양수 원경. 식은 Renderer/PostProcessMath.h ComputeCircleOfConfusion과 같아야 한다
//   틸트시프트(Mode 1·2): 화면 위치로 CoC (ComputeTiltShiftCoc — 기운 초점 띠 아래 = 근경, 위 = 원경), Mode 2는 깊이 CoC와 절댓값 큰 쪽
//   PSPrefilter : 전체 해상도 색(t0) + 깊이(t1) 4탭 → 반해상도 (Karis 가중 평균 색, 절댓값이 큰 CoC). 초점이 맞은 색은 어둡게(근경 번짐 차단)
//   PSBokeh     : 반해상도 원반 71탭 모으기. 원경 = 가운데·표본 CoC 중 작은 쪽이 거리를 덮는 표본, 근경 = 표본 자신의 CoC가 덮는 표본 (근경이 앞을 덮음)
//                 조리개 날(BladeCount >= 3)이면 표본 위치를 정다각형 안으로 옮기고 덮임 판정은 원래 고리 거리(다각형 노름)로 — 빛망울이 다각형.
//                 하이라이트 강조(HighlightBoost > 0)면 밝은 표본 가중치 1 + 세기 × (밝기 - 문턱) — 등불·불티가 또렷한 빛망울로 남는다
//   PSPostfilter: 반해상도 4탭 텐트 (원반 표본 사이 틈 메움)
//   PSCombine   : 전체 해상도 색(t0) + 보케(t1, 선형 확대) + 깊이(t2) → 원경 알파(가운데 CoC)와 근경 알파(보케 a)로 섞는다

cbuffer DepthOfFieldConstants : register(b0)
{
	float2 SourceTexelSize; // 1 / 이 패스 입력 크기 (Prefilter·Combine = 전체 해상도, Bokeh·Postfilter = 반해상도)
	float  RcpAspect;       // 화면 높이 / 너비 (CoC는 높이 비율이라 가로 표본 거리를 줄인다)
	float  MaxCoc;          // 보케 원반 반경 (화면 높이 비율) = 근경·원경 최대 흐림 중 큰 값
	float  FocusDistance;   // cm
	float  FocalRegion;     // cm
	float  NearTransition;  // cm
	float  FarTransition;   // cm
	float  NearBlur;        // 화면 높이 비율
	float  FarBlur;
	float  NearZ;
	float  FarZ;
	uint   bOrthographic;
	uint   Mode;            // 0 깊이, 1 틸트시프트, 2 둘 다 (EDepthOfFieldMode)
	float  TiltCenter;      // 초점 띠 가운데 (화면 위 0 ~ 아래 1)
	float  TiltBand;        // 선명한 띠 반폭 (화면 높이 비율)
	float  TiltTransition;  // 최대 흐림까지 (화면 높이 비율)
	float2 TiltNormal;      // 띠 법선 (-sin, cos) — 아래쪽이 +
	float  Aspect;          // 너비 / 높이
	float  BladeCount;      // 보케 조리개 날 수 (3 미만 = 원)
	float  BladeRotation;   // 라디안
	float  HighlightBoost;  // 밝은 점 강조 세기 (0 = 끔)
	float  HighlightThreshold;
};

Texture2D<float4> Source       : register(t0);
Texture2D<float4> Source2      : register(t1);
Texture2D<float>  Source3      : register(t2);
SamplerState      LinearSampler : register(s0);
SamplerState      PointSampler  : register(s1);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

// 장치 깊이 → 뷰 깊이 (FPostProcessMath::LinearizeDepth와 같음)
float LinearizeDepth(float DeviceDepth)
{
	if (bOrthographic != 0)
	{
		return lerp(NearZ, FarZ, DeviceDepth);
	}
	return NearZ * FarZ / (FarZ - DeviceDepth * (FarZ - NearZ));
}

// FPostProcessMath::ComputeCircleOfConfusion과 같은 식 (값은 CPU가 0 이상으로 정리해 넘긴다)
float ComputeCoc(float ViewDepth)
{
	const float Near = FocusDistance - FocalRegion - ViewDepth;
	const float Far  = ViewDepth - FocusDistance - FocalRegion;
	if (Near > 0.0f)
	{
		const float T = NearTransition > 0.0f ? min(Near / NearTransition, 1.0f) : 1.0f;
		return -T * NearBlur;
	}
	if (Far > 0.0f)
	{
		const float T = FarTransition > 0.0f ? min(Far / FarTransition, 1.0f) : 1.0f;
		return T * FarBlur;
	}
	return 0.0f;
}

// FPostProcessMath::ComputeTiltShiftCoc와 같은 식
float ComputeTiltShiftCoc(float2 UV)
{
	const float Signed  = (UV.x - 0.5f) * Aspect * TiltNormal.x + (UV.y - TiltCenter) * TiltNormal.y;
	const float Outside = abs(Signed) - TiltBand;
	if (Outside <= 0.0f)
	{
		return 0.0f;
	}
	const float T = TiltTransition > 0.0f ? min(Outside / TiltTransition, 1.0f) : 1.0f;
	return Signed > 0.0f ? -T * NearBlur : T * FarBlur;
}

float CocAt(float2 UV)
{
	if (Mode == 1)
	{
		return ComputeTiltShiftCoc(UV);
	}
	const float DepthCoc = ComputeCoc(LinearizeDepth(Source3.SampleLevel(PointSampler, UV, 0.0f)));
	if (Mode == 2)
	{
		const float TiltCoc = ComputeTiltShiftCoc(UV);
		return abs(TiltCoc) > abs(DepthCoc) ? TiltCoc : DepthCoc; // FPostProcessMath::CombineCircleOfConfusion
	}
	return DepthCoc;
}

float MaxComponent(float3 C)
{
	return max(C.r, max(C.g, C.b));
}

// ---- 1) 반해상도 축소 + CoC. 깊이는 t2(Source3)로 읽는다 (t1은 비워 둠)
float4 PSPrefilter(FFullscreenVSOutput Input) : SV_Target
{
	const float2 UV = Input.UV;
	const float3 Offset = float3(SourceTexelSize * 0.5f, 0.0f);
	const float2 UV0 = UV + float2(-Offset.x, -Offset.y);
	const float2 UV1 = UV + float2(Offset.x, -Offset.y);
	const float2 UV2 = UV + float2(-Offset.x, Offset.y);
	const float2 UV3 = UV + float2(Offset.x, Offset.y);
	const float3 C0 = Source.SampleLevel(LinearSampler, UV0, 0.0f).rgb;
	const float3 C1 = Source.SampleLevel(LinearSampler, UV1, 0.0f).rgb;
	const float3 C2 = Source.SampleLevel(LinearSampler, UV2, 0.0f).rgb;
	const float3 C3 = Source.SampleLevel(LinearSampler, UV3, 0.0f).rgb;
	const float Coc0 = CocAt(UV0);
	const float Coc1 = CocAt(UV1);
	const float Coc2 = CocAt(UV2);
	const float Coc3 = CocAt(UV3);

	// 밝은 점 반짝임 억제 (Karis 가중). 하이라이트 강조를 켜면 일반 평균 — Karis가 작은 광원 에너지를 미리 깎아 빛망울이 남지 않는다
	//   (입력은 TAA 뒤라 시간 반짝임은 이미 안정됨)
	const float Karis = HighlightBoost > 0.0f ? 0.0f : 1.0f;
	const float W0 = 1.0f / (MaxComponent(C0) * Karis + 1.0f);
	const float W1 = 1.0f / (MaxComponent(C1) * Karis + 1.0f);
	const float W2 = 1.0f / (MaxComponent(C2) * Karis + 1.0f);
	const float W3 = 1.0f / (MaxComponent(C3) * Karis + 1.0f);
	float3 Color = (C0 * W0 + C1 * W1 + C2 * W2 + C3 * W3) / (W0 + W1 + W2 + W3);

	// 절댓값이 큰 CoC (근경이 원경보다 크면 근경)
	const float CocMin = min(min(Coc0, Coc1), min(Coc2, Coc3));
	const float CocMax = max(max(Coc0, Coc1), max(Coc2, Coc3));
	const float Coc    = -CocMin > CocMax ? CocMin : CocMax;

	// 초점이 맞은 색은 어둡게 → 근경 모으기에서 선명한 물체가 번져 나오지 않는다 (원경은 가중치로 이미 막힘)
	const float HalfTexel = SourceTexelSize.y * 2.0f; // 반해상도 텍셀 높이
	Color *= smoothstep(0.0f, HalfTexel * 2.0f, abs(Coc));
	return float4(Color, Coc);
}

// FPostProcessMath::BokehPolygonRadius와 같은 식
float BokehPolygonRadius(float Angle)
{
	if (BladeCount < 3.0f)
	{
		return 1.0f;
	}
	const float Sector  = 6.28318530718f / BladeCount;
	const float Local   = Angle - BladeRotation;
	const float Wrapped = Local - Sector * floor(Local / Sector);
	return cos(Sector * 0.5f) / cos(Wrapped - Sector * 0.5f);
}

// FPostProcessMath::BokehHighlightWeight와 같은 식
float BokehHighlightWeight(float3 Color)
{
	return 1.0f + HighlightBoost * max(MaxComponent(Color) - HighlightThreshold, 0.0f);
}

// ---- 2) 원반 보케 모으기 (반해상도): 고리 5개 = 1 + 7 + 14 + 21 + 28 = 71탭 (43탭은 다각형 빛망울의 한 변에 표본 3~4개라 가장자리가 울퉁불퉁)
static const uint  BokehRings   = 5;
static const float BokehSamples = 71.0f;

float4 PSBokeh(FFullscreenVSOutput Input) : SV_Target
{
	const float4 Center = Source.SampleLevel(PointSampler, Input.UV, 0.0f);
	const float  Margin = SourceTexelSize.y * 2.0f;
	float4 Background = 0.0f;
	float4 Foreground = 0.0f;
	float  ForegroundHighlight = 0.0f; // 근경 색 정규화용 (Foreground.a는 덮인 표본 수 그대로)

	[unroll]
	for (uint Ring = 0; Ring < BokehRings; ++Ring)
	{
		const uint  Count  = Ring == 0 ? 1u : Ring * 7u;
		const float Radius = float(Ring) / float(BokehRings - 1u);
		[unroll]
		for (uint Index = 0; Index < Count; ++Index)
		{
			const float  Angle = (float(Index) + (Ring & 1u) * 0.5f) * (6.28318530718f / float(Count));
			// 거리(덮임 판정) = 고리 반경 그대로 (다각형 노름), 표본 위치만 다각형 가장자리 비율로 당긴다
			const float  Dist  = Radius * MaxCoc;
			const float2 Disp  = float2(cos(Angle), sin(Angle)) * (Dist * BokehPolygonRadius(Angle));
			const float4 Tap   = Source.SampleLevel(LinearSampler, Input.UV + Disp * float2(RcpAspect, 1.0f), 0.0f);
			const float  Highlight = BokehHighlightWeight(Tap.rgb);

			// 원경: 가운데와 표본 중 작은 원경 CoC가 이 거리를 덮어야 한다 (선명한 앞 물체 위로 원경이 번지지 않게)
			const float BackgroundCoc = max(min(Center.a, Tap.a), 0.0f);
			const float BackgroundW   = saturate((BackgroundCoc - Dist + Margin) / Margin);
			// 근경: 표본 자신의 근경 CoC가 이 거리를 덮으면 기여 (앞 물체가 선명한 뒤를 덮으며 번진다)
			float ForegroundW = saturate((-Tap.a - Dist + Margin) / Margin);
			ForegroundW *= step(SourceTexelSize.y, -Tap.a);

			// 강조는 색 합에만 (a = 덮인 표본 수 — 근경 알파 정규화에 쓰므로 그대로), 색은 강조 가중 합 / 강조 가중치 합
			Background += float4(Tap.rgb * Highlight, Highlight) * BackgroundW;
			Foreground += float4(Tap.rgb * Highlight, 1.0f) * ForegroundW;
			ForegroundHighlight += Highlight * ForegroundW;
		}
	}
	Background.rgb /= Background.a + (Background.a == 0.0f ? 1.0f : 0.0f);
	Foreground.rgb /= ForegroundHighlight + (ForegroundHighlight == 0.0f ? 1.0f : 0.0f);

	// 근경 알파 = 덮은 표본 비율 (원반 넓이 정규화), 원경은 합성에서 전체 해상도 CoC로 정한다
	const float Alpha = saturate(Foreground.a * 3.14159265f / BokehSamples);
	return float4(lerp(Background.rgb, Foreground.rgb, Alpha), Alpha);
}

// ---- 3) 4탭 텐트 (반해상도)
float4 PSPostfilter(FFullscreenVSOutput Input) : SV_Target
{
	const float4 Offset = SourceTexelSize.xyxy * float4(-0.5f, -0.5f, 0.5f, 0.5f);
	float4 Sum = Source.SampleLevel(LinearSampler, Input.UV + Offset.xy, 0.0f);
	Sum += Source.SampleLevel(LinearSampler, Input.UV + Offset.zy, 0.0f);
	Sum += Source.SampleLevel(LinearSampler, Input.UV + Offset.xw, 0.0f);
	Sum += Source.SampleLevel(LinearSampler, Input.UV + Offset.zw, 0.0f);
	return Sum * 0.25f;
}

// ---- 4) 합성 (전체 해상도)
float4 PSCombine(FFullscreenVSOutput Input) : SV_Target
{
	const float4 Color = Source.SampleLevel(PointSampler, Input.UV, 0.0f);
	const float4 Bokeh = Source2.SampleLevel(LinearSampler, Input.UV, 0.0f);
	const float  Coc   = CocAt(Input.UV);
	// 원경 알파: 전체 해상도 텍셀 2~4개 반경 사이에서 서서히 (선명한 경계는 원본 그대로)
	const float FarAlpha = smoothstep(SourceTexelSize.y * 2.0f, SourceTexelSize.y * 4.0f, Coc);
	const float Alpha    = FarAlpha + Bokeh.a - FarAlpha * Bokeh.a;
	return float4(lerp(Color.rgb, Bokeh.rgb, Alpha), Color.a);
}
