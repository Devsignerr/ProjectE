// 2D 스프라이트·타일맵 그림자 깊이 (Renderer/SpriteShadowRenderer.h 머리 주석이 기준). 방향광 캐스케이드·로컬 그림자 장마다 깊이만 그린다.
// 정점은 Sprite.hlsl과 같은 사각형 식(SpriteCommon.hlsli), 투영만 광원 뷰-투영. 모든 블렌드가 알파 컷오프로 clip,
// r.Sprite.TranslucentShadows면 알파·프리멀티플라이드 블렌드는 그림자 맵 텍셀 고정 4x4 디더 (덮는 비율 = 알파 — 시간에 고정).
#include "SpriteCommon.hlsli"

// 루트 상수 b0 (FSpriteShadowRenderer: 16 + 1)
cbuffer SpriteShadowConstants : register(b0)
{
	float4x4 LightViewProjection;
	uint     RunInfo; // E_SPRITE_CHUNK_BIT = 청크 구간 (t0 = 청크 정적 버퍼, t1 = 청크 머리), 아니면 항목 구간 (t0 = 프레임 업로드 버퍼의 구간 시작)
};
StructuredBuffer<FSpriteInstance> SpriteInstances   : register(t0);
StructuredBuffer<FSpriteChunk>    SpriteChunkHeader : register(t1);
Texture2D                         SpriteTextures[]  : register(t0, space1); // 셰이더 가시 힙 전체 (바인드리스)
SamplerState                      LinearClamp       : register(s0);

struct FSpriteShadowVSOutput
{
	float4 Position                     : SV_Position;
	float2 UV                           : TEXCOORD0;
	nointerpolation float Alpha         : TEXCOORD1;
	nointerpolation uint  TextureIndex  : TEXCOORD2;
	nointerpolation uint  Flags         : TEXCOORD3;
	nointerpolation float AlphaCutoff   : TEXCOORD4;
};

FSpriteShadowVSOutput SpriteShadowVS(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	FSpriteInstance Sprite = SpriteInstances[InstanceId];
	if ((RunInfo & E_SPRITE_CHUNK_BIT) != 0)
	{
		ApplySpriteChunk(Sprite, SpriteChunkHeader[0]);
	}
	if ((RunInfo & E_SPRITE_RUN_DITHER_BIT) != 0)
	{
		Sprite.Flags |= E_SPRITE_FLAG_SHADOW_DITHER;
	}
	const float2          Corner = SpriteCorners[VertexId];
	FSpriteShadowVSOutput Output;
	Output.Position     = mul(float4(GetSpriteCornerWorld(Sprite, Corner), 1.0f), LightViewProjection);
	Output.UV           = GetSpriteCornerUV(Sprite, Corner);
	Output.Alpha        = Sprite.Color.a;
	Output.TextureIndex = Sprite.TextureIndex;
	Output.Flags        = Sprite.Flags;
	Output.AlphaCutoff  = Sprite.AlphaCutoff;
	return Output;
}

// 알파 = 텍셀 알파 × 색 알파 (Sprite.hlsl SampleSprite와 같은 텍셀 — Point는 밉 0 Load, Linear는 밉 0 선형: 그림자 해상도는 화면과 달라 밉 바이어스 없음)
void SpriteShadowPS(FSpriteShadowVSOutput Input)
{
	Texture2D Texture = SpriteTextures[NonUniformResourceIndex(Input.TextureIndex)];
	float     Alpha;
	if ((Input.Flags & E_SPRITE_FLAG_POINT) != 0)
	{
		Alpha = LoadSpritePoint(Texture, Input.UV).a;
	}
	else
	{
		Alpha = Texture.SampleLevel(LinearClamp, Input.UV, 0.0f).a;
	}
	if ((Input.Flags & E_SPRITE_FLAG_SHADOW_DITHER) != 0)
	{
		// 4x4 Bayer 문턱 (i + 0.5) / 16 — 그림자 맵 텍셀 위치 고정: 알파 1은 모두, 0은 하나도 남지 않는다
		static const float Bayer[16] = { 0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f };
		const uint2 Cell = uint2(Input.Position.xy) & 3u;
		clip(Alpha * Input.Alpha - (Bayer[Cell.y * 4u + Cell.x] + 0.5f) / 16.0f);
		return;
	}
	clip(Alpha * Input.Alpha - Input.AlphaCutoff);
}
