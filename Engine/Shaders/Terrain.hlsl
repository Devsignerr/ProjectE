// 지형 (Phase 34): 높이맵 텍스처로 정점을 올리는 청크 패치 + 레이어 4개 PBR 블렌딩.
// 조명/그림자/IBL/클러스터 함수는 Mesh.hlsl을 그대로 쓴다 (b1 PerFrame, b3 그림자, t5~t12, s0~s2 — 레지스터 공간 0).
// 지형 전용 리소스는 레지스터 공간 1: b0 지형 상수, b1 그리기 상수, b2 그림자 패스, t0 높이(R16), t1 가중치(RGBA8), t2 청크 목록,
// t3~t22 레이어 머티리얼 텍스처 테이블 (레이어마다 5칸, EMaterialTextureSlot 순서)
#include "Mesh.hlsl"

struct FTerrainChunk
{
	uint X;     // 시작 정점
	uint Y;
	uint Step;  // 정점 간격
	uint Quads; // 패치 한 변 사각형 수
};

cbuffer TerrainConstants : register(b0, space1)
{
	float3 TerrainOrigin;
	float  TerrainHeightScale; // cm / 16비트 단위
	float2 TerrainCellSize;
	float  TerrainResolution;
	float  TerrainSkirtDepth;
	float4 LayerTiling;        // 1 / 타일 크기 (cm)
	float4 LayerBaseColor[4];
	float4 LayerParams[4];     // 금속, 거칠기, 노멀 배율, AO 세기
	uint   TerrainLayerCount;
	uint   TerrainDebugLod;
	float2 TerrainPadding;
};

// 그리기 상수 (루트 상수): 청크 목록 안 시작 위치, 패치 한 변 사각형 수
cbuffer TerrainDraw : register(b1, space1)
{
	uint ChunkOffset;
	uint PatchQuads;
};

cbuffer TerrainShadowPass : register(b2, space1)
{
	float4x4 TerrainLightViewProjection;
};

Texture2D<float>               TerrainHeights : register(t0, space1); // R16_UNORM
Texture2D<float4>              TerrainWeights : register(t1, space1); // 레이어 0~3 가중치
StructuredBuffer<FTerrainChunk> TerrainChunks : register(t2, space1);
Texture2D Layer0Textures[5] : register(t3, space1);
Texture2D Layer1Textures[5] : register(t8, space1);
Texture2D Layer2Textures[5] : register(t13, space1);
Texture2D Layer3Textures[5] : register(t18, space1);

// 패치 정점: 0 ~ (Q+1)² - 1 = 격자, 그 뒤 4변 × (Q+1) = 스커트 (같은 XY에서 아래로 내린 정점)
float3 TerrainVertexPosition(uint VertexId, uint InstanceId, out float2 OutGrid, out uint OutStep)
{
	const FTerrainChunk Chunk = TerrainChunks[ChunkOffset + InstanceId];
	const uint          Row   = PatchQuads + 1;
	uint2               Local;
	bool                bSkirt = false;
	if (VertexId < Row * Row)
	{
		Local = uint2(VertexId % Row, VertexId / Row);
	}
	else
	{
		const uint K    = VertexId - Row * Row;
		const uint Edge = K / Row;
		const uint T    = K % Row;
		Local           = Edge == 0 ? uint2(T, 0) : (Edge == 1 ? uint2(PatchQuads, T) : (Edge == 2 ? uint2(T, PatchQuads) : uint2(0, T)));
		bSkirt          = true;
	}
	const uint2 Cell   = uint2(Chunk.X, Chunk.Y) + Local * Chunk.Step;
	const float Height = TerrainHeights.Load(int3(Cell, 0)) * 65535.0f;
	OutGrid            = float2(Cell);
	OutStep            = Chunk.Step;
	float3 Position    = float3(TerrainOrigin.xy + OutGrid * TerrainCellSize, TerrainOrigin.z + Height * TerrainHeightScale);
	if (bSkirt)
	{
		Position.z -= TerrainSkirtDepth * Chunk.Step;
	}
	return Position;
}

struct FTerrainPixelInput
{
	float4                Position      : SV_Position;
	float3                WorldPosition : POSITION0;
	float2                Grid          : TEXCOORD0;
	nointerpolation uint  Step          : TEXCOORD1;
	// 움직임 벡터 (지터 없는 현재/이전 클립). 지형은 정적이라 이전 = 같은 월드 위치 × 이전 뷰-투영 (카메라 움직임만)
	float4                CurrentClip   : TEXCOORD2;
	float4                PreviousClip  : TEXCOORD3;
};

