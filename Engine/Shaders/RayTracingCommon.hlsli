#ifndef E_RAY_TRACING_COMMON_HLSLI
#define E_RAY_TRACING_COMMON_HLSLI

#include "Common.hlsli"

// 레이 트레이싱 공용 (Phase 50 — 인라인 RayQuery, SM 6.5). C++ 쪽은 Renderer/RayTracingScene.h(구조) + RayTracingMath.h(같은 식).
//
// 바인딩 (FRayTracingPassRoot — RayTracingEffects.h):
//   t0 TLAS, t1 인스턴스 정보(InstanceID 번호), t2 머티리얼 표, t17 그래프 머티리얼 파라미터(float4),
//   공간 1 t0~ 바인드리스 Texture2D, 공간 2 t0~ 바인드리스 ByteAddressBuffer
//   (둘 다 셰이더 가시 힙 처음부터 — 힙 칸 번호 = FD3D12DescriptorHandle::Index), s0 선형 반복, s1 선형 클램프, s2 점 클램프
//
// 머티리얼 평가 (MaterialCommon.hlsli 머리 주석 "레이 트레이싱에서 쓰는 법"):
//   고정 PBR = MaterialDefault.hlsli(E_MATERIAL_CUSTOM_RESOURCES, 상수 = 머티리얼 표, 텍스처 = 바인드리스 테이블)
//   그래프 = 디파인 E_RT_GRAPH_MATERIALS + 가상 파일 RayTracingGraphMaterials.generated.hlsli (FRayTracingScene::BuildGraphVariant —
//   이번 프레임 TLAS의 그래프 셰이더 생성 함수를 슬롯별 이름으로 이어 붙이고 EvaluateGraphMaterial(슬롯)로 분기). 변형이 없으면 회색 근사
//   텍스처 밉 = 광선 원뿔 LOD (MATERIAL_SAMPLE = SampleLevel)
//
// ---- Phase 51 DDGI가 쓰는 법
//   1) 이 파일 + RayTracingLighting.hlsli를 include (루트는 FRayTracingPassRoot, 상수 b0/b1은 각 셰이더가 정의) — 그래프 머티리얼을 보려면
//      FRayTracingEffects처럼 셰이더를 FRayTracingScene::GetGraphVariant()의 디파인 + 가상 파일로 컴파일한다
//   2) RayDesc를 채워 TraceClosestHit / TraceClosestHitCullBack(Ray, 마스크) — Masked는 후보 단계에서 알파 테스트
//   3) 맞으면 LoadHitSurface(Hit, 원뿔 폭, -Ray.Direction) → FHitSurface (월드 위치/기하 법선/머티리얼 표면)
//      → EvaluateHitLighting(Surface, View) (방향광 + RT 그림자 광선, 로컬 라이트, IBL/캡처, 발광). 빗나가면 SampleSkyRadiance(방향, 거칠기)

struct FRayTracingInstance // FRayTracingInstanceGpu
{
	uint VertexBuffer;
	uint IndexBuffer;
	uint FirstIndex;
	uint Material;
	uint Flags;
	uint3 InstancePadding;
};

struct FRayTracingMaterial // FRayTracingMaterialGpu
{
	float4 BaseColorFactor;
	float3 EmissiveFactor;
	float  Metallic;
	float  Roughness;
	float  NormalScale;
	float  OcclusionStrength;
	float  AlphaCutoff;
	uint   TextureTable;
	uint   Flags;
	uint   GraphSlot;
	uint   GraphParams;
	uint   GraphTextureTable;
	uint3  MaterialPadding;
};

// RayTracingMath 상수와 같은 값
static const uint E_RT_MASK_STATIC        = 0x01;
static const uint E_RT_MASK_SKINNED       = 0x02;
static const uint E_RT_MASK_FOLIAGE       = 0x04;
static const uint E_RT_MASK_TERRAIN       = 0x08;
static const uint E_RT_MASK_SHADOW_CASTER = 0x10;
static const uint E_RT_MASK_TYPES         = 0x0F;
static const uint E_RT_INFO_TWO_SIDED = 1u << 0;
static const uint E_RT_INFO_MASKED    = 1u << 1;
static const uint E_RT_INFO_SKINNED   = 1u << 2;
static const uint E_RT_INFO_MIRRORED  = 1u << 3;
static const uint E_RT_MATERIAL_GRAPH  = 1u << 0;
static const uint E_RT_MATERIAL_MASKED = 1u << 1;
static const uint E_RT_SAMPLE_COUNT    = 16; // RayTracingMath::InterleavedSampleCount

