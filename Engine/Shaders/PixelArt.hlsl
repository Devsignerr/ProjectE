#include "Common.hlsli"
#include "Fullscreen.hlsli"

// 픽셀 아트 합성: 저해상도 톤매핑 결과 → 출력 (sRGB RTV이므로 선형 값을 쓴다)
//   1) 서브픽셀 보정 최근접 확대 — FPixelArtMath::OutputToSource와 같은 식
//   2) 저해상도 깊이로 1px 외곽선(실루엣, 앞 물체 쪽 픽셀을 어둡게) + 볼록 모서리 하이라이트(한쪽 픽셀만 밝게)
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

// 깊이에서 노멀 재구성: 좌우/상하 중 깊이 변화가 작은 쪽 차분을 써서 경계 너머 면을 섞지 않는다. 카메라를 향하도록(-Z) 맞춤
float3 ReconstructNormal(int2 Texel, int2 SourceSize)
{
	const float3 Center = GetViewPosition(Texel, SourceSize);
	const float3 Right  = GetViewPosition(Texel + int2(1, 0), SourceSize);
	const float3 Left   = GetViewPosition(Texel + int2(-1, 0), SourceSize);
	const float3 Down   = GetViewPosition(Texel + int2(0, 1), SourceSize);
	const float3 Up     = GetViewPosition(Texel + int2(0, -1), SourceSize);

	const float3 DeltaX = abs(Right.z - Center.z) < abs(Center.z - Left.z) ? Right - Center : Center - Left;
	const float3 DeltaY = abs(Up.z - Center.z) < abs(Center.z - Down.z) ? Up - Center : Center - Down;
	float3       Normal = normalize(cross(DeltaY, DeltaX));
	return Normal.z > 0.0f ? -Normal : Normal;
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
		const float3 Center       = GetViewPosition(Texel, SourceSize);
		const float3 CenterNormal = ReconstructNormal(Texel, SourceSize);

		float DepthEdge  = 0.0f;
		float NormalEdge = 0.0f;
		[unroll]
		for (int Index = 0; Index < 4; ++Index)
		{
			const int2   NeighborTexel = Texel + NeighborOffsets[Index];
			const float3 Neighbor      = GetViewPosition(NeighborTexel, SourceSize);
			const float  DepthDelta    = Neighbor.z - Center.z;

			// 이웃이 충분히 멀면 이 픽셀은 앞 물체의 가장자리 → 외곽선은 앞 물체 쪽 1px에 생긴다
			if (DepthDelta > DepthThreshold)
			{
				DepthEdge = 1.0f;
			}

			// 같은 면 근처(깊이 차 작음)에서 노멀이 꺾이고, 이웃이 내 접평면 뒤로 떨어지면(볼록) 하이라이트.
			// 모서리 양쪽 중 한 픽셀만 칠하도록 오른쪽/위/카메라를 향한 노멀 쪽을 고른다
			if (abs(DepthDelta) < DepthThreshold)
			{
				const float3 NeighborNormal = ReconstructNormal(NeighborTexel, SourceSize);
				const float  Crease         = smoothstep(0.1f, 0.4f, 1.0f - dot(CenterNormal, NeighborNormal));
				const bool   bConvex        = dot(Neighbor - Center, CenterNormal) < 0.0f;
				const bool   bPreferredSide = dot(CenterNormal - NeighborNormal, float3(1.0f, 1.0f, -1.0f)) > 0.0f;
				if (bConvex && bPreferredSide)
				{
					NormalEdge = max(NormalEdge, Crease);
				}
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
