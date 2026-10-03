#ifndef E_AREA_LIGHT_HLSLI
#define E_AREA_LIGHT_HLSLI

// 면광원 LTC + IES + 쿠키 (Phase 52). Renderer/AreaLightMath.h와 **같은 식**을 유지한다 (테스트 AreaLight_*, Ies_*, LightCookie_*).
// 포함하기 전에: Lighting.hlsli, PBR.hlsli, 그리고 텍스처 접근 매크로
//   E_LIGHT_TEXTURE(Index)  : 셰이더 가시 힙 칸 → Texture2D (메시 = 공간 3 무제한 표, DXR = BindlessTextures)
//   E_LIGHT_SAMPLER_CLAMP   : 선형 클램프 (LTC 표, IES)
//   E_LIGHT_SAMPLER_WRAP    : 반복 (쿠키)
// LTC 표 칸은 FClusterConstants LtcTexture1/2 (메시 패스 b5) — DXR 히트는 LTC를 쓰지 않고 대표점 근사(Lighting.hlsli AreaLightApproxAttenuation)

static const float E_LTC_SCALE = 63.0f / 64.0f; // AreaLightMath::LtcTableSize
static const float E_LTC_BIAS  = 0.5f / 64.0f;
static const uint  E_AREA_MAX_VERTICES = 8;   // AreaLightMath::MaxPolygonVertices (원판 = 정 8각형)
static const float E_DISC_POLYGON_SCALE = 1.05390736f; // AreaLightMath::DiscPolygonScale

// 모서리 형태 계수 벡터 (AreaLightMath::IntegrateEdgeVector, Heitz 2017 유리 근사)
float3 LtcIntegrateEdgeVector(float3 A, float3 B)
{
	const float X             = dot(A, B);
	const float Y             = abs(X);
	const float NumA          = 0.8543985f + (0.4965155f + 0.0145206f * Y) * Y;
	const float NumB          = 3.4175940f + (4.1616724f + Y) * Y;
	const float V             = NumA / NumB;
	const float ThetaSinTheta = X > 0.0f ? V : 0.5f * rsqrt(max(1.0f - X * X, 1.0e-7f)) - V;
	return cross(A, B) * ThetaSinTheta;
}

// 구 근사 지평선 잘림 (AreaLightMath::ClipFormFactor): 표 2 .w
float LtcClipFormFactor(float3 FormFactor, bool bFlipZ)
{
	const float Length = length(FormFactor);
	if (Length <= 1.0e-7f)
	{
		return 0.0f;
	}
	float Z = FormFactor.z / Length;
	Z       = bFlipZ ? -Z : Z;
	const float2 UV = float2(Z * 0.5f + 0.5f, saturate(Length)) * E_LTC_SCALE + E_LTC_BIAS;
	return Length * E_LIGHT_TEXTURE(LtcTexture2).SampleLevel(E_LIGHT_SAMPLER_CLAMP, UV, 0).w;
}

// 면광원 다각형 (AreaLightMath::MakePolygon): 사각형 4점 / 원판 8점. 반환 = 꼭짓점 수
uint MakeAreaPolygon(FLocalLight Light, out float3 Points[E_AREA_MAX_VERTICES])
{
	const float3 R = Light.Right * Light.HalfWidth;
	const float3 U = Light.Up * Light.HalfHeight;
	if (Light.Type == E_LOCAL_LIGHT_DISC)
	{
		// Up에서 Right 쪽으로 45°씩 (cos, sin)
		static const float2 Ring[8] = { float2(1.0f, 0.0f), float2(0.70710678f, 0.70710678f), float2(0.0f, 1.0f), float2(-0.70710678f, 0.70710678f),
		                                float2(-1.0f, 0.0f), float2(-0.70710678f, -0.70710678f), float2(0.0f, -1.0f), float2(0.70710678f, -0.70710678f) };
		[unroll]
		for (uint Index = 0; Index < 8; ++Index)
		{
			Points[Index] = Light.Position + (U * Ring[Index].x + R * Ring[Index].y) * E_DISC_POLYGON_SCALE;
		}
		return 8;
	}
	Points[0] = Light.Position - R - U;
	Points[1] = Light.Position - R + U;
	Points[2] = Light.Position + R + U;
	Points[3] = Light.Position + R - U;
	[unroll]
	for (uint Rest = 4; Rest < 8; ++Rest)
	{
		Points[Rest] = Points[3];
	}
	return 4;
}

// 다각형 LTC 적분 (AreaLightMath::EvaluatePolygon). Inverse = (M00, M20, M02, M22): x' = M00·x + M02·z, z' = M20·x + M22·z
float LtcEvaluatePolygon(float3 N, float3 T1, float3 T2, float3 P, float4 Inverse, float3 Points[E_AREA_MAX_VERTICES], uint Count, bool bBehind)
{
	float3 L[E_AREA_MAX_VERTICES];
	[unroll]
	for (uint Index = 0; Index < E_AREA_MAX_VERTICES; ++Index)
	{
		const float3 W     = Points[Index] - P;
		const float3 Local = float3(dot(W, T1), dot(W, T2), dot(W, N));
		const float3 T     = float3(Inverse.x * Local.x + Inverse.z * Local.z, Local.y, Inverse.y * Local.x + Inverse.w * Local.z);
		L[Index]           = normalize(T);
	}
	float3 Sum = 0.0f;
	[unroll]
	for (uint Edge = 0; Edge < E_AREA_MAX_VERTICES; ++Edge)
	{
		if (Edge < Count)
		{
			const uint Next = Edge + 1 < Count ? Edge + 1 : 0;
			Sum += LtcIntegrateEdgeVector(L[Edge], L[Next]);
		}
	}
	return LtcClipFormFactor(Sum, bBehind);
}

