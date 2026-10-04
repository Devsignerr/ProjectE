// 2D 스프라이트 (Renderer/SpriteRenderer.h 머리 주석이 기준). 메시 루트 시그니처를 그대로 쓰므로 Mesh.hlsl의 바인딩·조명 함수를 포함한다
//   (방향광 + 캐스케이드 그림자, 클러스터 로컬 라이트(그림자 포함), 하늘 IBL/DDGI, 반투명 안개 b6/t23 — 반투명 메시 패스와 같은 바인딩).
// 정점 버퍼 없음: 인스턴스마다 정점 6개(사각형 두 삼각형)를 SV_VertexID로 만든다. 인스턴스 = 묶음 시작(b0 InstanceOffset) + SV_InstanceID.
// 텍스처는 바인드리스 — 셰이더 가시 힙 전체 표(공간 3, Mesh.hlsl LightTextures)를 인스턴스의 칸 번호로 읽는다.
// 픽셀 셰이더 = 블렌드 모드별 엔트리 (SpriteRenderer ESpriteBlendMode), 조명은 디파인 E_SPRITE_LIT 변형.
#include "Mesh.hlsl"

// ShaderTypes.h FSpriteInstanceGpu와 1:1 (80바이트)
struct FSpriteInstance
{
	float3 Origin;
	uint   TextureIndex;
	float3 AxisX;
	uint   Flags;
	float3 AxisZ;
	float  AlphaCutoff;
	float4 UVRect;
	float4 Color;
};

// 메시 인스턴스 목록(t13)과 같은 루트 SRV 자리 — 스프라이트 패스는 이 자리에 스프라이트 인스턴스 버퍼를 묶는다 (Mesh.hlsl의 Instances는 이 셰이더에서 쓰지 않음)
StructuredBuffer<FSpriteInstance> SpriteInstances : register(t13);

static const uint E_SPRITE_FLAG_POINT = 1u; // SpriteRenderer SpriteFlag_Point

struct FSpriteVSOutput
{
	float4 Position                     : SV_Position;
	float3 WorldPosition                : POSITION0;
	float3 WorldNormal                  : NORMAL;
	float2 UV                           : TEXCOORD0;
	float4 Color                        : COLOR;
	nointerpolation uint  TextureIndex  : TEXCOORD1;
	nointerpolation uint  Flags         : TEXCOORD2;
	nointerpolation float AlphaCutoff   : TEXCOORD3;
};

// 사각형 모서리 (u = 로컬 X, v = 로컬 위) — 두 삼각형. 양면이라 와인딩은 상관없다 (컬링 없음)
static const float2 SpriteCorners[6] = { float2(0.0f, 0.0f), float2(0.0f, 1.0f), float2(1.0f, 1.0f),
	                                     float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(1.0f, 0.0f) };

FSpriteVSOutput SpriteVS(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	const FSpriteInstance Sprite = SpriteInstances[InstanceOffset + InstanceId];
	const float2          Corner = SpriteCorners[VertexId];
	const float3          World  = Sprite.Origin + Sprite.AxisX * Corner.x + Sprite.AxisZ * Corner.y;

	FSpriteVSOutput Output;
	Output.Position      = mul(float4(World, 1.0f), ViewProjection); // 지터 포함 (씬 컬러에 그리는 패스)
	Output.WorldPosition = World;
	// 앞 = 로컬 +Y (2D 카메라는 +Y에서 -Y를 본다 — 화면 오른쪽 +X, 위 +Z). 픽셀 셰이더가 카메라 쪽으로 뒤집는다 (양면)
	Output.WorldNormal   = cross(Sprite.AxisZ, Sprite.AxisX); // (0,0,1) × (1,0,0) = (0,1,0)
	// SpriteMath::GetCornerUV와 같은 식: 텍스처 v는 아래로 증가하므로 로컬 위(v = 1)가 UVRect.y
	Output.UV            = lerp(Sprite.UVRect.xy, Sprite.UVRect.zw, float2(Corner.x, 1.0f - Corner.y));
	Output.Color         = Sprite.Color;
	Output.TextureIndex  = Sprite.TextureIndex;
	Output.Flags         = Sprite.Flags;
	Output.AlphaCutoff   = Sprite.AlphaCutoff;
	return Output;
}

