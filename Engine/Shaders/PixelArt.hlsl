#include "Common.hlsli"
#include "Fullscreen.hlsli"

// 픽셀 아트 합성: 저해상도 톤매핑 결과 → 출력 (sRGB RTV이므로 선형 값을 쓴다)
//   1) 서브픽셀 보정 최근접 확대 — FPixelArtMath::OutputToSource와 같은 식
//   2) 저해상도 깊이로 1px 외곽선(실루엣, 앞 물체 쪽 픽셀을 어둡게) + 볼록 모서리 하이라이트(깊이 능선, 한쪽 픽셀만 밝게)
//   3) 채널별 색 단계 양자화 + 4x4 Bayer 디더 (무늬 원점은 월드 격자에 고정 → 카메라가 움직여도 무늬가 화면에 붙지 않음)
// 모든 판정은 도트(소스 텍셀) 단위라 출력 픽셀 N×N이 같은 결과를 낸다.

cbuffer PixelArtConstants : register(b0)
{
	float2 OutputSize;
	float  PixelSize;
	float  OutlineStrength;
	float2 SubPixelOffset;    // 소스 텍셀 단위
	float  HighlightStrength;
	float  DepthThreshold;    // cm
	uint2  DitherOrigin;      // 0~3
	float  ColorLevels;       // 0 = 양자화 끔
	float  DitherStrength;
	uint   bOrthographic;
	float  NearZ;
	float  FarZ;
	float  PixelViewScale;    // 직교: 텍셀 월드 크기(cm), 원근: 깊이 1당 텍셀 크기
};

Texture2D<float4> SourceColor : register(t0); // 톤매핑된 저해상도 (선형 LDR)
Texture2D<float>  SourceDepth : register(t1); // 저해상도 장치 깊이 [0, 1]

static const int2 NeighborOffsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };

int2 GetSourceSize()
{
	uint Width;
	uint Height;
	SourceDepth.GetDimensions(Width, Height);
	return int2(Width, Height);
}

float LoadDeviceDepth(int2 Texel, int2 SourceSize)
{
	return SourceDepth.Load(int3(clamp(Texel, int2(0, 0), SourceSize - 1), 0));
}

// 장치 깊이 → 뷰 공간 깊이(cm). 배경(깊이 1)은 FarZ
float LinearizeDepth(float DeviceDepth)
{
	if (bOrthographic != 0)
	{
		return lerp(NearZ, FarZ, DeviceDepth);
	}
	return NearZ * FarZ / (FarZ - DeviceDepth * (FarZ - NearZ));
}

// 텍셀 중심의 뷰 공간 위치 (+X 오른쪽, +Y 위, +Z 앞)
float3 GetViewPosition(int2 Texel, int2 SourceSize)
{
	const float  Depth = LinearizeDepth(LoadDeviceDepth(Texel, SourceSize));
	const float2 Grid  = (float2(Texel) + 0.5f - float2(SourceSize) * 0.5f) * float2(1.0f, -1.0f);
	const float  Scale = bOrthographic != 0 ? PixelViewScale : PixelViewScale * Depth;
	return float3(Grid * Scale, Depth);
}

// 텍셀 하나의 월드 크기(cm) — 판정 문턱값의 기준
float TexelWorldSize(float Depth)
{
	return bOrthographic != 0 ? PixelViewScale : PixelViewScale * Depth;
}

// 축 방향 깊이 2차 차분 (앞뒤 이웃이 모두 같은 물체일 때만, 아니면 0). 볼록 능선(양옆이 뒤로 물러남)이면 양수
float RidgeCurvature(int2 Texel, int2 Axis, int2 SourceSize)
{
	const float Center = GetViewPosition(Texel, SourceSize).z;
	const float Next   = GetViewPosition(Texel + Axis, SourceSize).z;
	const float Prev   = GetViewPosition(Texel - Axis, SourceSize).z;
	if (abs(Next - Center) >= DepthThreshold || abs(Prev - Center) >= DepthThreshold)
	{
		return 0.0f;
	}
	return Next + Prev - 2.0f * Center;
}

