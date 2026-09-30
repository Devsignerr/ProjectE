#include "Common.hlsli"

// 게임 UI (FUIRenderer). 사각형마다 정점 6개를 SV_VertexID로 만들고, 사각형 데이터는 구조화 버퍼에서 읽는다.
//   좌표는 화면 픽셀 (좌상단 원점, +Y 아래). 색은 선형 + 직선 알파 (알파 블렌드), 출력 RTV는 sRGB.
//   Box: 둥근 사각형 거리(SDF)로 모서리 안티에일리어싱 + 안쪽 테두리. 텍스처(없으면 흰색)를 곱한다.
//   SdfText: 글꼴 아틀라스 거리 값 → 픽셀 거리(× 거리 범위)로 경계 0.5픽셀 부드럽게 + 외곽선. UV는 텍셀 좌표.

cbuffer UIConstants : register(b0)
{
	float2 ViewportSize;
	float2 ConstantsPadding;
};

cbuffer UIBatch : register(b1)
{
	uint  QuadOffset; // 묶음의 첫 사각형 (SV_InstanceID는 0부터)
	uint3 BatchPadding;
};

// UIDrawList.h FUIDrawQuad와 1:1
struct FUIQuad
{
	float4 Rect;           // 픽셀 (MinX, MinY, MaxX, MaxY)
	float4 UV;             // Box: 정규화 UV, SdfText: 아틀라스 텍셀
	float4 Color;
	float4 SecondaryColor; // 테두리 / 외곽선
	float4 Params;         // Box: (반지름, 테두리 폭, 모드, 0)  SdfText: (거리 범위, 외곽선 폭, 모드, 0)
};

StructuredBuffer<FUIQuad> Quads        : register(t1);
Texture2D                 QuadTexture  : register(t0);
SamplerState              LinearClamp  : register(s0);

struct FUIVSOutput
{
	float4               Position       : SV_Position;
	float2               UV             : TEXCOORD0;
	float2               Local          : TEXCOORD1; // 사각형 가운데 기준 픽셀
	nointerpolation float2 HalfSize     : TEXCOORD2;
	nointerpolation float4 Color        : COLOR0;
	nointerpolation float4 SecondaryColor : COLOR1;
	nointerpolation float4 Params       : TEXCOORD3;
};

static const float2 GCorners[6] = {
	float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
	float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(0.0f, 1.0f),
};

FUIVSOutput VSMain(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	const FUIQuad Quad   = Quads[QuadOffset + InstanceId];
	const float2  Corner = GCorners[VertexId];
	const float2  Pixel  = lerp(Quad.Rect.xy, Quad.Rect.zw, Corner);

	FUIVSOutput Output;
	Output.Position       = float4(Pixel.x / ViewportSize.x * 2.0f - 1.0f, 1.0f - Pixel.y / ViewportSize.y * 2.0f, 0.0f, 1.0f);
	Output.UV             = lerp(Quad.UV.xy, Quad.UV.zw, Corner);
	Output.HalfSize       = (Quad.Rect.zw - Quad.Rect.xy) * 0.5f;
	Output.Local          = (Corner - 0.5f) * 2.0f * Output.HalfSize;
	Output.Color          = Quad.Color;
	Output.SecondaryColor = Quad.SecondaryColor;
	Output.Params         = Quad.Params;
	return Output;
}

// 둥근 사각형 부호 거리 (안쪽 음수)
float RoundedRectDistance(float2 P, float2 HalfSize, float Radius)
{
	const float2 Q = abs(P) - HalfSize + Radius;
	return length(max(Q, 0.0f)) + min(max(Q.x, Q.y), 0.0f) - Radius;
}

float4 PSMain(FUIVSOutput Input) : SV_Target
{
	const uint Mode = (uint)(Input.Params.z + 0.5f);
	if (Mode == 1)
	{
		float2 AtlasSize;
		QuadTexture.GetDimensions(AtlasSize.x, AtlasSize.y);
		const float Value    = QuadTexture.Sample(LinearClamp, Input.UV / AtlasSize).r;
		const float Distance = (Value - 0.5f) * Input.Params.x; // 픽셀 (안쪽 +)
		const float TextA    = Input.Color.a * saturate(Distance + 0.5f);
		const float OutlineA = Input.Params.y > 0.0f ? Input.SecondaryColor.a * saturate(Distance + Input.Params.y + 0.5f) : 0.0f;
		// 글자를 외곽선 위에 합성
		const float Alpha = TextA + OutlineA * (1.0f - TextA);
		if (Alpha <= 0.0f)
		{
			discard;
		}
		const float3 Rgb = (Input.Color.rgb * TextA + Input.SecondaryColor.rgb * OutlineA * (1.0f - TextA)) / Alpha;
		return float4(Rgb, Alpha);
	}

	const float Distance = RoundedRectDistance(Input.Local, Input.HalfSize, Input.Params.x);
	const float Coverage = saturate(0.5f - Distance);
	if (Coverage <= 0.0f)
	{
		discard;
	}
	float4 Fill = Input.Color * QuadTexture.Sample(LinearClamp, Input.UV);
	if (Input.Params.y > 0.0f)
	{
		// 안쪽으로 테두리 폭만큼: 경계 거리 + 폭 > 0이면 테두리
		const float BorderFactor = saturate(Distance + Input.Params.y + 0.5f);
		Fill                     = lerp(Fill, Input.SecondaryColor, BorderFactor);
	}
	return float4(Fill.rgb, Fill.a * Coverage);
}
