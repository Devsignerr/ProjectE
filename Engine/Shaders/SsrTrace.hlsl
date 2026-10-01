#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// SSR 추적 (FScreenSpaceReflections, FScreenPassRootSignature). Hi-Z는 ScreenSpaceReflections.hlsl이 만든다. 식은 Renderer/ReflectionMath.h
//   출력 R16G16B16A16_FLOAT: rgb = 반사 색 (이전 프레임 씬 컬러, 톤매핑 전 HDR), a = 신뢰도 (0 = 맞지 않음 → 캡처/하늘 IBL)
//   SV_Target1 = (반사 움직임 벡터 xy, 흐림 반경 픽셀 z). 반사는 거울 방향 한 번만 추적하고(결정적 — 프레임마다 바뀌는 노이즈 없음)
//   거칠기는 SsrResolve.hlsl이 GGX 반사 원뿔 크기만큼 화면에서 흐려 표현한다 (식은 ReflectionMath::ComputeSsrBlurRadiusPixels).
//   예전 확률 반사(픽셀·프레임마다 GGX 방향 하나 + TAA 누적)는 광선 하나의 분산이 커서 누적해도 TV 노이즈처럼 지글거렸다

cbuffer SsrConstants : register(b0)
{
	float4x4 Projection;      // 깊이를 그린 투영 (지터 포함)
	float4x4 InvProjection;
	float4x4 View;            // 월드 → 뷰 (법선)
	float4x4 Reprojection;    // 현재 클립 → 이전 클립 (지터 없음)
	float2   ScreenSize;
	uint     HizMipCount;
	uint     MaxIterations;
	float    MaxDistance;     // cm
	float    Thickness;       // cm (교차 뒤 허용 두께)
	float    NearZ;
	uint     bOrthographic;
	float    ProjectionScale; // 투영[1][1] × 높이/2 (픽셀/거리 — 원근은 뷰 깊이로 더 나눈다)
	float    MaxRoughness;    // 이보다 거친 픽셀은 추적하지 않음
	float    MaxBlurRadius;   // 거칠기 흐림 반경 상한 (픽셀)
	uint     bDecals;         // 1 = 데칼 DBuffer(t4, t5)를 법선/거칠기에 적용
};

Texture2D<float>  SceneDepth     : register(t0);
Texture2D<float>  Hiz            : register(t1); // 칸마다 가장 가까운 깊이 (밉 체인)
Texture2D<float4> SceneNormal    : register(t2);
Texture2D<float4> PrevSceneColor : register(t3); // 이전 프레임 씬 컬러 (메인 패스 전이라 아직 지난 프레임 내용)
Texture2D<float4> DecalNormal    : register(t4); // DBufferB
Texture2D<float4> DecalMaterial  : register(t5); // DBufferC
SamplerState      LinearSampler  : register(s0);

static const float SsrFloatMax = 3.402823466e+38f;

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float3 ViewFromDepth(float2 UV, float Depth)
{
	const float4 P = mul(float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, Depth, 1.0f), InvProjection);
	return P.xyz / P.w;
}

float3 ScreenFromView(float3 P)
{
	const float4 Clip = mul(float4(P, 1.0f), Projection);
	const float3 Ndc  = Clip.xyz / Clip.w;
	return float3(Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f, Ndc.z);
}

// 밉 칸 수 (UV → 칸 변환용). 칸 크기는 정확히 2^밉 픽셀이므로 내림하지 않는다 — 실제 밉 크기 floor(크기/2^밉)로 나누면
//   크기가 2^밉의 배수가 아닐 때 칸이 점점 밀려 다른 픽셀 영역의 최소 깊이를 읽고 광선이 물체를 뚫고 지나간다.
//   화면 끝의 남는 칸은 마지막 실제 칸으로 자른다 (Hi-Z 내리기가 홀수 크기의 나머지를 마지막 칸에 넣는다)
float2 MipResolution(uint Mip)
{
	return ScreenSize / exp2((float)Mip);
}

