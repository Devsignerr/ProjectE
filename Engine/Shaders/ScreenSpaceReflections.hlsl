#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// SSR (FScreenSpaceReflections): 계층 깊이(Hi-Z, 칸마다 가장 가까운 깊이) 레이마칭. 식은 Renderer/ReflectionMath.h
//   CSHizCopy / CSHizDownsample : 사전 패스 깊이 → 최소 깊이 밉 체인 (계산, 밉마다 UAV)
//   PSTrace (전체 화면)        : 뷰 공간 반사 방향을 화면 공간 (UV, 깊이) 직선으로 바꿔 Hi-Z를 오르내리며 교차점을 찾고,
//                                두께 검사 → 이전 프레임 씬 컬러를 재투영해 읽는다. 출력 = (반사 색, 신뢰도)
//   메인 패스(Mesh.hlsl)가 거칠기로 페이드해 IBL 반사(캡처/하늘) 대신 섞는다. 깊이는 표준(0 가까움, 1 멂)

// ---- Hi-Z 만들기 (루트 상수 4개 + UAV 표 2개 + SRV 표)
cbuffer HizConstants : register(b0)
{
	uint SourceWidth;
	uint SourceHeight;
	uint DestWidth;
	uint DestHeight;
};
Texture2D<float>   HizDepth  : register(t0);
RWTexture2D<float> HizDest   : register(u0);
RWTexture2D<float> HizSource : register(u1);

[numthreads(8, 8, 1)]
void CSHizCopy(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= uint2(DestWidth, DestHeight)))
	{
		return;
	}
	HizDest[Id.xy] = HizDepth.Load(int3(Id.xy, 0));
}

[numthreads(8, 8, 1)]
void CSHizDownsample(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= uint2(DestWidth, DestHeight)))
	{
		return;
	}
	// 2x2 최소 + 홀수 크기의 마지막 행/열은 한 칸 더 (보수적)
	const uint2 Base = Id.xy * 2;
	const uint2 Last = uint2(SourceWidth, SourceHeight) - 1;
	float       Min  = 1.0f;
	const uint  ExtraX = (Id.x == DestWidth - 1 && (SourceWidth & 1) != 0) ? 1 : 0;
	const uint  ExtraY = (Id.y == DestHeight - 1 && (SourceHeight & 1) != 0) ? 1 : 0;
	for (uint Y = 0; Y <= 1 + ExtraY; ++Y)
	{
		for (uint X = 0; X <= 1 + ExtraX; ++X)
		{
			Min = min(Min, HizSource[min(Base + uint2(X, Y), Last)]);
		}
	}
	HizDest[Id.xy] = Min;
}
