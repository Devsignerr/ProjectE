#ifndef E_SHADOW_COMMON_HLSLI
#define E_SHADOW_COMMON_HLSLI

// 방향광 캐스케이드 섀도우 상수 (ShadowRenderer.h FShadowConstants와 1:1). 메시 패스 b3, 볼류메트릭 안개 b1
//   레지스터는 포함하는 쪽이 E_SHADOW_CONSTANTS_REGISTER로 바꾼다

#ifndef E_SHADOW_CONSTANTS_REGISTER
#define E_SHADOW_CONSTANTS_REGISTER b3
#endif

cbuffer ShadowConstants : register(E_SHADOW_CONSTANTS_REGISTER)
{
	float4x4 CascadeViewProjection[4];
	float4   CascadeSplits;     // 뷰 공간 far 거리
	float4   CascadeTexelWorld; // 캐스케이드별 월드 텍셀 크기
	float3   ShadowCameraForward;
	float    ShadowEnabled;
	float    ShadowTexelSize;   // 1 / 해상도
	float    ShadowNormalOffset;
	uint     CascadeCount;
	uint     VisualizeCascades;
};

// 뷰 깊이(카메라 앞 방향 거리)로 캐스케이드 선택. CascadeCount = 그림자 거리 밖
uint SelectCascadeByDepth(float ViewDepth)
{
	[unroll]
	for (uint Index = 0; Index < 4; ++Index)
	{
		if (Index < CascadeCount && ViewDepth <= CascadeSplits[Index])
		{
			return Index;
		}
	}
	return CascadeCount;
}

#endif // E_SHADOW_COMMON_HLSLI
