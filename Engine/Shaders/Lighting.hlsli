#ifndef E_LIGHTING_HLSLI
#define E_LIGHTING_HLSLI

// 점광원/스포트라이트/면광원 + 클러스터 공용 식. Renderer/LightMath.h, Renderer/AreaLightMath.h와 **같은 식**을 유지한다
// (테스트 LightTests, AreaLight_*/Ies_*/LightCookie_*). 텍스처가 필요한 면광원 LTC·IES·쿠키 평가는 AreaLight.hlsli

// LightMath::ELocalLightType / LocalLightFlag_* (번호는 끝에만 추가)
static const uint E_LOCAL_LIGHT_POINT = 0;
static const uint E_LOCAL_LIGHT_SPOT  = 1;
static const uint E_LOCAL_LIGHT_RECT  = 2;
static const uint E_LOCAL_LIGHT_DISC  = 3;
static const uint E_LOCAL_LIGHT_FLAG_TWO_SIDED = 1u;

// ShaderTypes.h FLocalLightGpuData와 1:1 (128바이트)
struct FLocalLight
{
	float3 Position;
	float  Radius;
	float3 Color;      // 선형 색 × 강도 (면광원은 휘도)
	float  ConeScale;
	float3 Direction;  // 스포트 축 / 면광원 법선 / IES 천저
	float  ConeOffset;
	int    ShadowIndex; // -1 = 그림자 없음
	uint   Type;        // E_LOCAL_LIGHT_*
	float  ShadowTexelFactor;
	float  SourceRadius; // 면광원 반그림자 크기 (cm)
	float3 Right;        // 면 가로축 / IES φ = 0
	float  HalfWidth;
	float3 Up;           // 면 세로축
	float  HalfHeight;
	int    IesTexture;    // 셰이더 가시 힙 칸 (-1 = 없음)
	int    CookieTexture; // 셰이더 가시 힙 칸 (-1 = 없음)
	uint   Flags;
	float  ShadowFar;
	float4 CookieTransform; // uv = 투영 좌표 × xy + zw
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
	// Phase 52 (뒤에 덧붙임)
	uint     LtcTexture1;             // 셰이더 가시 힙 칸 (AreaLight.hlsli)
	uint     LtcTexture2;
	float    LocalShadowNearZ;        // 로컬 그림자 근평면 (cm)
	int      DirectionalCookieTexture; // -1 = 없음
	float4   DirectionalCookieU;      // u = dot(P, xyz) + w
	float4   DirectionalCookieV;
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

// 로컬 그림자 원근 깊이 [0, 1] → 뷰 깊이 (AreaLightMath::LinearizeShadowDepth — 근평면 = LocalShadowNearZ)
float LinearizeLocalShadowDepth(float DeviceDepth, float FarZ)
{
	return LocalShadowNearZ * FarZ / max(FarZ - DeviceDepth * (FarZ - LocalShadowNearZ), 1.0e-4f);
}

bool IsAreaLight(FLocalLight Light)
{
	return Light.Type >= E_LOCAL_LIGHT_RECT;
}

// 경계 구 반지름 (AreaLightMath::ComputeBoundingRadius): 점/스포트 = Radius
float LocalLightBoundingRadius(FLocalLight Light)
{
	if (Light.Type == E_LOCAL_LIGHT_RECT)
	{
		return Light.Radius + sqrt(Light.HalfWidth * Light.HalfWidth + Light.HalfHeight * Light.HalfHeight);
	}
	if (Light.Type == E_LOCAL_LIGHT_DISC)
	{
		return Light.Radius + max(Light.HalfWidth, Light.HalfHeight);
	}
	return Light.Radius;
}

// 라이트 로컬 성분 (Forward, Right, Up)
float3 ToLightLocal(FLocalLight Light, float3 V)
{
	return float3(dot(V, Light.Direction), dot(V, Light.Right), dot(V, Light.Up));
}

// 면에서 가장 가까운 점 → 표면 벡터 (AreaLightMath::ComputeAreaOffset). Local = 가운데 기준 (Forward, Right, Up)
float3 AreaLightOffset(FLocalLight Light, float3 Local)
{
	float2 YZ = Local.yz;
	if (Light.Type == E_LOCAL_LIGHT_DISC)
	{
		const float2 N = YZ / max(float2(Light.HalfWidth, Light.HalfHeight), 1.0e-3f);
		const float  R = sqrt(dot(N, N));
		if (R > 1.0f)
		{
			YZ /= R;
		}
	}
	else
	{
		YZ = clamp(YZ, -float2(Light.HalfWidth, Light.HalfHeight), float2(Light.HalfWidth, Light.HalfHeight));
	}
	return float3(Local.x, Local.yz - YZ);
}

// 면에서 가장 가까운 점까지 거리 (AreaLightMath::DistanceToArea)
float DistanceToAreaLight(FLocalLight Light, float3 Local)
{
	const float3 Offset = AreaLightOffset(Light, Local);
	return sqrt(Offset.x * Offset.x + dot(Offset.yz, Offset.yz));
}

// 문 덮개 원뿔 코사인 (AreaLightMath::ComputeBarnDoorCos): |면 법선 · (가장 가까운 점 → 표면 방향)|
float AreaLightBarnDoorCos(FLocalLight Light, float3 Local)
{
	const float3 Offset = AreaLightOffset(Light, Local);
	return abs(Offset.x) / max(sqrt(Offset.x * Offset.x + dot(Offset.yz, Offset.yz)), 1.0e-4f);
}

float AreaLightArea(FLocalLight Light)
{
	return Light.Type == E_LOCAL_LIGHT_DISC ? 3.14159265f * Light.HalfWidth * Light.HalfHeight : 4.0f * Light.HalfWidth * Light.HalfHeight;
}

// 원거리 근사 (RT 히트·볼류메트릭 안개, AreaLightMath::ComputeApproxAreaFactor): 가운데 대표점 감쇠에 곱하는 값.
// CosEmit = dot(면 법선, 가운데 → 표면 방향)
float AreaLightApproxFactor(FLocalLight Light, float CosEmit)
{
	const float Cos = (Light.Flags & E_LOCAL_LIGHT_FLAG_TWO_SIDED) != 0 ? abs(CosEmit) : max(CosEmit, 0.0f);
	return Cos * max(AreaLightArea(Light), 1.0f) / (E_LIGHT_REFERENCE_DISTANCE * E_LIGHT_REFERENCE_DISTANCE);
}

// 대표점 감쇠 (면광원 전용 — 텍스처 없이): 감쇠 창은 면 거리, 역제곱은 가운데 거리(면 반 크기 이상), 문 덮개 원뿔(가장 가까운 점 기준), 면 코사인.
// L = 표면 → 가운데 (정규화). 반환 0이면 기여 없음
float AreaLightApproxAttenuation(FLocalLight Light, float3 WorldPosition, out float3 L)
{
	const float3 FromLight = WorldPosition - Light.Position;
	const float  Distance  = length(FromLight);
	L                      = -FromLight / max(Distance, 1.0e-4f);
	const float3 Local     = ToLightLocal(Light, FromLight);
	const float  Window    = LightDistanceWindow(DistanceToAreaLight(Light, Local), Light.Radius);
	if (Window <= 0.0f)
	{
		return 0.0f;
	}
	const float CosEmit = dot(Light.Direction, -L);
	return Window * LightInverseSquare(max(Distance, Light.SourceRadius)) *
	       LightConeAttenuation(AreaLightBarnDoorCos(Light, Local), Light.ConeScale, Light.ConeOffset) * AreaLightApproxFactor(Light, CosEmit);
}

// IES 좌표 (AreaLightMath::ComputeIesUV): Local = 라이트 → 표면 (Forward, Right, Up), 정규화. 텍스처 64(θ) × 32(φ)
float2 ComputeIesUV(float3 Local)
{
	const float Theta = acos(clamp(Local.x, -1.0f, 1.0f));
	float       Phi   = atan2(Local.z, Local.y);
	Phi               = Phi < 0.0f ? Phi + 6.28318531f : Phi;
	return float2((Theta / 3.14159265f * 63.0f + 0.5f) / 64.0f, (Phi / 6.28318531f * 31.0f + 0.5f) / 32.0f);
}

// 쿠키 uv (AreaLightMath::ComputeCookieUV). 원근 종류에서 뒤쪽이면 bValid = false
float2 ComputeCookieUV(FLocalLight Light, float3 Local, out bool bValid)
{
	bValid = true;
	if (Light.Type == E_LOCAL_LIGHT_POINT)
	{
		const float Theta = acos(clamp(Local.x, -1.0f, 1.0f));
		float       Phi   = atan2(Local.z, Local.y);
		Phi               = Phi < 0.0f ? Phi + 6.28318531f : Phi;
		return float2(Phi / 6.28318531f, Theta / 3.14159265f) * Light.CookieTransform.xy + Light.CookieTransform.zw;
	}
	if (Local.x <= 1.0e-4f)
	{
		bValid = false;
		return 0.0f;
	}
	return Local.yz / Local.x * Light.CookieTransform.xy + Light.CookieTransform.zw;
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
