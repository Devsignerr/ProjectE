#ifndef E_MATERIAL_COMMON_HLSLI
#define E_MATERIAL_COMMON_HLSLI

// 머티리얼 평가 인터페이스 (Phase 49 사이드 — 그래프 → HLSL 함수). 모든 머티리얼은 같은 함수 하나로 표면을 낸다:
//
//   void EvaluateMaterial(in FMaterialPixelInputs In, out FMaterialSurface Out);
//
//   - 고정 PBR(.emat에 Graph 없음): MaterialDefault.hlsli (b2 Material + t0~t4, 기존 식 그대로)
//   - 그래프 머티리얼: 가상 포함 파일 "MaterialGraph.generated.hlsli" (FMaterialGraphCompiler가 만든 문자열, 셰이더 디파인 E_MATERIAL_GRAPH)
//   메시 패스(Mesh.hlsl 메인/사전/반투명/가산 × 변형 8개)와 Masked 그림자(Shadow.hlsl ShadowMaterialPS)가 같은 함수를 include해 쓴다.
//
// ---- 레이 트레이싱(DXR 히트 셰이더)·계산 셰이더(DDGI 표면 평가)에서 쓰는 법
//   1) 파생 함수가 없으므로 샘플 매크로를 먼저 정의한다 (밉 선택: 광선 원뿔/거리로 고른 LOD 또는 SampleGrad):
//        #define MATERIAL_SAMPLE(Texture, Sampler, Uv) (Texture).SampleLevel(Sampler, Uv, MaterialLod)   // static float MaterialLod = ...
//      E_MATERIAL_MIP_BIAS를 정의하면 SampleBias(바이어스)가 된다 — 래스터 메시 패스(Mesh.hlsl)는 PerFrame MaterialMipBias(TAAU log2(내부/출력),
//      네이티브 0)로 항상 정의한다. 정의가 없으면 Sample (그림자 Masked 패스).
//      생성 코드와 MaterialDefault는 ddx/ddy/Sample을 직접 쓰지 않는다 (모든 텍스처 읽기가 이 매크로).
//   2) 리소스: 기본은 생성 코드가 cbuffer(E_MATERIAL_CBUFFER_REGISTER = b2)와 Texture2D MaterialTexture<i>(t<i>, E_MATERIAL_TEXTURE_SPACE = space2)를 선언한다.
//      바인드리스/로컬 루트 시그니처로 바꾸려면 E_MATERIAL_CUSTOM_RESOURCES를 정의하고 다음 매크로를 직접 정의한다:
//        E_MATERIAL_HEADER        float4 (x = 시간 초, y = 알파 컷오프 — FMaterialGraphHeader)
//        E_MATERIAL_PARAM(i)      float4 파라미터 레지스터 i (0 ~ E_MATERIAL_GRAPH_CONSTANT_REGISTERS-1, FMaterialParameterLayout)
//        E_MATERIAL_TEXTURE(i)    Texture2D 칸 i (0 ~ E_MATERIAL_GRAPH_TEXTURE_COUNT-1, FMaterial::Textures 순서)
//      예) #define E_MATERIAL_TEXTURE(i) MaterialTextures[NonUniformResourceIndex(HitMaterial.TextureBase + (i))]
//      샘플러 이름은 E_MATERIAL_SAMPLER_WRAP(반복, 메시 s0 이방성) / E_MATERIAL_SAMPLER_CLAMP(가장자리 고정)로 바꿀 수 있다.
//   3) 입력을 채운다 (FMaterialPixelInputs — 히트 지점: 무게중심 보간한 월드 위치/법선/탄젠트/UV/정점 색, CameraVector = -WorldRayDirection()).
//      PixelPosition은 화면 픽셀 위치(래스터 전용 — 생성 코드는 읽지 않는다). bFrontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE.
//   4) EvaluateMaterial(In, Surface) 후: Normal은 탄젠트 공간 → MaterialTangentToWorld(In, Surface.Normal)로 월드 법선,
//      Metallic은 saturate, 양면 뒷면이면 법선을 뒤집는다(Mesh.hlsl MakeMeshSurface와 같은 순서). Masked면 OpacityMask < E_MATERIAL_ALPHA_CUTOFF를
//      any-hit에서 IgnoreHit, 반투명은 Opacity로 섞는다.
//   그래프 셰이더 소스는 FMaterial::Shader->Hlsl(해시 = 변형 키), 셰이더 컴파일은 FShaderCompileDesc::VirtualFiles에 이 이름으로 넣는다.

