#include "HdrDisplay.hlsli"

// HDR 출력 최종 합성 (Phase 49, FD3D12HdrOutput — RHI EndFrame): 씬(선형 BT.709, 1 = 종이 흰색, 하이라이트 펼침 포함) +
// 겹침 층(UI/ImGui/디버그 선 — sRGB 인코딩, 투명 배경에 프리멀티플라이드 "over"로 쌓인 값)을 선형에서 합친 뒤 출력 형식으로 인코딩
//   Mode 0 = HDR10 (BT.2020 + PQ, R10G10B10A2), 1 = scRGB (선형 BT.709, 1 = 80 nits, FP16), 2 = SDR 미리보기 (sRGB, 스크린샷)

cbuffer HdrCompositeConstants : register(b0)
{
	uint  Mode;
	float PaperWhiteNits;
	float CompositePadding0;
	float CompositePadding1;
};

Texture2D<float4> SceneHdr : register(t0);
Texture2D<float4> Overlay  : register(t1);

struct FVSOutput
{
	float4 Position : SV_Position;
};

FVSOutput VSMain(uint VertexId : SV_VertexID)
{
	FVSOutput    Out;
	const float2 UV = float2((VertexId << 1) & 2, VertexId & 2);
	Out.Position    = float4(UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return Out;
}

float4 PSMain(FVSOutput Input) : SV_Target0
{
	const int3   Pixel  = int3(int2(Input.Position.xy), 0);
	const float3 Scene  = max(SceneHdr.Load(Pixel).rgb, 0.0f);
	const float4 Layer  = Overlay.Load(Pixel);
	const float3 Over   = Layer.a > 1.0e-4f ? HdrSrgbToLinear(saturate(Layer.rgb / Layer.a)) * Layer.a : 0.0f;
	const float3 Linear = Scene * (1.0f - Layer.a) + Over; // 종이 흰색 단위
	if (Mode == 0)
	{
		return float4(HdrPqEncode(HdrRec709ToRec2020(Linear) * (PaperWhiteNits / E_HDR_PQ_MAX_NITS)), 1.0f);
	}
	if (Mode == 1)
	{
		return float4(Linear * (PaperWhiteNits / E_HDR_SCRGB_WHITE), 1.0f);
	}
	return float4(HdrLinearToSrgb(saturate(Linear)), 1.0f);
}