// 사전 패스(TerrainPrepassPS)와 메인 패스(TerrainPS)가 같은 바이트코드를 쓴다 → 메인 패스 깊이 EQUAL
FTerrainPixelInput TerrainVS(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	FTerrainPixelInput Output;
	const float3       World = TerrainVertexPosition(VertexId, InstanceId, Output.Grid, Output.Step);
	Output.Position          = mul(float4(World, 1.0f), ViewProjection);
	Output.WorldPosition     = World;
	Output.CurrentClip       = mul(float4(World, 1.0f), UnjitteredViewProjection);
	Output.PreviousClip      = mul(float4(World, 1.0f), PrevViewProjection);
	return Output;
}

float4 TerrainShadowVS(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID) : SV_Position
{
	float2       Grid;
	uint         Step;
	const float3 World = TerrainVertexPosition(VertexId, InstanceId, Grid, Step);
	return mul(float4(World, 1.0f), TerrainLightViewProjection);
}

// 격자 좌표 → 높이맵 UV (정점 = 텍셀 중심)
float2 TerrainGridToUv(float2 Grid)
{
	return (Grid + 0.5f) / TerrainResolution;
}

float SampleTerrainWorldHeight(float2 Grid)
{
	return TerrainHeights.SampleLevel(IblSampler, TerrainGridToUv(Grid), 0) * 65535.0f * TerrainHeightScale;
}

// 높이맵 중심 차분 법선 (LOD와 무관하게 원본 해상도)
float3 ComputeTerrainNormal(float2 Grid)
{
	const float Left  = SampleTerrainWorldHeight(Grid - float2(1.0f, 0.0f));
	const float Right = SampleTerrainWorldHeight(Grid + float2(1.0f, 0.0f));
	const float Down  = SampleTerrainWorldHeight(Grid - float2(0.0f, 1.0f));
	const float Up    = SampleTerrainWorldHeight(Grid + float2(0.0f, 1.0f));
	return normalize(float3(-(Right - Left) / (2.0f * TerrainCellSize.x), -(Up - Down) / (2.0f * TerrainCellSize.y), 1.0f));
}

struct FLayerBlend
{
	float3 Albedo;
	float  Metallic;
	float  Roughness;
	float  Occlusion;
	float2 NormalXY; // 탄젠트 공간 XY (배율 적용)
};

// 레이어 하나 샘플 후 가중치로 누적 (분기 없이 — 미분 연산이 균일 흐름에 있게)
#define E_ACCUMULATE_LAYER(Index, Textures)                                                                              \
	{                                                                                                                     \
		const float2 LayerUv = WorldXY * LayerTiling[Index];                                                              \
		const float4 Near    = Textures[0].SampleBias(LinearSampler, LayerUv, MaterialMipBias);                                                \
		const float4 Far     = Textures[0].SampleBias(LinearSampler, LayerUv * 0.123f, MaterialMipBias);                                       \
		const float4 Base    = lerp(Near, Far, 0.35f) * LayerBaseColor[Index]; /* 큰 배율 한 번 더 섞어 반복 무늬 줄이기 */         \
		const float4 MR      = Textures[1].SampleBias(LinearSampler, LayerUv, MaterialMipBias);                                                \
		const float2 NXY     = (Textures[2].SampleBias(LinearSampler, LayerUv, MaterialMipBias).xy * 2.0f - 1.0f) * LayerParams[Index].z;      \
		const float  AO      = lerp(1.0f, Textures[3].SampleBias(LinearSampler, LayerUv, MaterialMipBias).r, LayerParams[Index].w);            \
		const float  W       = Weights[Index];                                                                            \
		Blend.Albedo += Base.rgb * W;                                                                                     \
		Blend.Metallic += saturate(MR.b * LayerParams[Index].x) * W;                                                      \
		Blend.Roughness += clamp(MR.g * LayerParams[Index].y, 0.045f, 1.0f) * W;                                          \
		Blend.Occlusion += AO * W;                                                                                        \
		Blend.NormalXY += NXY * W;                                                                                        \
	}

float3 TerrainLodColor(uint Step)
{
	const float3 Colors[6] = { float3(1.0f, 0.35f, 0.35f), float3(0.35f, 1.0f, 0.35f), float3(0.35f, 0.5f, 1.0f),
	                           float3(1.0f, 1.0f, 0.35f), float3(1.0f, 0.35f, 1.0f), float3(0.35f, 1.0f, 1.0f) };
	return Colors[min(firstbitlow(max(Step, 1u)), 5u)];
}