RaytracingAccelerationStructure      SceneTlas       : register(t0);
StructuredBuffer<FRayTracingInstance> RtInstances    : register(t1);
StructuredBuffer<FRayTracingMaterial> RtMaterials    : register(t2);
StructuredBuffer<float4>             RtGraphParams   : register(t17);
Texture2D                            BindlessTextures[] : register(t0, space1);
ByteAddressBuffer                    BindlessBuffers[]  : register(t0, space2);
SamplerState                         RtWrapSampler   : register(s0);
SamplerState                         RtClampSampler  : register(s1);
SamplerState                         RtPointSampler  : register(s2);

// ---- RayTracingMath와 같은 식

// 광선 원점 오프셋 (Wächter & Binder, RTG 6장): 기하 법선 쪽으로 몇 ulp
float3 OffsetRayOrigin(float3 P, float3 N)
{
	const float Origin     = 1.0f / 32.0f;
	const float FloatScale = 1.0f / 65536.0f;
	const float IntScale   = 256.0f;
	const int3  Of         = int3(IntScale * N);
	const float3 Stepped   = asfloat(asint(P) + select(P < 0.0f, -Of, Of));
	return select(abs(P) < Origin, P + FloatScale * N, Stepped);
}

// 1차 표면(깊이 버퍼 재구성) 바이어스 (cm) — RayTracingMath::ComputeSurfaceBias
float ComputeSurfaceBias(float ViewDepth, float NdotL, float Scale)
{
	const float Grazing = 1.0f - saturate(NdotL);
	return (0.5f + max(ViewDepth, 0.0f) * 0.0005f) * (1.0f + Grazing) * Scale;
}

// 교차 표본 순서 (Bayer 4x4 + 프레임 회전) — RayTracingMath::GetInterleavedSampleIndex
uint GetInterleavedSampleIndex(uint2 Pixel, uint Frame)
{
	static const uint Bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
	return (Bayer[(Pixel.y & 3u) * 4u + (Pixel.x & 3u)] + Frame) % E_RT_SAMPLE_COUNT;
}

// 단위 원판 Vogel 나선 — RayTracingMath::GetDiskSample
float2 GetDiskSample(uint Index, uint Count)
{
	const float T     = ((float)Index + 0.5f) / (float)Count;
	const float Angle = (float)Index * 2.39996323f;
	return sqrt(T) * float2(cos(Angle), sin(Angle));
}

