#include "RayTracingLighting.hlsli"
#include "RayTracingView.hlsli"
#include "Fullscreen.hlsli"

// 레이 트레이싱 디버그 (--debug-view rt-instances, FRayTracingEffects): 카메라 광선을 TLAS로 직접 추적해 TLAS 내용을 보여 준다.
//   RtDebugMode 0 = 인스턴스 번호 색 × 면 각, 1 = 히트 머티리얼 알베도(바인드리스 평가 확인), 2 = 히트 조명(반사가 보는 값)

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSInstances(FFullscreenVSOutput Input) : SV_Target
{
	const float2 UV        = (floor(Input.Position.xy) + 0.5f) / RtScreenSize;
	const float3 Far       = ReconstructWorldPosition(UV, 1.0f);
	const float3 Near      = ReconstructWorldPosition(UV, 0.0f);
	const float3 Direction = normalize(Far - Near);
	RayDesc Ray;
	Ray.Origin    = RtOrthographic != 0 ? Near : RtCameraPosition;
	Ray.Direction = Direction;
	Ray.TMin      = 0.0f;
	Ray.TMax      = RtMaxDistance;
	const FRayHit Hit = TraceClosestHitCullBack(Ray, E_RT_MASK_TYPES);
	if (!Hit.bHit)
	{
		return float4(0.05f, 0.05f, 0.08f, 1.0f);
	}
	const float       ConeWidth = Hit.T / max(RtProjectionScale, 1.0e-3f);
	const FHitSurface Surface   = LoadHitSurface(Hit, ConeWidth, -Direction);
	if (RtDebugMode == 1)
	{
		return float4(Surface.Albedo, 1.0f);
	}
	if (RtDebugMode == 2)
	{
		return float4(EvaluateHitLighting(Surface, -Direction), 1.0f);
	}
	const float Facing = abs(dot(Surface.GeometricNormal, Direction));
	return float4(HashInstanceColor(Hit.Instance) * (0.35f + 0.65f * Facing), 1.0f);
}