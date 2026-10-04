// 방향광 그림자 캐시 부분 되살리기 (Renderer/ShadowCacheMath.h "섀도우 맵 장 되살리기", FShadowRenderer):
// 지난 프레임 동적 2D 캐스터가 그린 텍셀 사각형(시저)만 캐시 깊이로 다시 쓴다 — D32는 서브리소스 일부 복사가 안 되므로 SV_Depth로 쓴다.
// 깊이 테스트 ALWAYS + 쓰기, 바이어스 0 (SV_Depth는 바이어스를 받지 않는다) → 캐시 값 그대로 (D32_FLOAT ↔ R32_FLOAT 비트 동일)
#include "Fullscreen.hlsli"

cbuffer RestoreConstants : register(b0)
{
	uint Slice; // 캐스케이드 (캐시 배열 장)
};
Texture2DArray<float> CacheDepth : register(t0);

FFullscreenVSOutput ShadowCacheRestoreVS(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float ShadowCacheRestorePS(FFullscreenVSOutput Input) : SV_Depth
{
	return CacheDepth.Load(int4(int2(Input.Position.xy), Slice, 0));
}