// 원뿔 안 방향 — RayTracingMath::SampleConeDirection
float3 SampleConeDirection(float3 Axis, float TanHalfAngle, float2 Disk)
{
	const float3 Helper    = abs(Axis.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	const float3 Tangent   = normalize(cross(Helper, Axis));
	const float3 Bitangent = cross(Axis, Tangent);
	return normalize(Axis + (Tangent * Disk.x + Bitangent * Disk.y) * TanHalfAngle);
}

// ---- 기하 읽기 (바인드리스)

struct FHitVertex
{
	float3 Position;
	float3 Normal;
	float2 UV;
	float4 Color;
	float4 Tangent;
};

FHitVertex LoadHitVertex(uint BufferIndex, uint Index)
{
	const uint Base = Index * 64; // FVertex
	FHitVertex Vertex;
	Vertex.Position = asfloat(BindlessBuffers[NonUniformResourceIndex(BufferIndex)].Load3(Base));
	Vertex.Normal   = asfloat(BindlessBuffers[NonUniformResourceIndex(BufferIndex)].Load3(Base + 12));
	Vertex.UV       = asfloat(BindlessBuffers[NonUniformResourceIndex(BufferIndex)].Load2(Base + 24));
	Vertex.Color    = asfloat(BindlessBuffers[NonUniformResourceIndex(BufferIndex)].Load4(Base + 32));
	Vertex.Tangent  = asfloat(BindlessBuffers[NonUniformResourceIndex(BufferIndex)].Load4(Base + 48));
	return Vertex;
}

uint3 LoadHitTriangle(FRayTracingInstance Instance, uint Primitive)
{
	return BindlessBuffers[NonUniformResourceIndex(Instance.IndexBuffer)].Load3((Instance.FirstIndex + Primitive * 3) * 4);
}

// ---- 머티리얼 평가 (바인드리스)

static float               RtMaterialLod          = 0.0f;
static uint                RtMaterialTextureTable = 0;
static FRayTracingMaterial RtHitMaterial;
#define MATERIAL_SAMPLE(Texture, Sampler, Uv) (Texture).SampleLevel(Sampler, Uv, RtMaterialLod)
#define E_MATERIAL_CUSTOM_RESOURCES
#define E_MATERIAL_DEFAULT_CONSTANTS RtHitMaterial
#define E_MATERIAL_HEADER (RtGraphParams[RtHitMaterial.GraphParams])
#define E_MATERIAL_PARAM(Index) (RtGraphParams[RtHitMaterial.GraphParams + 1 + (Index)])
#define E_MATERIAL_TEXTURE(Index) BindlessTextures[NonUniformResourceIndex(RtMaterialTextureTable + (Index))]
#define E_MATERIAL_SAMPLER_WRAP RtWrapSampler
#define E_MATERIAL_SAMPLER_CLAMP RtClampSampler
#include "MaterialCommon.hlsli"
#include "MaterialDefault.hlsli"
#ifdef E_RT_GRAPH_MATERIALS
#include "RayTracingGraphMaterials.generated.hlsli"
#endif

// 이번 히트 머티리얼(RtHitMaterial)로 표면 평가: 그래프 슬롯이 있으면 생성 함수, 아니면 고정 PBR (그래프 근사 포함)
void EvaluateHitMaterial(in FMaterialPixelInputs In, out FMaterialSurface Out)
{
#ifdef E_RT_GRAPH_MATERIALS
	if (RtHitMaterial.GraphSlot != 0)
	{
		RtMaterialTextureTable = RtHitMaterial.GraphTextureTable;
		EvaluateGraphMaterial(RtHitMaterial.GraphSlot, In, Out);
		return;
	}
#endif
	RtMaterialTextureTable = RtHitMaterial.TextureTable;
	EvaluateMaterial(In, Out);
}

// 이 빌드에서 쓰는 텍스처 테이블 (밉 LOD 기준 크기)
uint GetHitTextureTable()
{
#ifdef E_RT_GRAPH_MATERIALS
	if (RtHitMaterial.GraphSlot != 0)
	{
		return RtHitMaterial.GraphTextureTable;
	}
#endif
	return RtHitMaterial.TextureTable;
}

// 정점 보간 → 머티리얼 입력 (Mesh.hlsl MakeMaterialInputs와 같은 뜻). ObjectToWorld/WorldToObject = 인스턴스 변환 (스킨 = 항등),
// OutFaceCross = 월드 면 외적 (길이 = 면적 × 2), OutUvArea = UV 면적 × 2. View = 표면 → 광선 원점
FMaterialPixelInputs BuildHitInputs(FRayTracingInstance Instance, uint Primitive, float2 Barycentrics, float3x4 ObjectToWorld, float3x4 WorldToObject,
                                    float3 View, bool bFrontFace, out float3 OutFaceCross, out float OutUvArea)
{
	const uint3  Tri = LoadHitTriangle(Instance, Primitive);
	const float3 W   = float3(1.0f - Barycentrics.x - Barycentrics.y, Barycentrics.x, Barycentrics.y);
	const FHitVertex V0 = LoadHitVertex(Instance.VertexBuffer, Tri.x);
	const FHitVertex V1 = LoadHitVertex(Instance.VertexBuffer, Tri.y);
	const FHitVertex V2 = LoadHitVertex(Instance.VertexBuffer, Tri.z);

	const float3 P0 = mul(ObjectToWorld, float4(V0.Position, 1.0f));
	const float3 P1 = mul(ObjectToWorld, float4(V1.Position, 1.0f));
	const float3 P2 = mul(ObjectToWorld, float4(V2.Position, 1.0f));
	OutFaceCross    = cross(P1 - P0, P2 - P0);
	const float2 Duv1 = V1.UV - V0.UV;
	const float2 Duv2 = V2.UV - V0.UV;
	OutUvArea         = abs(Duv1.x * Duv2.y - Duv1.y * Duv2.x);

	FMaterialPixelInputs In;
	In.WorldPosition = P0 * W.x + P1 * W.y + P2 * W.z;
	// 법선: 역전치 = (WorldToObject 3x3)ᵀ → 행벡터 곱
	In.WorldNormal   = normalize(mul(V0.Normal * W.x + V1.Normal * W.y + V2.Normal * W.z, (float3x3)WorldToObject));
	const float4 Tangent    = V0.Tangent * W.x + V1.Tangent * W.y + V2.Tangent * W.z;
	const float  Handedness = (Instance.Flags & E_RT_INFO_MIRRORED) != 0 ? -1.0f : 1.0f;
	In.WorldTangent  = float4(normalize(mul((float3x3)ObjectToWorld, Tangent.xyz)), Tangent.w * Handedness);
	In.UV0           = V0.UV * W.x + V1.UV * W.y + V2.UV * W.z;
	In.VertexColor   = V0.Color * W.x + V1.Color * W.y + V2.Color * W.z;
	In.CameraVector  = View;
	In.PixelPosition = 0.0f;
	In.bFrontFace    = bFrontFace;
	return In;
}

// 후보 알파 테스트 (Masked): Mesh.hlsl PSMainMasked와 같은 판정 — 고정 PBR = 베이스 알파(텍스처 × 정점 색 × 팩터), 그래프 = OpacityMask (밉 0)
bool PassesAlphaTest(uint InstanceIndex, uint Primitive, float2 Barycentrics, float3x4 ObjectToWorld, float3x4 WorldToObject, float3 View, bool bFrontFace)
{
	const FRayTracingInstance Instance = RtInstances[InstanceIndex];
	if ((Instance.Flags & E_RT_INFO_MASKED) == 0)
	{
		return true;
	}
	RtHitMaterial = RtMaterials[Instance.Material];
	RtMaterialLod = 0.0f;
	float3 FaceCross;
	float  UvArea;
	const FMaterialPixelInputs In = BuildHitInputs(Instance, Primitive, Barycentrics, ObjectToWorld, WorldToObject, View, bFrontFace, FaceCross, UvArea);
	FMaterialSurface Surface;
	EvaluateHitMaterial(In, Surface);
	return Surface.OpacityMask >= RtHitMaterial.AlphaCutoff;
}

// ---- 추적

struct FRayHit
{
	bool   bHit;
	float  T;
	uint   Instance;   // InstanceID (정보 버퍼 번호)
	uint   Primitive;
	float2 Barycentrics;
	bool   bFrontFace;
	float3x4 ObjectToWorld;
	float3x4 WorldToObject;
};

// 후보(비불투명 = Masked) 처리: 알파 테스트를 통과하면 확정
#define E_RT_PROCESS_CANDIDATES(Query, Ray)                                                                                          \
	while (Query.Proceed())                                                                                                          \
	{                                                                                                                                \
		if (Query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&                                                                \
		    PassesAlphaTest(Query.CandidateInstanceID(), Query.CandidatePrimitiveIndex(), Query.CandidateTriangleBarycentrics(),     \
		                    Query.CandidateObjectToWorld3x4(), Query.CandidateWorldToObject3x4(), -Ray.Direction,                    \
		                    Query.CandidateTriangleFrontFace()))                                                                     \
		{                                                                                                                            \
			Query.CommitNonOpaqueTriangleHit();                                                                                      \
		}                                                                                                                            \
	}

// 가장 가까운 히트. CullFlags = RAY_FLAG_CULL_BACK_FACING_TRIANGLES 등 (양면 머티리얼 인스턴스는 TRIANGLE_CULL_DISABLE이라 컬링되지 않는다)
#define E_RT_DEFINE_TRACE_CLOSEST(Name, CullFlags)                                                                 \
	FRayHit Name(RayDesc Ray, uint Mask)                                                                           \
	{                                                                                                              \
		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | CullFlags> Query;                                           \
		Query.TraceRayInline(SceneTlas, RAY_FLAG_NONE, Mask, Ray);                                                 \
		E_RT_PROCESS_CANDIDATES(Query, Ray)                                                                        \
		FRayHit Hit = (FRayHit)0;                                                                                  \
		Hit.bHit    = Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;                                           \
		Hit.T       = Ray.TMax;                                                                                    \
		if (Hit.bHit)                                                                                              \
		{                                                                                                          \
			Hit.T             = Query.CommittedRayT();                                                             \
			Hit.Instance      = Query.CommittedInstanceID();                                                       \
			Hit.Primitive     = Query.CommittedPrimitiveIndex();                                                   \
			Hit.Barycentrics  = Query.CommittedTriangleBarycentrics();                                             \
			Hit.bFrontFace    = Query.CommittedTriangleFrontFace();                                                \
			Hit.ObjectToWorld = Query.CommittedObjectToWorld3x4();                                                 \
			Hit.WorldToObject = Query.CommittedWorldToObject3x4();                                                 \
		}                                                                                                          \
		return Hit;                                                                                                \
	}
E_RT_DEFINE_TRACE_CLOSEST(TraceClosestHit, RAY_FLAG_NONE)
E_RT_DEFINE_TRACE_CLOSEST(TraceClosestHitCullBack, RAY_FLAG_CULL_BACK_FACING_TRIANGLES)

// 그림자/가림 광선: 첫 히트에서 끝 (Masked는 알파 테스트). 반환 = 가림 거리 (안 가리면 음수)
float TraceOcclusion(RayDesc Ray, uint Mask)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> Query;
	Query.TraceRayInline(SceneTlas, RAY_FLAG_NONE, Mask, Ray);
	E_RT_PROCESS_CANDIDATES(Query, Ray)
	return Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? Query.CommittedRayT() : -1.0f;
}