// FidelityFX SSSR 계층 추적을 표준 깊이(가까울수록 작음, Hi-Z = 칸의 최소 깊이)로 옮긴 것.
//   광선이 칸의 가장 가까운 면보다 앞이면 칸 경계(또는 그 면 깊이)까지 건너뛰고 거친 밉으로, 아니면 고운 밉으로 내려간다
bool AdvanceRay(float3 Origin, float3 Direction, float3 InvDirection, float2 MipPosition, float2 MipResolutionInv, float2 FloorOffset,
                float2 UvOffset, float SurfaceZ, inout float3 Position, inout float T)
{
	float2 XyPlane = floor(MipPosition) + FloorOffset;
	XyPlane        = XyPlane * MipResolutionInv + UvOffset;
	float3 TPlanes = (float3(XyPlane, SurfaceZ) - Origin) * InvDirection;
	TPlanes.z      = Direction.z > 0.0f ? TPlanes.z : SsrFloatMax; // 멀어지는 광선만 면 깊이 평면과 만난다
	const float TMin          = min(min(TPlanes.x, TPlanes.y), TPlanes.z);
	const bool  bAboveSurface = Position.z < SurfaceZ;
	const bool  bSkippedTile  = asuint(TMin) != asuint(TPlanes.z) && bAboveSurface;
	T                         = bAboveSurface ? TMin : T;
	Position                  = Origin + T * Direction;
	return bSkippedTile;
}

float3 HierarchicalRaymarch(float3 Origin, float3 Direction, out bool bValid)
{
	const float3 InvDirection = float3(Direction.x != 0.0f ? 1.0f / Direction.x : SsrFloatMax, Direction.y != 0.0f ? 1.0f / Direction.y : SsrFloatMax,
	                                   Direction.z != 0.0f ? 1.0f / Direction.z : SsrFloatMax);
	int    Mip           = 0;
	float2 MipRes        = MipResolution(0);
	float2 MipResInv     = 1.0f / MipRes;
	float2 UvOffset      = 0.005f / ScreenSize;
	UvOffset             = float2(Direction.x < 0.0f ? -UvOffset.x : UvOffset.x, Direction.y < 0.0f ? -UvOffset.y : UvOffset.y);
	const float2 FloorOffset = float2(Direction.x < 0.0f ? 0.0f : 1.0f, Direction.y < 0.0f ? 0.0f : 1.0f);

	// 첫 칸 경계까지 (자기 자신과 교차 방지)
	float2 XyPlane = floor(MipRes * Origin.xy) + FloorOffset;
	XyPlane        = XyPlane * MipResInv + UvOffset;
	const float2 T0 = (XyPlane - Origin.xy) * InvDirection.xy;
	float        T  = min(T0.x, T0.y);
	float3       Position = Origin + T * Direction;

	uint Iteration = 0;
	while (Iteration < MaxIterations && Mip >= 0)
	{
		if (T > 1.0f || any(Position.xy < 0.0f) || any(Position.xy > 1.0f))
		{
			bValid = false;
			return Position; // 최대 거리 밖 또는 화면 밖
		}
		const float2 MipPosition = MipRes * Position.xy;
		const float  SurfaceZ    = Hiz.Load(int3(min(int2(MipPosition), max(int2(MipRes), 1) - 1), Mip));
		const bool   bSkipped    = AdvanceRay(Origin, Direction, InvDirection, MipPosition, MipResInv, FloorOffset, UvOffset, SurfaceZ, Position, T);
		const int    NextMip     = clamp(Mip + (bSkipped ? 1 : -1), -1, (int)HizMipCount - 1);
		if (NextMip != Mip)
		{
			MipRes    = MipResolution((uint)max(NextMip, 0));
			MipResInv = 1.0f / MipRes;
		}
		Mip = NextMip;
		++Iteration;
	}
	bValid = Iteration < MaxIterations && T <= 1.0f;
	return Position;
}