struct FMaterialPixelInputs
{
	float3 WorldPosition;
	float3 WorldNormal;   // 기하(정점) 법선, 정규화 — 양면 뒷면도 뒤집지 않은 값
	float4 WorldTangent;  // xyz = +U 방향(정규화 전 보간값 가능), w = 바이탄젠트 부호
	float2 UV0;
	float4 VertexColor;
	float3 CameraVector;  // 표면 → 카메라 (정규화). 그림자 패스는 WorldNormal
	float2 PixelPosition; // SV_Position.xy (래스터 전용)
	bool   bFrontFace;
};

struct FMaterialSurface
{
	float3 BaseColor;        // 선형
	float  Metallic;         // 호출 쪽이 saturate
	float  Roughness;        // 지각적 거칠기 (메인 패스가 0.045~1로 클램프)
	float3 Normal;           // 탄젠트 공간 (+Z = 기하 법선). 정규화하지 않아도 된다
	float  AmbientOcclusion; // 1 = 가림 없음 (간접광에만)
	float3 Emissive;         // HDR 선형
	float  Opacity;          // Translucent/Additive 알파
	float  OpacityMask;      // Masked: E_MATERIAL_ALPHA_CUTOFF보다 작으면 버림
};

// ---- 샘플 매크로 (위 사용법 1)
#ifndef MATERIAL_SAMPLE
	#ifdef E_MATERIAL_MIP_BIAS
		#define MATERIAL_SAMPLE(Texture, Sampler, Uv) (Texture).SampleBias(Sampler, Uv, E_MATERIAL_MIP_BIAS)
	#else
		#define MATERIAL_SAMPLE(Texture, Sampler, Uv) (Texture).Sample(Sampler, Uv)
	#endif
#endif

// ---- 그래프 머티리얼 리소스 (위 사용법 2)
#ifndef E_MATERIAL_CBUFFER_REGISTER
	#define E_MATERIAL_CBUFFER_REGISTER b2
#endif
#ifndef E_MATERIAL_TEXTURE_SPACE
	#define E_MATERIAL_TEXTURE_SPACE space2
#endif
#ifndef E_MATERIAL_HEADER
	#define E_MATERIAL_HEADER MaterialHeader
#endif
#ifndef E_MATERIAL_PARAM
	#define E_MATERIAL_PARAM(Index) MaterialParams[Index]
#endif
#ifndef E_MATERIAL_TEXTURE
	#define E_MATERIAL_TEXTURE(Index) MaterialTexture##Index
#endif
#ifndef E_MATERIAL_TIME
	#define E_MATERIAL_TIME (E_MATERIAL_HEADER.x)
#endif
#ifndef E_MATERIAL_SAMPLER_WRAP
	#define E_MATERIAL_SAMPLER_WRAP LinearSampler
#endif
#ifndef E_MATERIAL_SAMPLER_CLAMP
	#define E_MATERIAL_SAMPLER_CLAMP IblSampler
#endif

// ---- 생성 코드가 부르는 공용 식 (노드 Fresnel/BlendNormals, 노멀 텍스처 용도)

// 노멀 맵 샘플 → 탄젠트 노멀 (XY만 저장 — BC5. Z 재구성), A = 1
float4 MaterialDecodeNormal(float4 Sample)
{
	float3 Normal;
	Normal.xy = Sample.xy * 2.0f - 1.0f;
	Normal.z  = sqrt(saturate(1.0f - dot(Normal.xy, Normal.xy)));
	return float4(Normal, 1.0f);
}

// Schlick 프레넬 계수: Base + (1 - Base) * (1 - N·V)^Exponent
float MaterialFresnel(float3 Normal, float3 CameraVector, float Exponent, float BaseReflectFraction)
{
	const float NdotV = saturate(dot(normalize(Normal), CameraVector));
	return BaseReflectFraction + (1.0f - BaseReflectFraction) * pow(max(1.0f - NdotV, 0.000001f), Exponent);
}

// Reoriented Normal Mapping (Barré-Brisebois & Hill 2012): 탄젠트 노멀 두 장 (Base 위에 Detail)
float3 MaterialBlendNormalsRnm(float3 Base, float3 Detail)
{
	const float3 T = Base + float3(0.0f, 0.0f, 1.0f);
	const float3 U = Detail * float3(-1.0f, -1.0f, 1.0f);
	return normalize(T * dot(T, U) / T.z - U);
}

// 탄젠트 공간 노멀 → 월드 (보간으로 틀어진 탄젠트를 법선에 다시 직교화, B = cross(N, T) * w)
float3 MaterialTangentToWorld(float3 WorldNormal, float4 WorldTangent, float3 TangentNormal)
{
	const float3 N = WorldNormal;
	const float3 T = normalize(WorldTangent.xyz - N * dot(N, WorldTangent.xyz));
	const float3 B = cross(N, T) * WorldTangent.w;
	return normalize(T * TangentNormal.x + B * TangentNormal.y + N * TangentNormal.z);
}

#endif // E_MATERIAL_COMMON_HLSLI
