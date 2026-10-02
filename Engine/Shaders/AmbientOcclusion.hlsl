#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// SSAO (FAmbientOcclusion): 반해상도 GTAO → 양방향 블러 (가로/세로). 식은 Renderer/AmbientOcclusionMath.h와 같다
//   출력 R16G16_FLOAT: R = 가시도(1 = 가림 없음), G = 뷰 깊이(cm, 블러/업샘플 경계 판정). 기하 없는 픽셀 = (1, 0)
//   메인 패스(Mesh.hlsl)가 4탭 깊이 가중 업샘플로 읽어 간접광(IBL)에만 곱한다

cbuffer AoConstants : register(b0)
{
	float4x4 InvProjection;    // 지터 포함 투영의 역 (깊이 → 뷰 위치)
	float4x4 View;             // 월드 → 뷰 (법선 변환)
	float2   FullSize;         // 전체 해상도 (깊이/법선)
	float2   HalfTexelSize;    // 1 / 반해상도
	float    Radius;           // cm
	float    Intensity;        // 가시도^Intensity
	float    PixelsPerUnit;    // 0.5 * 전체 높이 * Proj[1][1] (반경 → 전체 해상도 픽셀, 원근은 뷰 깊이로 나눔)
	uint     bOrthographic;
	uint     FrameIndex;       // 방향 회전 (TAA 누적)
	float    BlurSharpness;    // 깊이 경계 보존 강도
	float2   BlurDirection;    // 블러: (1, 0) 또는 (0, 1) 반해상도 텍셀
	uint     ResolutionDivisor; // 2 = 반해상도, 1 = 전체 해상도 (픽셀 아트)
	uint     bGridNoise;        // 1 = 노이즈를 월드 도트 격자에 고정 (픽셀 아트 카메라 스냅)
	int2     GridOrigin;        // 소스 픽셀 (0,0)의 격자 번호
};

Texture2D<float>  SceneDepth    : register(t0); // 전체 해상도 깊이 (블러 패스는 안 씀)
Texture2D<float4> SceneNormal   : register(t1);
Texture2D<float2> AoSource      : register(t2); // 블러 입력
SamplerState      PointSampler  : register(s1);

static const float AoPi = 3.14159265f;

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float3 ReconstructViewPosition(float2 UV, float DeviceDepth)
{
	const float4 Ndc  = float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, DeviceDepth, 1.0f);
	const float4 View = mul(Ndc, InvProjection);
	return View.xyz / View.w;
}

float IntegrateArc(float H, float N)
{
	return 0.25f * (-cos(2.0f * H - N) + cos(N) + 2.0f * H * sin(N));
}

// 인터리브드 그래디언트 노이즈 (Jimenez) — 4x4 반복보다 띠가 적다
float InterleavedGradientNoise(float2 Pixel, uint Frame)
{
	Pixel += (float)(Frame % 8u) * 5.588238f;
	return frac(52.9829189f * frac(0.06711056f * Pixel.x + 0.00583715f * Pixel.y));
}

// 격자 칸 해시 (PCG) → [0, 1). 카메라가 도트 단위로 움직여도 같은 월드 칸은 같은 값
float GridCellNoise(int2 Cell)
{
	uint H = asuint(Cell.x) * 747796405u + asuint(Cell.y) * 2891336453u;
	H      = ((H >> ((H >> 28u) + 4u)) ^ H) * 277803737u;
	H      = (H >> 22u) ^ H;
	return (float)(H >> 8u) * (1.0f / 16777216.0f);
}

