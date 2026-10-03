#ifndef E_RAY_TRACING_COMMON_HLSLI
#define E_RAY_TRACING_COMMON_HLSLI

#include "Common.hlsli"

// 레이 트레이싱 공용 (Phase 50 — 인라인 RayQuery, SM 6.5). C++ 쪽은 Renderer/RayTracingScene.h(구조) + RayTracingMath.h(같은 식).
//
// 바인딩 (FRayTracingPassRoot — RayTracingEffects.h):
//   t0 TLAS, t1 인스턴스 정보(InstanceID 번호), t2 머티리얼 표, 공간 1 t0~ 바인드리스 Texture2D, 공간 2 t0~ 바인드리스 ByteAddressBuffer
//   (둘 다 셰이더 가시 힙 처음부터 — 힙 칸 번호 = FD3D12DescriptorHandle::Index), s0 선형 반복, s1 선형 클램프, s2 점 클램프
//
// ---- Phase 51 DDGI가 쓰는 법
//   1) 이 파일 + RayTracingLighting.hlsli를 include (루트는 FRayTracingPassRoot, 상수 b0/b1은 각 셰이더가 정의)
//   2) RayDesc를 채워 TraceClosestHit(Ray, 마스크, 컬링 플래그, out FRayHit) — Masked는 후보 단계에서 알파 테스트
//   3) 맞으면 LoadHitSurface(Hit, 원뿔 폭, -Ray.Direction) → FHitSurface (월드 위치/기하 법선/머티리얼 표면 — MaterialDefault 바인드리스 평가)
//      → EvaluateHitLighting(Surface, ...) (방향광 + RT 그림자 광선, 로컬 라이트, IBL/캡처, 발광). 빗나가면 SampleSkyRadiance(방향, 거칠기)
//   그래프 머티리얼은 회색 근사(FRayTracingMaterialGpu::MaterialFlagGraph) — 생성 셰이더 히트는 상태 객체(히트 그룹 오프셋 예약됨) 후속

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
	uint2  MaterialPadding;
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

