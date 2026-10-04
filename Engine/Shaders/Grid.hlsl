#include "Common.hlsli"

// 에디터 뷰포트 무한 그리드 (Z = 0 평면, 단위 cm — PlaneMode 1이면 2D 평면 X-Z, Y = PlaneDepth)
//   카메라 아래에 큰 사각형을 만들어 씬 깊이와 비교(쓰기 없음)하고, 주/보조 격자선과 월드 X/Y 축을 알파 블렌드로 그린다.
//   바닥면과 같은 높이에서 깜빡이지 않도록 래스터 위치는 시선 방향으로 아주 조금 카메라 쪽에 당긴다.

cbuffer GridConstants : register(b0)
{
	float4x4 ViewProjection;
	float3   CameraPosition;
	float    Extent;        // 사각형 반 크기
	float    MinorStep;     // 보조 격자 간격
	float    MajorStep;     // 주 격자 간격
	float    FadeDistance;  // 이 거리에서 완전히 사라짐
	float    PlaneMode;     // 0 = XY(Z = 0), 1 = 2D XZ (축별 간격·원점, 거리 페이드 없음)
	float2   Origin2D;      // 2D 격자 원점 (월드 X, Z — 타일맵 셀 경계)
	float    MinorStepV;    // 2D 세로 보조 간격 (가로는 MinorStep)
	float    MajorStepV;
	float2   PixelStep;     // 2D 픽셀 격자 간격 (0 = 없음)
	float    PlaneDepth;    // 2D 평면 깊이 (월드 Y)
	float    Padding0;
};

struct FGridVSOutput
{
	float4 Position : SV_Position;
	float3 World    : WORLD_POSITION;
};

static const float2 GCorners[6] = {
	float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
	float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f),
};

FGridVSOutput GridVS(uint VertexId : SV_VertexID)
{
	// 주 격자에 맞춰 스냅한 카메라 XY 중심 (정밀도 유지)
	float3 World;
	if (PlaneMode > 0.5f)
	{
		const float2 Center = floor(CameraPosition.xz / MajorStep) * MajorStep;
		const float2 Plane  = Center + GCorners[VertexId] * Extent;
		World               = float3(Plane.x, PlaneDepth, Plane.y);
	}
	else
	{
		const float2 Center = floor(CameraPosition.xy / MajorStep) * MajorStep;
		World               = float3(Center + GCorners[VertexId] * Extent, 0.0f);
	}
	const float3 Pulled = CameraPosition + (World - CameraPosition) * 0.999f;

	FGridVSOutput Output;
	Output.Position = mul(float4(Pulled, 1.0f), ViewProjection);
	Output.World    = World;
	return Output;
}

// 화면 공간 폭 LineWidth(픽셀)의 격자선 강도. OutDerivative: 픽셀당 격자 칸 수
float GridLine(float2 Coord, float LineWidth, out float OutDerivative)
{
	const float2 Derivative = fwidth(Coord);
	const float2 Distance   = abs(frac(Coord - 0.5f) - 0.5f) / max(Derivative, 1.0e-6f);
	OutDerivative           = max(Derivative.x, Derivative.y);
	return 1.0f - saturate(min(Distance.x, Distance.y) - (LineWidth - 1.0f) * 0.5f);
}

float AxisLine(float Value, float LineWidth)
{
	const float Derivative = max(fwidth(Value), 1.0e-6f);
	return 1.0f - saturate(abs(Value) / Derivative - (LineWidth - 1.0f) * 0.5f);
}