// IES × 쿠키 (Local = 라이트 → 표면 방향 (Forward, Right, Up), 정규화). 둘 다 없으면 1
float3 EvaluateLightProfile(FLocalLight Light, float3 Local)
{
	float3 Result = 1.0f;
	if (Light.IesTexture >= 0)
	{
		Result *= E_LIGHT_TEXTURE(Light.IesTexture).SampleLevel(E_LIGHT_SAMPLER_CLAMP, ComputeIesUV(Local), 0).r;
	}
	if (Light.CookieTexture >= 0)
	{
		bool         bValid;
		const float2 UV = ComputeCookieUV(Light, Local, bValid);
		// 픽셀마다 다른 라이트를 도는 루프 안이라 미분이 없다 → 밉 0 (먼 곳 자글거림은 TAA가 다듬는다)
		Result *= bValid ? E_LIGHT_TEXTURE(Light.CookieTexture).SampleLevel(E_LIGHT_SAMPLER_WRAP, UV, 0).rgb : 0.0f;
	}
	return Result;
}

// 면광원 하나의 직접광 (그림자 제외). Surface.V = 표면 → 카메라
float3 EvaluateAreaLight(FLocalLight Light, FSurface Surface, float3 WorldPosition)
{
	const float3 FromLight = WorldPosition - Light.Position;
	const float3 Local     = ToLightLocal(Light, FromLight);
	const bool   bTwoSided = (Light.Flags & E_LOCAL_LIGHT_FLAG_TWO_SIDED) != 0;
	const bool   bBehind   = Local.x < 0.0f;
	if (bBehind && !bTwoSided)
	{
		return 0.0f;
	}
	const float Window = LightDistanceWindow(DistanceToAreaLight(Light, Local), Light.Radius);
	if (Window <= 0.0f)
	{
		return 0.0f;
	}
	const float  Distance = length(FromLight);
	const float3 LocalDir = Local / max(Distance, 1.0e-4f);
	// 문 덮개: 가운데 → 표면 방향 원뿔 (양면 뒤쪽은 뒤집은 법선 기준)
	const float  Cone     = LightConeAttenuation(abs(LocalDir.x), Light.ConeScale, Light.ConeOffset);
	if (Cone <= 0.0f)
	{
		return 0.0f;
	}
	const float3 Profile = EvaluateLightProfile(Light, LocalDir) * (Window * Cone);
	if (all(Profile <= 0.0f))
	{
		return 0.0f;
	}

	// (T1, T2, N) 기저: T1 = 시선의 접평면 성분 (시선 = 법선이면 임의 접선)
	const float3 N     = Surface.N;
	float3       T1    = Surface.V - N * dot(Surface.V, N);
	const float  T1Len = length(T1);
	T1                 = T1Len > 1.0e-5f ? T1 / T1Len : normalize(abs(N.z) < 0.999f ? cross(N, float3(0.0f, 0.0f, 1.0f)) : float3(1.0f, 0.0f, 0.0f));
	const float3 T2    = cross(N, T1);

	float3     Points[E_AREA_MAX_VERTICES];
	const uint Count = MakeAreaPolygon(Light, Points);

	const float  NdotV    = clamp(dot(N, Surface.V), 1.0e-4f, 1.0f);
	const float2 UV       = float2(saturate(Surface.Roughness), sqrt(saturate(1.0f - NdotV))) * E_LTC_SCALE + E_LTC_BIAS;
	const float4 Table1   = E_LIGHT_TEXTURE(LtcTexture1).SampleLevel(E_LIGHT_SAMPLER_CLAMP, UV, 0);
	const float4 Table2   = E_LIGHT_TEXTURE(LtcTexture2).SampleLevel(E_LIGHT_SAMPLER_CLAMP, UV, 0);
	const float  Diffuse  = LtcEvaluatePolygon(N, T1, T2, WorldPosition, float4(1.0f, 0.0f, 0.0f, 1.0f), Points, Count, bBehind);
	const float  Specular = LtcEvaluatePolygon(N, T1, T2, WorldPosition, Table1, Points, Count, bBehind);
	const float3 F0       = GetF0(Surface);
	const float3 SpecularColor = F0 * Table2.x + (1.0f - F0) * Table2.y;
	return Light.Color * Profile * (Diffuse * Surface.Albedo * (1.0f - Surface.Metallic) + Specular * SpecularColor);
}

// 방향광 쿠키 (FClusterConstants DirectionalCookie*): 빛에 수직인 평면 좌표. 미분이 있는 곳(픽셀 셰이더 본 흐름)에서만 부른다
float3 EvaluateDirectionalCookie(float3 WorldPosition, float MipBias)
{
	const float2 UV = float2(dot(WorldPosition, DirectionalCookieU.xyz) + DirectionalCookieU.w, dot(WorldPosition, DirectionalCookieV.xyz) + DirectionalCookieV.w);
	return E_LIGHT_TEXTURE(DirectionalCookieTexture).SampleBias(E_LIGHT_SAMPLER_WRAP, UV, MipBias).rgb;
}

#endif // E_AREA_LIGHT_HLSLI
