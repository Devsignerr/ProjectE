#ifndef E_RAY_TRACING_VIEW_HLSLI
#define E_RAY_TRACING_VIEW_HLSLI

// 레이 트레이싱 화면 패스 공용 뷰 상수 b0 (RayTracingEffects.cpp FRayTracingViewConstants와 1:1) + 깊이 → 월드 재구성
cbuffer RayTracingView : register(b0)
{
	float4x4 RtInvViewProjection;   // 지터 포함 투영(깊이를 그린 투영)의 역: 깊이 → 월드
	float4x4 RtViewProjection;      // 지터 없음 (현재 프레임)
	float4x4 RtPrevViewProjection;  // 지터 없음 (이전 프레임 — 이력 없으면 현재)
	float3   RtCameraPosition;
	uint     RtOrthographic;
	float3   RtCameraForward;
	float    RtProjectionScale;     // 투영[1][1] × 높이/2 (원근: 픽셀/거리 × 뷰 깊이, 직교: 픽셀/cm)
	float2   RtScreenSize;          // 씬(내부) 해상도
	uint     RtFrameIndex;          // 교차 표본 회전 (정지 화면 결정적)
	uint     RtDecals;              // 1 = DBuffer B/C를 법선·거칠기에 적용 (반사)
	float    RtNormalBias;          // 1차 표면 바이어스 배율 (RayTracingMath::ComputeSurfaceBias)
	float    RtMaxDistance;         // 광선 최대 거리 (cm)
	float    RtMaxRoughness;        // 반사: 이보다 거친 픽셀은 추적하지 않음 (캡처/하늘)
	float    RtMaxBlurRadius;       // 반사 흐림 반경 상한 (픽셀)
	float    RtSunTanHalfAngle;     // 그림자: 태양 원반 반각 tan
	float    RtMinFilterRadius;     // 그림자 공간 필터 최소 반경 (픽셀 — 4x4 교차 패턴을 지움)
	float    RtMaxFilterRadius;     // 그림자 공간 필터 최대 반경 (픽셀)
	float    RtHistoryWeight;       // 그림자 시간 누적: 이번 프레임 비중
	uint     RtHistoryValid;
	uint     RtDebugMode;           // 디버그 패스: 0 인스턴스 색, 1 히트 알베도, 2 히트 조명
	float2   RtViewPadding;
};

float3 ReconstructWorldPosition(float2 UV, float Depth)
{
	const float4 P = mul(float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, Depth, 1.0f), RtInvViewProjection);
	return P.xyz / P.w;
}

float ComputeViewDepth(float3 WorldPosition)
{
	return dot(WorldPosition - RtCameraPosition, RtCameraForward);
}

// 카메라 → 점 방향 (직교면 카메라 앞)
float3 ComputeViewDirection(float3 WorldPosition)
{
	return RtOrthographic != 0 ? RtCameraForward : normalize(WorldPosition - RtCameraPosition);
}

// 월드 점 → UV (행렬 = 지터 없는 현재/이전 뷰-투영). w ≤ 0이면 -1
float2 ProjectToUV(float3 WorldPosition, float4x4 ViewProjection)
{
	const float4 Clip = mul(float4(WorldPosition, 1.0f), ViewProjection);
	if (Clip.w <= 1.0e-5f)
	{
		return -1.0f;
	}
	return float2(Clip.x / Clip.w * 0.5f + 0.5f, 0.5f - Clip.y / Clip.w * 0.5f);
}

#endif // E_RAY_TRACING_VIEW_HLSLI