float2 PSCompute(FFullscreenVSOutput Input) : SV_Target
{
	const int2  HalfPixel = int2(Input.Position.xy);
	const int2  FullPixel = min(HalfPixel * (int)ResolutionDivisor, int2(FullSize) - 1);
	const float Depth     = SceneDepth.Load(int3(FullPixel, 0));
	if (Depth >= 1.0f)
	{
		return float2(1.0f, 0.0f);
	}
	const float2 FullTexel = 1.0f / FullSize;
	const float2 UV        = (float2(FullPixel) + 0.5f) * FullTexel;
	const float3 P         = ReconstructViewPosition(UV, Depth);
	const float3 V         = normalize(bOrthographic != 0 ? float3(0.0f, 0.0f, -1.0f) : -P);
	const float3 N         = normalize(mul(DecodeScreenNormal(SceneNormal.Load(int3(FullPixel, 0))), (float3x3)View));

	// 반경을 화면 픽셀로 (너무 작으면 가림 없음, 너무 크면 제한)
	const float RadiusPixels = min(Radius * PixelsPerUnit / (bOrthographic != 0 ? 1.0f : max(P.z, 1.0e-3f)), 256.0f);
	if (RadiusPixels < 1.0f)
	{
		return float2(1.0f, P.z);
	}
	const float StepPixels = RadiusPixels / 4.0f;
	const float Noise      = bGridNoise != 0 ? GridCellNoise(HalfPixel + GridOrigin) : InterleavedGradientNoise(float2(HalfPixel), FrameIndex);
	const float Jitter     = frac(Noise * 1.618034f + 0.5f);

	float Visibility = 0.0f;
	[unroll]
	for (uint Slice = 0; Slice < 2; ++Slice)
	{
		const float  Phi       = (Slice + Noise) * (AoPi / 2.0f);
		const float2 Direction = float2(cos(Phi), sin(Phi)); // 화면 UV 방향 (+Y 아래)
		const float3 SliceDir  = float3(Direction.x, -Direction.y, 0.0f); // 뷰 공간 (+Y 위)
		const float3 Axis      = normalize(cross(SliceDir, V));
		const float3 ProjN     = N - Axis * dot(N, Axis);
		const float  ProjLen   = max(length(ProjN), 1.0e-4f);
		const float3 OrthoDir  = SliceDir - dot(SliceDir, V) * V;
		const float  CosN      = saturate(dot(ProjN, V) / ProjLen);
		const float  NAngle   = (dot(OrthoDir, ProjN) >= 0.0f ? 1.0f : -1.0f) * acos(CosN);

		float CosH[2] = { -1.0f, -1.0f }; // [0] = 음수 쪽, [1] = 양수 쪽
		[unroll]
		for (uint Side = 0; Side < 2; ++Side)
		{
			const float SideSign = Side == 0 ? -1.0f : 1.0f;
			[unroll]
			for (uint Step = 0; Step < 4; ++Step)
			{
				const float2 Offset   = Direction * SideSign * StepPixels * (Step + Jitter) + 0.5f * Direction * SideSign;
				const float2 SampleUV = UV + Offset * FullTexel;
				const bool   bInside  = all(SampleUV >= 0.0f) && all(SampleUV <= 1.0f);
				const float SampleDepth = SceneDepth.SampleLevel(PointSampler, saturate(SampleUV), 0.0f);
				const float3 S          = ReconstructViewPosition(SampleUV, SampleDepth);
				const float3 D          = S - P;
				const float  DistSq     = dot(D, D);
				// 화면 밖·하늘 샘플은 가리지 않는다
				const float  Falloff    = (bInside && SampleDepth < 1.0f) ? saturate(1.0f - DistSq / (Radius * Radius)) : 0.0f;
				const float  Cos        = dot(D * rsqrt(max(DistSq, 1.0e-6f)), V);
				CosH[Side]              = max(CosH[Side], lerp(-1.0f, Cos, Falloff));
			}
		}
		float H0 = -acos(clamp(CosH[0], -1.0f, 1.0f));
		float H1 = acos(clamp(CosH[1], -1.0f, 1.0f));
		H0       = NAngle + max(H0 - NAngle, -AoPi / 2.0f);
		H1       = NAngle + min(H1 - NAngle, AoPi / 2.0f);
		Visibility += ProjLen * (IntegrateArc(H0, NAngle) + IntegrateArc(H1, NAngle));
	}
	Visibility = saturate(Visibility / 2.0f);
	return float2(pow(Visibility, Intensity), P.z);
}

// 양방향 블러 (반해상도, 한 방향 9탭): 깊이 상대 차이가 큰 샘플은 버린다
float2 PSBlur(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel  = int2(Input.Position.xy);
	const float2 Center = AoSource.Load(int3(Pixel, 0));
	if (Center.y <= 0.0f)
	{
		return Center; // 기하 없음
	}
	float Sum    = Center.x;
	float Weight = 1.0f;
	[unroll]
	for (int Tap = -4; Tap <= 4; ++Tap)
	{
		const float2 Sample = AoSource.Load(int3(Pixel + int2(BlurDirection * Tap), 0));
		const float  Gauss  = exp(-(float)(Tap * Tap) / 8.0f);
		// 가운데(이미 넣음)·기하 없음·화면 밖(Load가 0)은 가중 0
		const float W = (Tap != 0 && Sample.y > 0.0f) ? Gauss * exp(-abs(Sample.y - Center.y) / max(Center.y, 1.0e-3f) * BlurSharpness) : 0.0f;
		Sum += Sample.x * W;
		Weight += W;
	}
	return float2(Sum / Weight, Center.y);
}