// 2D 평면 (X-Z): 축별 간격 격자 + 픽셀 격자(칸이 충분히 클 때만) + 축선 (Z = 0 빨강, X = 0 파랑). 거리 페이드 없음 (직교)
float4 GridPS2D(float3 World)
{
	const float2 P = World.xz - Origin2D;

	float MinorDerivative;
	float MajorDerivative;
	const float Minor = GridLine(P / float2(MinorStep, MinorStepV), 1.0f, MinorDerivative);
	const float Major = GridLine(P / float2(MajorStep, MajorStepV), 1.5f, MajorDerivative);
	const float MinorFade = 1.0f - saturate(MinorDerivative * 6.0f - 0.5f);
	const float MajorFade = 1.0f - saturate(MajorDerivative * 4.0f - 0.5f);

	float4 Color = float4(0.6f, 0.6f, 0.6f, 0.0f);
	Color.a      = max(Minor * MinorFade * 0.22f, Major * MajorFade * 0.45f);
	if (PixelStep.x > 0.0f && PixelStep.y > 0.0f)
	{
		float PixelDerivative;
		const float Pixel     = GridLine(P / PixelStep, 1.0f, PixelDerivative);
		const float PixelFade = 1.0f - saturate(PixelDerivative * 10.0f - 0.5f); // 도트 한 칸이 화면 10픽셀 넘게 클 때만
		Color.a               = max(Color.a, Pixel * PixelFade * 0.08f);
	}

	const float AxisX = AxisLine(World.z, 2.0f); // Z = 0 선 = X축
	const float AxisZ = AxisLine(World.x, 2.0f); // X = 0 선 = Z축
	if (AxisX > 0.0f || AxisZ > 0.0f)
	{
		const float3 AxisColor = AxisX >= AxisZ ? float3(0.85f, 0.12f, 0.1f) : float3(0.15f, 0.35f, 0.95f);
		const float  AxisAlpha = max(AxisX, AxisZ) * 0.9f;
		Color.rgb              = lerp(Color.rgb, AxisColor, saturate(AxisAlpha / max(Color.a + AxisAlpha, 1.0e-4f)));
		Color.a                = max(Color.a, AxisAlpha);
	}
	if (Color.a <= 0.001f)
	{
		discard;
	}
	return Color;
}

float4 GridPS(FGridVSOutput Input) : SV_Target
{
	if (PlaneMode > 0.5f)
	{
		return GridPS2D(Input.World);
	}
	const float2 P = Input.World.xy;

	float MinorDerivative;
	float MajorDerivative;
	const float Minor = GridLine(P / MinorStep, 1.0f, MinorDerivative);
	const float Major = GridLine(P / MajorStep, 1.5f, MajorDerivative);

	// 칸이 몇 픽셀 이하로 작아지면 모아레 대신 서서히 숨긴다
	const float MinorFade = 1.0f - saturate(MinorDerivative * 6.0f - 0.5f);
	const float MajorFade = 1.0f - saturate(MajorDerivative * 4.0f - 0.5f);

	const float Distance     = length(Input.World - CameraPosition);
	const float DistanceFade = pow(1.0f - saturate(Distance / FadeDistance), 2.0f);

	float4 Color = float4(0.55f, 0.55f, 0.55f, 0.0f);
	Color.a      = max(Minor * MinorFade * 0.14f, Major * MajorFade * 0.45f);

	// 월드 축: Y = 0 선은 X축(빨강), X = 0 선은 Y축(초록)
	const float AxisX = AxisLine(P.y, 2.0f);
	const float AxisY = AxisLine(P.x, 2.0f);
	if (AxisX > 0.0f || AxisY > 0.0f)
	{
		const float3 AxisColor = AxisX >= AxisY ? float3(0.85f, 0.12f, 0.1f) : float3(0.15f, 0.75f, 0.1f);
		const float  AxisAlpha = max(AxisX, AxisY) * 0.9f;
		Color.rgb              = lerp(Color.rgb, AxisColor, saturate(AxisAlpha / max(Color.a + AxisAlpha, 1.0e-4f)));
		Color.a                = max(Color.a, AxisAlpha);
	}

	Color.a *= DistanceFade;
	if (Color.a <= 0.001f)
	{
		discard;
	}
	return Color;
}