// 후보 알파 테스트 (Masked): 베이스 컬러 알파(텍스처 × 정점 색 × 팩터) ≥ 컷오프면 맞음 — Mesh.hlsl PSMainMasked와 같은 판정 (밉 0)
bool PassesAlphaTest(uint InstanceIndex, uint Primitive, float2 Barycentrics)
{
	const FRayTracingInstance Instance = RtInstances[InstanceIndex];
	if ((Instance.Flags & E_RT_INFO_MASKED) == 0)
	{
		return true;
	}
	const FRayTracingMaterial Material = RtMaterials[Instance.Material];
	const uint3  Tri = LoadHitTriangle(Instance, Primitive);
	const float3 W   = float3(1.0f - Barycentrics.x - Barycentrics.y, Barycentrics.x, Barycentrics.y);
	const FHitVertex V0 = LoadHitVertex(Instance.VertexBuffer, Tri.x);
	const FHitVertex V1 = LoadHitVertex(Instance.VertexBuffer, Tri.y);
	const FHitVertex V2 = LoadHitVertex(Instance.VertexBuffer, Tri.z);
	const float2 UV     = V0.UV * W.x + V1.UV * W.y + V2.UV * W.z;
	const float  Alpha  = V0.Color.a * W.x + V1.Color.a * W.y + V2.Color.a * W.z;
	const float  Base   = BindlessTextures[NonUniformResourceIndex(Material.TextureTable)].SampleLevel(RtWrapSampler, UV, 0.0f).a;
	return Base * Alpha * Material.BaseColorFactor.a >= Material.AlphaCutoff;
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

// 가장 가까운 히트. Masked 인스턴스(비불투명)는 후보마다 알파 테스트. CullFlags = RAY_FLAG_CULL_BACK_FACING_TRIANGLES 등
// (양면 머티리얼 인스턴스는 TRIANGLE_CULL_DISABLE이라 컬링되지 않는다)
#define E_RT_DEFINE_TRACE_CLOSEST(Name, CullFlags)                                                                 \
	FRayHit Name(RayDesc Ray, uint Mask)                                                                           \
	{                                                                                                              \
		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | CullFlags> Query;                                           \
		Query.TraceRayInline(SceneTlas, RAY_FLAG_NONE, Mask, Ray);                                                 \
		while (Query.Proceed())                                                                                    \
		{                                                                                                          \
			if (Query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&                                          \
			    PassesAlphaTest(Query.CandidateInstanceID(), Query.CandidatePrimitiveIndex(), Query.CandidateTriangleBarycentrics())) \
			{                                                                                                      \
				Query.CommitNonOpaqueTriangleHit();                                                                \
			}                                                                                                      \
		}                                                                                                          \
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
	while (Query.Proceed())
	{
		if (Query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
		    PassesAlphaTest(Query.CandidateInstanceID(), Query.CandidatePrimitiveIndex(), Query.CandidateTriangleBarycentrics()))
		{
			Query.CommitNonOpaqueTriangleHit();
		}
	}
	return Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? Query.CommittedRayT() : -1.0f;
}

// ---- 히트 표면 (정점 보간 + 머티리얼 평가)

// MaterialDefault.hlsli 바인드리스 평가 (MaterialCommon.hlsli 머리 주석 "레이 트레이싱에서 쓰는 법")
static float               RtMaterialLod = 0.0f;
static FRayTracingMaterial RtHitMaterial;
#define MATERIAL_SAMPLE(Texture, Sampler, Uv) (Texture).SampleLevel(Sampler, Uv, RtMaterialLod)
#define E_MATERIAL_CUSTOM_RESOURCES
#define E_MATERIAL_DEFAULT_CONSTANTS RtHitMaterial
#define E_MATERIAL_TEXTURE(Index) BindlessTextures[NonUniformResourceIndex(RtHitMaterial.TextureTable + (Index))]
#define E_MATERIAL_SAMPLER_WRAP RtWrapSampler
#define E_MATERIAL_SAMPLER_CLAMP RtClampSampler
#include "MaterialCommon.hlsli"
#include "MaterialDefault.hlsli"

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
	const uint3  Tri = LoadHitTriangle(Instance, Hit.Primitive);
	const float3 W   = float3(1.0f - Hit.Barycentrics.x - Hit.Barycentrics.y, Hit.Barycentrics.x, Hit.Barycentrics.y);
	const FHitVertex V0 = LoadHitVertex(Instance.VertexBuffer, Tri.x);
	const FHitVertex V1 = LoadHitVertex(Instance.VertexBuffer, Tri.y);
	const FHitVertex V2 = LoadHitVertex(Instance.VertexBuffer, Tri.z);

	const float3x3 ObjectToWorld3 = (float3x3)Hit.ObjectToWorld;
	const float3   P0 = mul(Hit.ObjectToWorld, float4(V0.Position, 1.0f));
	const float3   P1 = mul(Hit.ObjectToWorld, float4(V1.Position, 1.0f));
	const float3   P2 = mul(Hit.ObjectToWorld, float4(V2.Position, 1.0f));
	const float3   FaceCross = cross(P1 - P0, P2 - P0);

	FMaterialPixelInputs In;
	In.WorldPosition = P0 * W.x + P1 * W.y + P2 * W.z;
	// 법선: 역전치 = (WorldToObject 3x3)ᵀ → 행벡터 곱
	In.WorldNormal   = normalize(mul(V0.Normal * W.x + V1.Normal * W.y + V2.Normal * W.z, (float3x3)Hit.WorldToObject));
	const float4 Tangent = V0.Tangent * W.x + V1.Tangent * W.y + V2.Tangent * W.z;
	const float  Handedness = (Instance.Flags & E_RT_INFO_MIRRORED) != 0 ? -1.0f : 1.0f;
	In.WorldTangent  = float4(normalize(mul(ObjectToWorld3, Tangent.xyz)), Tangent.w * Handedness);
	In.UV0           = V0.UV * W.x + V1.UV * W.y + V2.UV * W.z;
	In.VertexColor   = V0.Color * W.x + V1.Color * W.y + V2.Color * W.z;
	In.CameraVector  = View;
	In.PixelPosition = 0.0f;
	In.bFrontFace    = Hit.bFrontFace;

	// 텍스처 LOD: 광선 원뿔 (베이스 컬러 텍스처 크기 기준)
	uint TexWidth, TexHeight, MipCount;
	BindlessTextures[NonUniformResourceIndex(RtHitMaterial.TextureTable)].GetDimensions(0, TexWidth, TexHeight, MipCount);
	const float2 Duv1 = V1.UV - V0.UV;
	const float2 Duv2 = V2.UV - V0.UV;
	const float  UvArea    = abs(Duv1.x * Duv2.y - Duv1.y * Duv2.x);
	const float  WorldArea = length(FaceCross);
	const float  TexelArea = max(UvArea * (float)TexWidth * (float)TexHeight, 1.0e-12f);
	float3       FaceNormal = WorldArea > 1.0e-12f ? FaceCross / WorldArea : In.WorldNormal;
	const float  CosNormal  = max(abs(dot(FaceNormal, View)), 1.0e-3f);
	RtMaterialLod = clamp(0.5f * log2(TexelArea / max(WorldArea, 1.0e-12f)) + log2(max(ConeWidth, 1.0e-6f) / CosNormal), 0.0f, (float)MipCount - 1.0f);

	FMaterialSurface Material;
	EvaluateMaterial(In, Material);

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