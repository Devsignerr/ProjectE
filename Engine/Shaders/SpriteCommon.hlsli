// 2D 스프라이트 공용 (Sprite.hlsl 그리기 · SpriteShadow.hlsl 그림자 깊이 — Renderer/SpriteRenderer.h, SpriteShadowRenderer.h 머리 주석이 기준).
// 인스턴스·청크 머리 구조와 사각형 정점 식만 둔다 — 버퍼 레지스터는 포함하는 쪽이 정한다 (두 셰이더의 루트 시그니처가 다르다).

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

// 타일맵 청크 머리 (ShaderTypes.h FSpriteChunkGpu와 1:1, 64바이트). 청크 인스턴스는 타일맵 로컬 공간(Y = 0 평면)이고 머리가 월드로 옮긴다
struct FSpriteChunk
{
	float3 AxisX;
	uint   TextureIndex;
	float3 AxisZ;
	float  AlphaCutoff; // 타일맵 컴포넌트 AlphaCutoff (인스턴스 값을 덮는다)
	float3 Translation;
	uint   Pad1;
	float4 Color;
};

static const uint E_SPRITE_FLAG_POINT = 1u;           // SpriteRenderer SpriteFlag_Point / SpriteTiles::FlagPoint
static const uint E_SPRITE_CHUNK_BIT  = 0x80000000u;  // SpriteRenderer SpriteChunkRunBit (b0 최상위 비트 = 청크 구간)

// 사각형 모서리 (u = 로컬 X, v = 로컬 위) — 두 삼각형. 양면이라 와인딩은 상관없다 (컬링 없음)
static const float2 SpriteCorners[6] = { float2(0.0f, 0.0f), float2(0.0f, 1.0f), float2(1.0f, 1.0f),
	                                     float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(1.0f, 0.0f) };

// 청크 로컬(Y = 0 평면) → 월드: 위치는 이동 포함, 축은 방향만. 색은 곱하고 텍스처 칸·컷오프는 청크 것
void ApplySpriteChunk(inout FSpriteInstance Sprite, FSpriteChunk Chunk)
{
	Sprite.Origin       = Chunk.Translation + Chunk.AxisX * Sprite.Origin.x + Chunk.AxisZ * Sprite.Origin.z;
	Sprite.AxisX        = Chunk.AxisX * Sprite.AxisX.x + Chunk.AxisZ * Sprite.AxisX.z;
	Sprite.AxisZ        = Chunk.AxisX * Sprite.AxisZ.x + Chunk.AxisZ * Sprite.AxisZ.z;
	Sprite.Color       *= Chunk.Color;
	Sprite.TextureIndex = Chunk.TextureIndex;
	Sprite.AlphaCutoff  = Chunk.AlphaCutoff;
}

float3 GetSpriteCornerWorld(FSpriteInstance Sprite, float2 Corner)
{
	return Sprite.Origin + Sprite.AxisX * Corner.x + Sprite.AxisZ * Corner.y;
}

// SpriteMath::GetCornerUV와 같은 식: 텍스처 v는 아래로 증가하므로 로컬 위(v = 1)가 UVRect.y
float2 GetSpriteCornerUV(FSpriteInstance Sprite, float2 Corner)
{
	return lerp(Sprite.UVRect.xy, Sprite.UVRect.zw, float2(Corner.x, 1.0f - Corner.y));
}

// Point 필터: 밉 0 텍셀 그대로 (클램프). 점 필터를 밉이 있는 텍스처에 샘플러로 걸면 축소 때 흐린 밉이 골라져 픽셀 아트가 번진다
float4 LoadSpritePoint(Texture2D Texture, float2 UV)
{
	uint Width, Height;
	Texture.GetDimensions(Width, Height);
	const int2 Size  = int2(Width, Height);
	const int2 Pixel = clamp(int2(floor(UV * float2(Size))), int2(0, 0), Size - 1);
	return Texture.Load(int3(Pixel, 0));
}
