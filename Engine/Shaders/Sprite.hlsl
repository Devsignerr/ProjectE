// 2D 스프라이트 (Renderer/SpriteRenderer.h 머리 주석이 기준). 메시 루트 시그니처를 그대로 쓰므로 Mesh.hlsl의 바인딩·조명 함수를 포함한다
//   (방향광 + 캐스케이드 그림자, 클러스터 로컬 라이트(그림자 포함), 하늘 IBL/DDGI, 반투명 안개 b6/t23 — 반투명 메시 패스와 같은 바인딩).
// 정점 버퍼 없음: 인스턴스마다 정점 6개(사각형 두 삼각형)를 SV_VertexID로 만든다. 인스턴스 = 묶음 시작(b0 InstanceOffset) + SV_InstanceID
//   (타일맵 청크 구간은 b0 최상위 비트 — 아래 FSpriteChunk).
// 텍스처는 바인드리스 — 셰이더 가시 힙 전체 표(공간 3, Mesh.hlsl LightTextures)를 인스턴스의 칸 번호로 읽는다.
// 픽셀 셰이더 = 블렌드 모드별 엔트리 (SpriteRenderer ESpriteBlendMode), 조명은 디파인 E_SPRITE_LIT 변형.
#include "Mesh.hlsl"

#include "SpriteCommon.hlsli"

// 메시 인스턴스 목록(t13)과 같은 루트 SRV 자리 — 스프라이트 패스는 이 자리에 스프라이트 인스턴스 버퍼를 묶는다 (Mesh.hlsl의 Instances는 이 셰이더에서 쓰지 않음)
StructuredBuffer<FSpriteInstance> SpriteInstances : register(t13);
// 청크 구간(b0 최상위 비트)의 청크 머리 하나 — 메시 인스턴스 번호 목록 자리 (스프라이트 셰이더는 안 쓰는 자리). 그때 t13 = 청크 정적 버퍼(0번부터)
StructuredBuffer<FSpriteChunk> SpriteChunkHeader : register(t14);
// 직전 프레임 사각형 (Masked 움직임 벡터 — 메시의 스킨 팔레트 자리, 스프라이트 셰이더는 스킨을 쓰지 않음). 항목 구간 = 인스턴스와 같은 번호,
// 청크 구간 = 칸 하나(직전 청크 월드)
StructuredBuffer<FSpritePrev> SpritePrevious : register(t15);

struct FSpriteVSOutput
{
	float4 Position                     : SV_Position;
	float4 CurrentClip                  : TEXCOORD4; // 지터 없음 (움직임 벡터 — Masked만 쓴다)
	float4 PreviousClip                 : TEXCOORD5;
	float3 WorldPosition                : POSITION0;
	float3 WorldNormal                  : NORMAL;
	float2 UV                           : TEXCOORD0;
	float4 Color                        : COLOR;
	nointerpolation uint  TextureIndex  : TEXCOORD1;
	nointerpolation uint  Flags         : TEXCOORD2;
	nointerpolation float AlphaCutoff   : TEXCOORD3;
	nointerpolation uint  Flash         : TEXCOORD6; // 번쩍임 RGBA8 (0 = 없음)
};

FSpriteVSOutput SpriteVS(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	const bool      bChunk = (InstanceOffset & E_SPRITE_CHUNK_BIT) != 0;
	FSpriteInstance Sprite = SpriteInstances[bChunk ? InstanceId : InstanceOffset + InstanceId];
	const float2    Corner = SpriteCorners[VertexId];
	float3          PreviousWorld;
	uint            Flash = 0;
	if (bChunk)
	{
		PreviousWorld = GetSpriteChunkCornerWorld(Sprite, SpritePrevious[0], Corner); // 로컬 인스턴스 그대로 (청크 적용 전)
		ApplySpriteChunk(Sprite, SpriteChunkHeader[0]);
	}
	else
	{
		const FSpritePrev Previous = SpritePrevious[InstanceOffset + InstanceId];
		PreviousWorld              = Previous.Origin + Previous.AxisX * Corner.x + Previous.AxisZ * Corner.y;
		Flash                      = Previous.Flash;
	}
	const float3 World = GetSpriteCornerWorld(Sprite, Corner);

	FSpriteVSOutput Output;
	Output.Position      = mul(float4(World, 1.0f), ViewProjection); // 지터 포함 (씬 컬러에 그리는 패스)
	Output.CurrentClip   = mul(float4(World, 1.0f), UnjitteredViewProjection);
	Output.PreviousClip  = mul(float4(PreviousWorld, 1.0f), PrevViewProjection);
	Output.WorldPosition = World;
	// 앞 = 로컬 +Y (2D 카메라는 +Y에서 -Y를 본다 — 화면 오른쪽 +X, 위 +Z). 픽셀 셰이더가 카메라 쪽으로 뒤집는다 (양면)
	Output.WorldNormal   = cross(Sprite.AxisZ, Sprite.AxisX); // (0,0,1) × (1,0,0) = (0,1,0)
	Output.UV            = GetSpriteCornerUV(Sprite, Corner);
	Output.Color         = Sprite.Color;
	Output.TextureIndex  = Sprite.TextureIndex;
	Output.Flags         = Sprite.Flags;
	Output.AlphaCutoff   = Sprite.AlphaCutoff;
	Output.Flash         = Flash;
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
		Texel = LoadSpritePoint(Texture, Input.UV);
	}
	else
	{
		Texel = Texture.SampleBias(IblSampler, Input.UV, MaterialMipBias);
	}
	return Texel * Input.Color;
}

