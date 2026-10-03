#define E_CLUSTER_CONSTANTS_REGISTER b0
#include "Common.hlsli"
#include "Lighting.hlsli"

// 클러스터 라이트 컬링: 클러스터(화면 타일 x 로그 깊이 조각) 하나당 스레드 하나가 모든 라이트의 경계 구(LocalLightBoundingRadius)를 뷰 공간 AABB와 비교한다.
// 결과 ClusterData[클러스터 * 64] = 개수, 뒤 63칸 = 라이트 인덱스 (넘치면 버림). 식은 LightMath::ComputeClusterViewBounds와 같다

StructuredBuffer<FLocalLight> LocalLights : register(t0);
RWStructuredBuffer<uint>      ClusterData : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 DispatchId : SV_DispatchThreadID)
{
	const uint Cluster      = DispatchId.x;
	const uint ClusterCount = ClusterGridX * ClusterGridY * ClusterGridZ;
	if (Cluster >= ClusterCount)
	{
		return;
	}
	const uint TileX = Cluster % ClusterGridX;
	const uint TileY = (Cluster / ClusterGridX) % ClusterGridY;
	const uint Slice = Cluster / (ClusterGridX * ClusterGridY);

	// 뷰 공간 AABB (타일 Y = 0은 화면 위쪽, 마지막 조각은 원평면 너머까지)
	const float NdcMinX = -1.0f + 2.0f * (float)TileX / (float)ClusterGridX;
	const float NdcMaxX = -1.0f + 2.0f * (float)(TileX + 1) / (float)ClusterGridX;
	const float NdcMaxY = 1.0f - 2.0f * (float)TileY / (float)ClusterGridY;
	const float NdcMinY = 1.0f - 2.0f * (float)(TileY + 1) / (float)ClusterGridY;
	const float ZNear   = Slice == 0 ? 0.0f : ClusterSliceToDepth(Slice, ClusterNearZ, ClusterFarZ, ClusterGridZ);
	const float ZFar    = Slice + 1 >= ClusterGridZ ? ClusterFarZ * 1.0e3f : ClusterSliceToDepth(Slice + 1, ClusterNearZ, ClusterFarZ, ClusterGridZ);

	float3 BoxMin = float3(1.0e30f, 1.0e30f, ZNear);
	float3 BoxMax = float3(-1.0e30f, -1.0e30f, ZFar);
	[unroll]
	for (uint Corner = 0; Corner < 2; ++Corner)
	{
		const float Z      = Corner == 0 ? ZNear : ZFar;
		const float ScaleX = ClusterOrthographic != 0 ? ClusterProjScaleX : Z * ClusterProjScaleX;
		const float ScaleY = ClusterOrthographic != 0 ? ClusterProjScaleY : Z * ClusterProjScaleY;
		BoxMin.xy = min(BoxMin.xy, float2(NdcMinX * ScaleX, NdcMinY * ScaleY));
		BoxMax.xy = max(BoxMax.xy, float2(NdcMaxX * ScaleX, NdcMaxY * ScaleY));
	}

	const uint Base  = Cluster * E_CLUSTER_STRIDE;
	uint       Count = 0;
	for (uint Index = 0; Index < LocalLightCount && Count < E_CLUSTER_STRIDE - 1; ++Index)
	{
		const FLocalLight Light   = LocalLights[Index];
		const float3      Center  = mul(float4(Light.Position, 1.0f), ClusterView).xyz;
		const float3      Closest = clamp(Center, BoxMin, BoxMax);
		const float3      Delta   = Closest - Center;
		const float       Bounds  = LocalLightBoundingRadius(Light); // 면광원 = 영향 반경 + 면 반 대각선 (점/스포트 = Radius)
		if (dot(Delta, Delta) <= Bounds * Bounds)
		{
			ClusterData[Base + 1 + Count] = Index;
			++Count;
		}
	}
	ClusterData[Base] = Count;
}