float3 LinearToSrgbApprox(float3 Color)
{
	return pow(max(Color, 0.0f), 1.0f / 2.2f);
}

float3 SrgbToLinearApprox(float3 Color)
{
	return pow(max(Color, 0.0f), 2.2f);
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSMain(FFullscreenVSOutput Input) : SV_Target
{
	const int2 SourceSize = GetSourceSize();

	// 1) 출력 픽셀 → 소스 텍셀 (SV_Position은 픽셀 중심)
	const float2 SourcePosition = (Input.Position.xy - OutputSize * 0.5f) / PixelSize + float2(SourceSize) * 0.5f + SubPixelOffset;
	const int2   Texel          = clamp(int2(floor(SourcePosition)), int2(0, 0), SourceSize - 1);

	float3 Color = SourceColor.Load(int3(Texel, 0)).rgb;

	// 2) 외곽선 / 모서리 하이라이트 (배경 픽셀에는 그리지 않는다)
	const bool bBackground = LoadDeviceDepth(Texel, SourceSize) >= 1.0f;
	if (!bBackground && (OutlineStrength > 0.0f || HighlightStrength > 0.0f))
	{
		const float3 Center = GetViewPosition(Texel, SourceSize);

		// 이웃이 충분히 멀면 이 픽셀은 앞 물체의 가장자리 → 외곽선은 앞 물체 쪽 1px에 생긴다
		float DepthEdge = 0.0f;
		[unroll]
		for (int Index = 0; Index < 4; ++Index)
		{
			if (GetViewPosition(Texel + NeighborOffsets[Index], SourceSize).z - Center.z > DepthThreshold)
			{
				DepthEdge = 1.0f;
			}
		}

		// 볼록 모서리 하이라이트: 가로/세로 깊이 능선(2차 차분 > 문턱). 판정값이 확실히 양수인 곳만 켜므로 카메라가 도트 단위로
		// 움직일 때 생기는 미세 오차에 흔들리지 않는다(같은 평면 = 0). 모서리 양쪽 두 픽셀 중 능선 값이 큰 쪽 하나만,
		// 비슷하면(여유 Tie 이내) 축의 앞쪽(왼쪽/위) 픽셀로 고정. 완만한 곡면(구 등)은 문턱 아래라 칠하지 않는다
		const float Texel1     = TexelWorldSize(Center.z);
		const float Tie        = 0.02f * Texel1;
		const int2  Axes[2]    = { int2(1, 0), int2(0, 1) };
		float       NormalEdge = 0.0f;
		[unroll]
		for (int AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
		{
			const int2  Axis  = Axes[AxisIndex];
			const float Ridge = RidgeCurvature(Texel, Axis, SourceSize);
			if (Ridge > Tie && Ridge + Tie >= RidgeCurvature(Texel + Axis, Axis, SourceSize) && Ridge > RidgeCurvature(Texel - Axis, Axis, SourceSize) + Tie)
			{
				NormalEdge = max(NormalEdge, smoothstep(0.4f, 1.0f, Ridge / Texel1));
			}
		}

		if (DepthEdge > 0.0f)
		{
			Color *= 1.0f - OutlineStrength;
		}
		else
		{
			Color *= 1.0f + HighlightStrength * NormalEdge;
		}
	}

	// 3) 양자화 + Bayer 디더 (지각적으로 고르게 나뉘도록 감마 공간에서)
	if (ColorLevels >= 2.0f)
	{
		static const float Bayer[16] = { 0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f };
		const uint2        Cell      = (uint2(Texel) + DitherOrigin) & 3u;
		const float        Threshold = (Bayer[Cell.y * 4 + Cell.x] + 0.5f) / 16.0f - 0.5f;
		const float        Steps     = ColorLevels - 1.0f;

		float3 Encoded = LinearToSrgbApprox(saturate(Color));
		Encoded        = floor(Encoded * Steps + 0.5f + Threshold * DitherStrength) / Steps;
		Color          = SrgbToLinearApprox(saturate(Encoded));
	}

	return float4(Color, 1.0f);
}