// 번쩍임 (SpriteRenderer PackFlash와 같은 순서): 조명·안개를 지난 색을 번쩍임 색으로 덮는다 (A = 덮는 정도)
float3 ApplySpriteFlash(float3 Color, uint Packed)
{
	if (Packed == 0)
	{
		return Color;
	}
	const float4 Flash = float4(Packed & 255u, (Packed >> 8) & 255u, (Packed >> 16) & 255u, Packed >> 24) / 255.0f;
	return lerp(Color, Flash.rgb, Flash.a);
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
// E_SPRITE_STATIC (정지 변형 — SpriteRenderer.h 머리 주석 "TAA"): 그리는 값·카메라 기준이 직전 프레임과 같은 스프라이트는 알파(반응형)를
//   건드리지 않는다. 블렌드가 Premultiplied(색 ONE/INV_SRC_ALPHA, 알파 ZERO/ONE)라 Alpha는 rgb × a를 여기서 곱한다

// Alpha (Src·SrcA + Dst·(1 - SrcA)): 자기 색에 투과율 + 산란
float4 SpritePSAlpha(FSpriteVSOutput Input) : SV_Target
{
	const float4 Base  = SampleSprite(Input);
	const float4 Fog   = EvaluateFog(Input.WorldPosition);
	const float3 Color = ApplySpriteFlash(ShadeSprite(Input, Base.rgb) * Fog.a + Fog.rgb, Input.Flash);
	const float  Alpha = saturate(Base.a);
#ifdef E_SPRITE_STATIC
	return float4(Color * Alpha, Alpha);
#else
	return float4(Color, Alpha);
#endif
}

// Premultiplied (Src + Dst·(1 - SrcA), 알파도 같은 식 — EBlendMode::PremultipliedOver): 텍스처 rgb가 이미 알파를 곱한 값.
// 색 틴트는 프리멀티플라이드 규칙대로 rgb × 색 × 색 알파. 산란은 덮인 만큼
float4 SpritePSPremultiplied(FSpriteVSOutput Input) : SV_Target
{
	const float4 Texel = SampleSprite(Input) * float4(Input.Color.aaa, 1.0f); // (텍스처 × 색) rgb에 색 알파를 한 번 더
	const float4 Fog   = EvaluateFog(Input.WorldPosition);
	return float4(ApplySpriteFlash(ShadeSprite(Input, Texel.rgb) * Fog.a + Fog.rgb * saturate(Texel.a), Input.Flash), saturate(Texel.a));
}

// Additive (Src + Dst): 색 × 알파를 더한다. 빛을 더하므로 안개는 투과율만
float4 SpritePSAdditive(FSpriteVSOutput Input) : SV_Target
{
	const float4 Base     = SampleSprite(Input);
	const float  Coverage = saturate(Base.a);
#ifdef E_SPRITE_STATIC
	const float Reactive = 0.0f; // 알파는 더하기라 0이면 그대로
#else
	const float Reactive = Coverage;
#endif
	return float4(ShadeSprite(Input, Base.rgb) * Coverage * EvaluateFog(Input.WorldPosition).a, Reactive);
}

// Masked (블렌드 없음, 깊이 씀): 알파 < 컷오프면 버림. 움직임 벡터(SV_Target1 — 메시 사전 패스와 같은 식)를 쓰므로 알파 0(반응형 아님) —
// 움직이는 카메라·스프라이트도 메시처럼 이력을 재투영한다 (HD-2D)
struct FSpriteMaskedOutput
{
	float4 Color    : SV_Target0;
	float2 Velocity : SV_Target1; // R16G16_FLOAT, UV 단위 현재 - 이전
};

FSpriteMaskedOutput SpritePSMasked(FSpriteVSOutput Input)
{
	const float4 Base = SampleSprite(Input);
	clip(Base.a - Input.AlphaCutoff);
	const float4 Fog = EvaluateFog(Input.WorldPosition);
	FSpriteMaskedOutput Output;
	Output.Color    = float4(ApplySpriteFlash(ShadeSprite(Input, Base.rgb) * Fog.a + Fog.rgb, Input.Flash), 0.0f);
	Output.Velocity = ComputeVelocity(Input.CurrentClip, Input.PreviousClip);
	return Output;
}
