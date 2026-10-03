#include "Scene/IrradianceVolume.h"

#include "Core/Reflection/TypeInfo.h"

void RegisterIrradianceVolumeTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	if (Registry.IsRegistered<FIrradianceVolumeComponent>())
	{
		return;
	}
	Registry.RegisterType<FIrradianceVolumeComponent>("IrradianceVolumeComponent", "GI 프로브 볼륨 (DDGI)")
		.Property(&FIrradianceVolumeComponent::HalfExtents, "HalfExtents", "상자 반 크기 (cm)").Range(10.0f, 100000.0f, 1.0f)
		.Tooltip("엔티티 위치가 가운데, 월드 축 정렬 (회전/스케일 무시). 칸 가운데에 프로브, 상자 = 셰이딩 범위 (방보다 조금 크게)")
		.Property(&FIrradianceVolumeComponent::Spacing, "Spacing", "프로브 간격 (cm)").Range(10.0f, 10000.0f, 1.0f)
		.Tooltip("축마다 최대 32개, 볼륨당 최대 8192개 (넘으면 간격을 늘린다)")
		.Property(&FIrradianceVolumeComponent::Intensity, "Intensity", "세기").Range(0.0f, 10.0f, 0.01f)
		.Property(&FIrradianceVolumeComponent::FadeDistance, "FadeDistance", "경계 페이드 (cm)").Range(1.0f, 10000.0f, 1.0f)
		.Property(&FIrradianceVolumeComponent::NormalBias, "NormalBias", "법선 편향 (cm)").Range(0.0f, 1000.0f, 0.5f)
		.Tooltip("셰이딩 점을 법선 쪽으로 민다 (벽 너머 프로브 누수·얼룩이 보이면 키운다, 간격의 10~30%)")
		.Property(&FIrradianceVolumeComponent::ViewBias, "ViewBias", "시선 편향 (cm)").Range(0.0f, 1000.0f, 0.5f)
		.Property(&FIrradianceVolumeComponent::Hysteresis, "Hysteresis", "누적 히스테리시스").Range(0.0f, 0.995f, 0.005f)
		.Tooltip("이전 값 비중 (클수록 안정하지만 조명 변화를 늦게 따라감 — 급변은 자동으로 빠르게)")
		.Property(&FIrradianceVolumeComponent::RaysPerProbe, "RaysPerProbe", "프로브당 광선").Range(64.0f, 512.0f, 1.0f)
		.Tooltip("고정 32개는 재배치·분류용, 나머지가 조도·거리 누적")
		.Property(&FIrradianceVolumeComponent::ProbeUpdateBudget, "ProbeUpdateBudget", "프레임당 갱신 프로브").Range(0.0f, 8192.0f, 1.0f)
		.Tooltip("0 = 매 프레임 전부 (r.DDGI.ProbeBudget이 전체 상한)")
		.Property(&FIrradianceVolumeComponent::MaxRayDistance, "MaxRayDistance", "광선 최대 거리 (cm)").Range(100.0f, 1000000.0f, 10.0f)
		.Property(&FIrradianceVolumeComponent::bRelocation, "Relocation", "프로브 재배치")
		.Tooltip("벽에 묻히거나 너무 가까운 프로브를 칸 안에서 밀어낸다")
		.Property(&FIrradianceVolumeComponent::bClassification, "Classification", "벽 속 프로브 끄기")
		.Tooltip("뒷면을 많이 본 프로브(벽 속)는 셰이딩에서 뺀다 (실내 빛 누수 방지)")
		.Property(&FIrradianceVolumeComponent::Priority, "Priority", "우선순위").Tooltip("겹치면 큰 값이 먼저 (같으면 작은 상자)")
		.Property(&FIrradianceVolumeComponent::DebugProbes, "DebugProbes", "프로브 표시").Range(0.0f, 3.0f, 1.0f)
		.Tooltip("0 없음, 1 조도, 2 거리(평균), 3 상태 (초록 활성 / 빨강 벽 속 / 파랑 아직 없음). r.DDGI.ShowProbes가 0 이상이면 그 값")
		.Property(&FIrradianceVolumeComponent::DebugProbeRadius, "DebugProbeRadius", "프로브 구 반지름 (cm)").Range(1.0f, 100.0f, 0.5f)
		.NoReplicate()
		.AsComponent();
}