float4 TerrainPS(FTerrainPixelInput Input) : SV_Target
{
	const float2 WorldXY = Input.WorldPosition.xy;
	float4       Weights = TerrainWeights.SampleLevel(IblSampler, TerrainGridToUv(Input.Grid), 0);
	Weights /= max(dot(Weights, 1.0f), 1.0e-4f);

	FLayerBlend Blend;
	Blend.Albedo    = 0.0f;
	Blend.Metallic  = 0.0f;
	Blend.Roughness = 0.0f;
	Blend.Occlusion = 0.0f;
	Blend.NormalXY  = 0.0f;
	E_ACCUMULATE_LAYER(0, Layer0Textures)
	E_ACCUMULATE_LAYER(1, Layer1Textures)
	E_ACCUMULATE_LAYER(2, Layer2Textures)
	E_ACCUMULATE_LAYER(3, Layer3Textures)

	// 탄젠트 프레임: T = 표면 위 +X(+U), B = 노멀 맵 +Y = UV 위쪽(-V = -Y) = -cross(N, T)
	const float3 N = ComputeTerrainNormal(Input.Grid);
	const float3 T = normalize(float3(1.0f, 0.0f, 0.0f) - N * N.x);
	const float3 B = -cross(N, T);
	float3       TangentNormal;
	TangentNormal.xy = Blend.NormalXY;
	TangentNormal.z  = sqrt(saturate(1.0f - dot(TangentNormal.xy, TangentNormal.xy)));

	FSurface Surface;
	Surface.Albedo    = Blend.Albedo;
	Surface.Metallic  = Blend.Metallic;
	Surface.Roughness = Blend.Roughness;
	Surface.N         = normalize(T * TangentNormal.x + B * TangentNormal.y + N * TangentNormal.z);
	Surface.V         = normalize(CameraPosition - Input.WorldPosition);
	// 메시와 같은 자리: 데칼(DBuffer) → 거칠기 클램프 → SSAO는 간접광에만 (Mesh.hlsl PSMain)
	if (DecalsEnabled != 0)
	{
		ApplyDecals(Input.Position.xy, Surface);
	}
	Surface.Roughness = clamp(Surface.Roughness, 0.045f, 1.0f);
	Surface.Occlusion = Blend.Occlusion * SampleScreenAmbientOcclusion(Input.Position.xy, Input.WorldPosition);

	const float3 L        = -DirectionalLight.Direction;
	const float3 Radiance = DirectionalLight.Color * DirectionalLight.Intensity;
	const float  Shadow   = ComputeDirectionalShadow(Input.WorldPosition, N, L, Input.Position.xy, true); // RT 그림자 마스크(t24) 또는 섀도맵

	float3 Color = EvaluateDirectLight(Surface, L, Radiance) * Shadow;
	Color += EvaluateLocalLights(Surface, Input.Position.xy, Input.WorldPosition, N);
	Color += EvaluateImageBasedLighting(Surface, Input.WorldPosition, Input.Position.xy); // 캡처/SSR/하늘

	if (VisualizeCascades != 0)
	{
		Color *= CascadeDebugColor(Input.WorldPosition);
	}
	if (TerrainDebugLod != 0)
	{
		Color *= TerrainLodColor(Input.Step);
	}
	return float4(Color, 0.0f); // 알파 = TAA 반응형 마스크 (불투명 0)
}

// 깊이 사전 패스: 깊이 + 화면 공간 법선(높이맵 법선) + 거칠기(레이어 가중 금속/거칠기 G × 팩터) + 움직임 벡터 (Mesh.hlsl PSPrepass와 같은 출력)
FPrepassOutput TerrainPrepassPS(FTerrainPixelInput Input)
{
	const float2 WorldXY = Input.WorldPosition.xy;
	float4       Weights = TerrainWeights.SampleLevel(IblSampler, TerrainGridToUv(Input.Grid), 0);
	Weights /= max(dot(Weights, 1.0f), 1.0e-4f);
	const float Roughness = Layer0Textures[1].SampleBias(LinearSampler, WorldXY * LayerTiling[0], MaterialMipBias).g * LayerParams[0].y * Weights[0] +
	                        Layer1Textures[1].SampleBias(LinearSampler, WorldXY * LayerTiling[1], MaterialMipBias).g * LayerParams[1].y * Weights[1] +
	                        Layer2Textures[1].SampleBias(LinearSampler, WorldXY * LayerTiling[2], MaterialMipBias).g * LayerParams[2].y * Weights[2] +
	                        Layer3Textures[1].SampleBias(LinearSampler, WorldXY * LayerTiling[3], MaterialMipBias).g * LayerParams[3].y * Weights[3];
	FPrepassOutput Output;
	Output.Normal   = EncodeScreenNormal(ComputeTerrainNormal(Input.Grid), Roughness);
	Output.Velocity = ComputeVelocity(Input.CurrentClip, Input.PreviousClip);
	return Output;
}

// 에디터 선택 아웃라인 마스크 (R8): TerrainShadowVS(뷰-투영 = 편집 카메라)와 함께
float TerrainMaskPS() : SV_Target
{
	return 1.0f;
}