// ---- 히트 표면 (정점 보간 + 머티리얼 평가)

struct FHitSurface
{
	float3 Position;
	float3 GeometricNormal; // 월드, 광선 쪽 (뒷면이면 뒤집음)
	float3 Normal;          // 셰이딩 법선 (노멀 맵, 광선 쪽)
	float3 Albedo;
	float  Metallic;
	float  Roughness;
	float  Occlusion;
	float3 Emissive;
	uint   Instance;
};

// 원뿔 폭(월드) = 히트까지 퍼진 광선 원뿔 지름 (텍스처 LOD — RayTracingMath::ComputeRayConeLod), View = 표면 → 광선 원점 방향
FHitSurface LoadHitSurface(FRayHit Hit, float ConeWidth, float3 View)
{
	const FRayTracingInstance Instance = RtInstances[Hit.Instance];
	RtHitMaterial                      = RtMaterials[Instance.Material];
	float3                     FaceCross;
	float                      UvArea;
	const FMaterialPixelInputs In = BuildHitInputs(Instance, Hit.Primitive, Hit.Barycentrics, Hit.ObjectToWorld, Hit.WorldToObject, View, Hit.bFrontFace,
	                                               FaceCross, UvArea);

	// 텍스처 LOD: 광선 원뿔 (첫 텍스처 크기 기준)
	uint TexWidth, TexHeight, MipCount;
	BindlessTextures[NonUniformResourceIndex(GetHitTextureTable())].GetDimensions(0, TexWidth, TexHeight, MipCount);
	const float WorldArea = length(FaceCross);
	const float TexelArea = max(UvArea * (float)TexWidth * (float)TexHeight, 1.0e-12f);
	float3      FaceNormal = WorldArea > 1.0e-12f ? FaceCross / WorldArea : In.WorldNormal;
	const float CosNormal  = max(abs(dot(FaceNormal, View)), 1.0e-3f);
	RtMaterialLod = clamp(0.5f * log2(TexelArea / max(WorldArea, 1.0e-12f)) + log2(max(ConeWidth, 1.0e-6f) / CosNormal), 0.0f, (float)MipCount - 1.0f);

	FMaterialSurface Material;
	EvaluateHitMaterial(In, Material);

	// 광선 쪽 면: 기하 법선을 광선 원점 쪽으로 (양면/뒷면 히트), 셰이딩 법선도 같은 쪽 (Mesh.hlsl MakeMeshSurface의 뒷면 뒤집기와 같은 뜻)
	FaceNormal = dot(FaceNormal, In.WorldNormal) < 0.0f ? -FaceNormal : FaceNormal;
	const bool bFlip = dot(In.WorldNormal, View) < 0.0f;
	FHitSurface Surface;
	Surface.Position        = In.WorldPosition;
	Surface.Instance        = Hit.Instance;
	Surface.GeometricNormal = bFlip ? -FaceNormal : FaceNormal;
	Surface.Normal          = MaterialTangentToWorld(In.WorldNormal, In.WorldTangent, Material.Normal);
	Surface.Normal          = bFlip ? -Surface.Normal : Surface.Normal;
	Surface.Albedo          = Material.BaseColor;
	Surface.Metallic        = saturate(Material.Metallic);
	Surface.Roughness       = clamp(Material.Roughness, 0.045f, 1.0f);
	Surface.Occlusion       = Material.AmbientOcclusion;
	Surface.Emissive        = Material.Emissive;
	return Surface;
}

// 디버그 (rt-instances): 인스턴스 번호 → 색
float3 HashInstanceColor(uint Index)
{
	uint H = Index * 747796405u + 2891336453u;
	H      = ((H >> ((H >> 28u) + 4u)) ^ H) * 277803737u;
	H      = (H >> 22u) ^ H;
	return float3((H & 0xFFu), (H >> 8) & 0xFFu, (H >> 16) & 0xFFu) / 255.0f * 0.8f + 0.2f;
}

#endif // E_RAY_TRACING_COMMON_HLSLI