// 픽셀 하나의 반사 추적. SurfaceView = 반사 표면 뷰 위치, HitDistance = 표면 → 교차점 거리 (cm, 맞지 않으면 0), Roughness = 표면 거칠기
float4 TraceReflection(int2 Pixel, out float3 SurfaceView, out float HitDistance, out float Roughness)
{
	SurfaceView = 0.0f;
	HitDistance = 0.0f;
	Roughness   = 0.0f;
	const float Depth = SceneDepth.Load(int3(Pixel, 0));
	if (Depth >= 1.0f)
	{
		return 0.0f;
	}
	const float4 NormalData  = SceneNormal.Load(int3(Pixel, 0));
	float3       WorldNormal = DecodeScreenNormal(NormalData);
	Roughness                = DecodeScreenRoughness(NormalData);
	if (bDecals != 0)
	{
		ApplyScreenDecals(DecalNormal, DecalMaterial, Pixel, WorldNormal, Roughness); // 메인 패스와 같은 표면 (데칼 거칠기/노멀)
	}
	if (Roughness > MaxRoughness)
	{
		return 0.0f; // 메인 패스가 어차피 0으로 페이드
	}
	const float2 UV = (float2(Pixel) + 0.5f) / ScreenSize;
	const float3 P  = ViewFromDepth(UV, Depth);
	SurfaceView     = P;
	const float3 N  = normalize(mul(WorldNormal, (float3x3)View));
	const float3 V  = bOrthographic != 0 ? float3(0.0f, 0.0f, 1.0f) : normalize(P); // 카메라 → 점
	const float3 R  = reflect(V, N);
	if (dot(R, N) <= 0.0f)
	{
		return 0.0f;
	}
	// 카메라 쪽으로 향하는 반사는 근평면 앞에서 자른다
	float Length = MaxDistance;
	if (bOrthographic == 0 && R.z < 0.0f)
	{
		Length = min(Length, (P.z - NearZ * 1.5f) / -R.z);
	}
	if (Length <= 1.0f)
	{
		return 0.0f;
	}
	const float3 Start     = ScreenFromView(P);
	const float3 End       = ScreenFromView(P + R * Length);
	const float3 Direction = End - Start;

	bool         bValid = false;
	const float3 Hit    = HierarchicalRaymarch(Start, Direction, bValid);
	if (!bValid)
	{
		return 0.0f;
	}

	// 두께 검사: 교차점 뒤로 얼마나 들어갔나 (뷰 깊이)
	const float  SceneHitDepth = SceneDepth.Load(int3(int2(Hit.xy * ScreenSize), 0));
	const float3 HitView       = ViewFromDepth(Hit.xy, Hit.z);
	const float3 SceneView     = ViewFromDepth(Hit.xy, SceneHitDepth);
	if (SceneHitDepth >= 1.0f || HitView.z - SceneView.z > Thickness)
	{
		return 0.0f;
	}
	// 뒷면 반사 금지 (맞은 면이 광선과 같은 쪽을 보면)
	const float3 HitNormal = normalize(mul(DecodeScreenNormal(SceneNormal.Load(int3(int2(Hit.xy * ScreenSize), 0))), (float3x3)View));
	if (dot(HitNormal, R) > 0.2f)
	{
		return 0.0f;
	}

	// 이전 프레임 위치로 재투영해 색을 읽는다
	const float4 PrevClip = mul(float4(Hit.x * 2.0f - 1.0f, 1.0f - Hit.y * 2.0f, Hit.z, 1.0f), Reprojection);
	if (PrevClip.w <= 1.0e-5f)
	{
		return 0.0f;
	}
	const float2 PrevUV = float2(PrevClip.x / PrevClip.w * 0.5f + 0.5f, 0.5f - PrevClip.y / PrevClip.w * 0.5f);
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return 0.0f;
	}
	const float3 Color = PrevSceneColor.SampleLevel(LinearSampler, PrevUV, 0.0f).rgb;

	// 신뢰도: 화면 가장자리 페이드 × 먼 교차 페이드
	const float2 Edge       = min(Hit.xy, 1.0f - Hit.xy);
	const float  EdgeFade   = saturate(min(Edge.x, Edge.y) * 10.0f);
	const float  TravelFade = 1.0f - saturate((length(HitView - P) / max(MaxDistance, 1.0f) - 0.7f) / 0.3f);
	// 카메라 쪽으로 돌아오는 광선은 화면에 정보가 적어 틀리기 쉽다 → 뷰 z가 -0.3 이하면 0
	const float  TowardFade = bOrthographic != 0 ? 1.0f : saturate((R.z + 0.3f) / 0.3f);
	HitDistance = length(HitView - P);
	return float4(max(Color, 0.0f), EdgeFade * TravelFade * TowardFade);
}