// 텍스처 × 색 (선형 — 색 텍스처는 sRGB 포맷이라 읽으면 선형)
// Point: 밉 0 텍셀을 그대로 (Load). 점 필터를 밉이 있는 텍스처에 샘플러로 걸면 축소 때 흐린 밉이 골라져 픽셀 아트가 번진다 —
//   스프라이트는 보통 확대해서 보므로 항상 밉 0 (축소하면 에일리어싱은 감수). 메시 루트에 점 샘플러가 없어 Load로 고른다 (클램프)
// Linear: 선형 클램프 + TAAU 밉 바이어스 (메시 머티리얼과 같은 MaterialMipBias)
float4 SampleSprite(FSpriteVSOutput Input)
{
	Texture2D Texture = E_LIGHT_TEXTURE(Input.TextureIndex);
	float4    Texel;
	if ((Input.Flags & E_SPRITE_FLAG_POINT) != 0)
	{
		uint Width, Height;
		Texture.GetDimensions(Width, Height);
		const int2 Size  = int2(Width, Height);
		const int2 Pixel = clamp(int2(floor(Input.UV * float2(Size))), int2(0, 0), Size - 1);
		Texel = Texture.Load(int3(Pixel, 0));
	}
	else
	{
		Texel = Texture.SampleBias(IblSampler, Input.UV, MaterialMipBias);
	}
	return Texel * Input.Color;
}

// 조명: 언릿 = 알베도 그대로. 릿 = 반투명 메시와 같은 EvaluateMeshLighting (방향광 + 섀도맵, 로컬 라이트 + 그림자, IBL/DDGI — 화면 버퍼(SSR/SSAO/데칼) 없음).
// 표면 = 거칠기 1 유전체, 법선 = 사각형 앞을 카메라 쪽으로 (양면)
float3 ShadeSprite(FSpriteVSOutput Input, float3 Albedo)
{
#ifdef E_SPRITE_LIT
	const float3 V = normalize(CameraPosition - Input.WorldPosition);
	float3       N = normalize(Input.WorldNormal);
	N              = dot(N, V) < 0.0f ? -N : N;
	FSurface Surface;
	Surface.Albedo    = Albedo;
	Surface.Metallic  = 0.0f;
	Surface.Roughness = 1.0f;
	Surface.N         = N;
	Surface.V         = V;
	Surface.Occlusion = 1.0f;
	return EvaluateMeshLighting(Surface, Input.WorldPosition, N, Input.Position.xy, false);
#else
	return Albedo;
#endif
}

// 출력 알파 = 덮인 정도 (TAA 반응형 마스크 — 반투명 메시·파티클과 같은 규칙). 안개는 반투명 규칙대로 직접 (Fog.hlsli EvaluateFog)

// Alpha (Src·SrcA + Dst·(1 - SrcA)): 자기 색에 투과율 + 산란
float4 SpritePSAlpha(FSpriteVSOutput Input) : SV_Target
{
	const float4 Base = SampleSprite(Input);
	const float4 Fog  = EvaluateFog(Input.WorldPosition);
	return float4(ShadeSprite(Input, Base.rgb) * Fog.a + Fog.rgb, saturate(Base.a));
}

// Premultiplied (Src + Dst·(1 - SrcA), 알파도 같은 식 — EBlendMode::PremultipliedOver): 텍스처 rgb가 이미 알파를 곱한 값.
// 색 틴트는 프리멀티플라이드 규칙대로 rgb × 색 × 색 알파. 산란은 덮인 만큼
float4 SpritePSPremultiplied(FSpriteVSOutput Input) : SV_Target
{
	const float4 Texel = SampleSprite(Input) * float4(Input.Color.aaa, 1.0f); // (텍스처 × 색) rgb에 색 알파를 한 번 더
	const float4 Fog   = EvaluateFog(Input.WorldPosition);
	return float4(ShadeSprite(Input, Texel.rgb) * Fog.a + Fog.rgb * saturate(Texel.a), saturate(Texel.a));
}

// Additive (Src + Dst): 색 × 알파를 더한다. 빛을 더하므로 안개는 투과율만
float4 SpritePSAdditive(FSpriteVSOutput Input) : SV_Target
{
	const float4 Base     = SampleSprite(Input);
	const float  Coverage = saturate(Base.a);
	return float4(ShadeSprite(Input, Base.rgb) * Coverage * EvaluateFog(Input.WorldPosition).a, Coverage);
}

// Masked (블렌드 없음, 깊이 씀): 알파 < 컷오프면 버림. 움직임 벡터를 쓰지 않으므로 알파 1(반응형) — 움직이는 스프라이트가 이력에 끌리지 않게
float4 SpritePSMasked(FSpriteVSOutput Input) : SV_Target
{
	const float4 Base = SampleSprite(Input);
	clip(Base.a - Input.AlphaCutoff);
	const float4 Fog = EvaluateFog(Input.WorldPosition);
	return float4(ShadeSprite(Input, Base.rgb) * Fog.a + Fog.rgb, 1.0f);
}
