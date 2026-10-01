#ifndef E_LIGHTING_HLSLI
#define E_LIGHTING_HLSLI

// 점광원/스포트라이트 + 클러스터 공용 식. Renderer/LightMath.h와 **같은 식**을 유지한다 (테스트 LightTests)

// ShaderTypes.h FLocalLightGpuData와 1:1
struct FLocalLight
{
	float3 Position;
	float  Radius;
	float3 Color;      // 선형 색 × 강도
	float  ConeScale;
	float3 Direction;
	float  ConeOffset;
	int    ShadowIndex; // -1 = 그림자 없음
	uint   Type;        // 0 점광원, 1 스포트
	float  ShadowTexelFactor;
	float  Padding0;
};

// ShaderTypes.h FClusterConstants와 1:1. 레지스터는 포함하는 쪽이 E_CLUSTER_CONSTANTS_REGISTER로 바꿀 수 있다 (메시 b5, 컬링 b0)
#ifndef E_CLUSTER_CONSTANTS_REGISTER
#define E_CLUSTER_CONSTANTS_REGISTER b5
#endif
cbuffer ClusterConstants : register(E_CLUSTER_CONSTANTS_REGISTER)
{
	float4x4 ClusterView;
	uint     ClusterGridX;
	uint     ClusterGridY;
	uint     ClusterGridZ;
	uint     LocalLightCount;
	float2   ClusterScreenSize;
	float    ClusterSliceScale;
	float    ClusterSliceBias;
	float    ClusterNearZ;
	float    ClusterFarZ;
	float    ClusterProjScaleX;
	float    ClusterProjScaleY;
	uint     ClusterOrthographic;
	float    LocalShadowNormalOffset; // 그림자 텍셀 배수
	float    LocalShadowTexelSize;    // 1 / 그림자 타일 해상도
	uint     ClusterPadding0;
};

static const uint E_CLUSTER_STRIDE = 64; // LightMath::ClusterStride: [0] = 개수, [1..63] = 라이트 인덱스
static const float E_LIGHT_REFERENCE_DISTANCE = 100.0f; // LightMath::ReferenceDistance (cm)
static const float E_LIGHT_MIN_DISTANCE       = 10.0f;  // LightMath::MinDistance

float LightDistanceWindow(float Distance, float Radius)
{
	if (Radius <= 0.0f)
	{
		return 0.0f;
	}
	const float Ratio  = Distance / Radius;
	const float Ratio2 = Ratio * Ratio;
	const float Window = saturate(1.0f - Ratio2 * Ratio2);
	return Window * Window;
}

float LightInverseSquare(float Distance)
{
	const float Scaled = max(Distance, E_LIGHT_MIN_DISTANCE) / E_LIGHT_REFERENCE_DISTANCE;
	return 1.0f / (Scaled * Scaled);
}

float LightDistanceAttenuation(float Distance, float Radius)
{
	return LightInverseSquare(Distance) * LightDistanceWindow(Distance, Radius);
}

float LightConeAttenuation(float CosAngle, float ConeScale, float ConeOffset)
{
	const float T = saturate(CosAngle * ConeScale + ConeOffset);
	return T * T;
}

// 조각 경계 깊이: Near * (Far / Near)^(Slice / SliceCount)
float ClusterSliceToDepth(uint Slice, float NearZ, float FarZ, uint SliceCount)
{
	return NearZ * pow(FarZ / NearZ, (float)Slice / (float)SliceCount);
}

uint ClusterDepthToSlice(float ViewDepth, float SliceScale, float SliceBias, uint SliceCount)
{
	const float Slice = floor(log(max(ViewDepth, 1.0e-3f)) * SliceScale + SliceBias);
	return (uint)clamp(Slice, 0.0f, (float)(SliceCount - 1));
}

// LightMath::SelectCubeFace (0..5 = +X,-X,+Y,-Y,+Z,-Z)
uint SelectCubeFace(float3 Direction)
{
	const float3 A = abs(Direction);
	if (A.x >= A.y && A.x >= A.z)
	{
		return Direction.x >= 0.0f ? 0u : 1u;
	}
	if (A.y >= A.z)
	{
		return Direction.y >= 0.0f ? 2u : 3u;
	}
	return Direction.z >= 0.0f ? 4u : 5u;
}

#endif // E_LIGHTING_HLSLI