struct FSsrTraceOutput
{
	float4 Color  : SV_Target0; // rgb = 반사 색, a = 신뢰도
	float4 Motion : SV_Target1; // xy = 반사 움직임 벡터 (현재 UV − 이전 UV, 맞지 않으면 0 → 누적은 표면 움직임), z = 흐림 반경 (픽셀)
};

// 반사 움직임: 거울에 비친 상은 표면이 아니라 "시선 방향으로 표면 뒤 교차 거리만큼 간 가상 점"에 있는 것처럼 움직인다.
//   그 가상 점의 이전 프레임 화면 위치를 누적(SsrResolve.hlsl)이 이력 위치로 쓴다 — 평면 거울은 정확, 곡면은 근사.
//   표면 움직임으로 이력을 찾으면 카메라가 움직일 때 반사 내용이 어긋나 클램프가 이력을 버리고 반사 윤곽 계단이 지글거린다
FSsrTraceOutput PSTrace(FFullscreenVSOutput Input)
{
	const int2      Pixel = int2(Input.Position.xy);
	float3          SurfaceView;
	float           HitDistance;
	float           Roughness;
	FSsrTraceOutput Output;
	Output.Color  = TraceReflection(Pixel, SurfaceView, HitDistance, Roughness);
	Output.Motion = 0.0f;
	if (Output.Color.a > 0.0f && HitDistance > 0.0f)
	{
		const float3 ViewDirection = bOrthographic != 0 ? float3(0.0f, 0.0f, 1.0f) : normalize(SurfaceView);
		const float3 Virtual       = ScreenFromView(SurfaceView + ViewDirection * HitDistance);
		const float4 PrevClip      = mul(float4(Virtual.x * 2.0f - 1.0f, 1.0f - Virtual.y * 2.0f, Virtual.z, 1.0f), Reprojection);
		if (PrevClip.w > 1.0e-5f)
		{
			const float2 PrevUV = float2(PrevClip.x / PrevClip.w * 0.5f + 0.5f, 0.5f - PrevClip.y / PrevClip.w * 0.5f);
			Output.Motion.xy    = (float2(Pixel) + 0.5f) / ScreenSize - PrevUV;
		}
	}
	// 거칠기 흐림 반경 (ReflectionMath::ComputeSpecularConeTangent / ComputeSsrReflectionViewDepth / ComputeSsrBlurRadiusPixels와 같은 식).
	//   빗나간 픽셀도 반경을 준다(최대 추적 거리 기준) — 0이면 맞음/빗나감 경계의 빗나간 쪽이 흐려지지 않아 반사 영역이 칼로 자른 다각형이 된다.
	//   원뿔 폭은 반사된 상(가상 점, 표면 뒤 교차 거리)의 깊이로 화면에 옮긴다 — 표면 깊이로 나누면 먼 반사일수록 과하게 흐려진다
	const float Alpha = Roughness * Roughness;
	if (SurfaceView.z > 0.0f && Alpha >= 1.0e-3f)
	{
		const float Distance      = Output.Color.a > 0.0f && HitDistance > 0.0f ? HitDistance : MaxDistance;
		const float Power         = max(2.0f / (Alpha * Alpha) - 2.0f, 0.0f);
		const float CosAngle      = pow(0.244f, 1.0f / (Power + 1.0f));
		const float ConeTangent   = sqrt(max(1.0f - CosAngle * CosAngle, 0.0f)) / max(CosAngle, 1.0e-4f);
		const float VirtualDepth  = SurfaceView.z * (1.0f + Distance / max(length(SurfaceView), 1.0e-3f));
		const float PixelsPerUnit = bOrthographic != 0 ? ProjectionScale : ProjectionScale / max(VirtualDepth, 1.0e-3f);
		Output.Motion.z           = min(Distance * ConeTangent * PixelsPerUnit, MaxBlurRadius);
	}
	return Output;
}

