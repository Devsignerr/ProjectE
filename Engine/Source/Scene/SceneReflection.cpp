#include "Scene/SceneReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimIK.h"
#include "Scene/Components.h"
#include "Scene/Gameplay.h"
#include "Scene/Particles.h"
#include "Scene/Prefab.h"
#include "Scene/SequencePlayer.h"
#include "Scene/SubScene.h"
#include "Scene/Foliage.h"
#include "Scene/Terrain.h"
#include "Scene/Building/BuildingScene.h"
#include "Scene/SkyAtmosphere.h"
#include "Scene/Ability/AbilityReflection.h"
#include "Scene/IrradianceVolume.h"
#include "Scene/Sprite/Sprite2DComponents.h"

void RegisterSceneTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();

	// 이름/계층은 인스펙터와 직렬화가 별도로 다루므로 목록에서 숨기고 제거 불가
	Registry.RegisterType<FNameComponent>("NameComponent", "이름")
		.Property(&FNameComponent::Name, "Name", "이름")
		.AsComponent(false)
		.Hide();

	// 계층은 복제 생성 메시지(부모 NetId)로, 생성됨 표식은 편집 전용이라 복제하지 않는다
	Registry.RegisterType<FHierarchyComponent>("HierarchyComponent", "계층")
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	Registry.RegisterType<FTransientComponent>("TransientComponent", "생성됨")
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	// WorldMatrix는 파생 값이므로 등록하지 않는다 (직렬화 제외)
	Registry.RegisterType<FTransformComponent>("TransformComponent", "트랜스폼")
		.Property(&FTransformComponent::Position, "Position", "위치").Range(-1.0e7f, 1.0e7f, 1.0f) // cm
		.Property(&FTransformComponent::Rotation, "Rotation", "회전")
		.Property(&FTransformComponent::Scale, "Scale", "스케일").Range(0.001f, 1000.0f, 0.02f)
		.AsComponent(false);

	// 런타임 핸들은 Transient, 에셋 참조 문자열이 직렬화 대상
	Registry.RegisterType<FStaticMeshComponent>("StaticMeshComponent", "스태틱 메시")
		.Property(&FStaticMeshComponent::Mesh, "Mesh", "메시", PF_Transient | PF_ReadOnly)
		.Property(&FStaticMeshComponent::Material, "Material", "머티리얼", PF_Transient | PF_ReadOnly)
		.Property(&FStaticMeshComponent::bVisible, "Visible", "표시")
		.Property(&FStaticMeshComponent::MeshAsset, "MeshAsset", "메시 에셋", PF_ReadOnly)
		.Property(&FStaticMeshComponent::MaterialAsset, "MaterialAsset", "머티리얼 에셋", PF_ReadOnly).AssetFilter(".emat")
		.AsComponent();

	Registry.RegisterType<FModelComponent>("ModelComponent", "모델")
		.Property(&FModelComponent::AssetPath, "AssetPath", "에셋 경로", PF_ReadOnly)
		.AsComponent();

	// 소켓 부착: 대상 모델과 소켓은 인스펙터 전용 UI(목록 선택)로 고른다
	Registry.RegisterType<FSocketAttachmentComponent>("SocketAttachmentComponent", "소켓 부착")
		.Property(&FSocketAttachmentComponent::Target, "Target", "대상 모델", PF_Hidden)
		.Property(&FSocketAttachmentComponent::Socket, "Socket", "소켓", PF_Hidden)
		.AsComponent();

	Registry.RegisterType<FDirectionalLightComponent>("DirectionalLightComponent", "방향광")
		.Property(&FDirectionalLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FDirectionalLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 50.0f, 0.05f)
		.Property(&FDirectionalLightComponent::CookieTexture, "CookieTexture", "쿠키 텍스처").AssetFilter(".png;.jpg;.jpeg;.tga;.bmp").Tooltip("빛에 수직인 평면에 반복 투영 (구름 그림자 등)")
		.Property(&FDirectionalLightComponent::CookieTileSize, "CookieTileSize", "쿠키 타일 크기 (cm)").Range(1.0f, 1000000.0f, 10.0f)
		.Property(&FDirectionalLightComponent::CookiePanSpeed, "CookiePanSpeed", "쿠키 이동 (타일/초)")
		.AsComponent();

	// 런타임 상태(Runtime)는 등록하지 않는다. FSkinComponent는 런타임 전용이라 등록하지 않음
	Registry.RegisterType<FAnimationComponent>("AnimationComponent", "애니메이션")
		.Property(&FAnimationComponent::Clip, "Clip", "클립")
		.Property(&FAnimationComponent::Speed, "Speed", "속도").Range(-5.0f, 5.0f, 0.01f)
		.Property(&FAnimationComponent::BlendTime, "BlendTime", "블렌드 시간").Range(0.0f, 5.0f, 0.01f)
		.Property(&FAnimationComponent::bPlaying, "Playing", "재생")
		.Property(&FAnimationComponent::bLoop, "Loop", "반복")
		.Property(&FAnimationComponent::bRootMotion, "RootMotion", "루트 모션 (이전)").Tooltip("켜면 루트 모션 모드 '전부'와 같다")
		.Property(&FAnimationComponent::RootMotionMode, "RootMotionMode", "루트 모션 모드")
		.Enum({ { "None", "없음" }, { "MontagesOnly", "몽타주만" }, { "All", "전부" } })
		.Tooltip("클립의 루트 뼈 이동을 엔티티 이동으로 옮긴다 (캐릭터 이동 컴포넌트가 있으면 그 이동으로)")
		.Property(&FAnimationComponent::RootMotionBone, "RootMotionBone", "루트 모션 뼈").Tooltip("비우면 클립마다 최상위 이동 뼈")
		.Property(&FAnimationComponent::bRootMotionRotation, "RootMotionRotation", "루트 모션 회전").Tooltip("루트 뼈의 위쪽 축 회전도 추출")
		.Property(&FAnimationComponent::RetargetSources, "RetargetSources", "리타기팅 소스")
		.Tooltip("다른 스켈레톤 모델 경로 (Content 기준, ';' 구분) — 그 모델의 클립을 이 모델에 맞춰 덧붙인다")
		.AsComponent();

	Registry.RegisterType<FCameraComponent>("CameraComponent", "카메라")
		.Property(&FCameraComponent::FovYDegrees, "FovYDegrees", "시야각 (도)").Range(5.0f, 170.0f, 0.5f)
		.Property(&FCameraComponent::NearZ, "NearZ", "근평면 (cm)").Range(0.1f, 10000.0f, 0.5f)
		.Property(&FCameraComponent::FarZ, "FarZ", "원평면 (cm)").Range(100.0f, 10000000.0f, 100.0f)
		.Property(&FCameraComponent::bPrimary, "Primary", "주 카메라")
		.Property(&FCameraComponent::Priority, "Priority", "우선순위").Tooltip("주 카메라가 여럿이면 큰 값이 이긴다")
		.Property(&FCameraComponent::bOrthographic, "Orthographic", "직교 투영")
		.Property(&FCameraComponent::OrthoHeight, "OrthoHeight", "직교 높이 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.AsComponent();

	Registry.RegisterType<FPixelArtComponent>("PixelArtComponent", "픽셀 아트")
		.Property(&FPixelArtComponent::bEnabled, "Enabled", "사용")
		.Property(&FPixelArtComponent::PixelSize, "PixelSize", "도트 크기 (px)").Range(1.0f, 16.0f)
		.Property(&FPixelArtComponent::bSnapCamera, "SnapCamera", "카메라 도트 스냅")
		.Property(&FPixelArtComponent::OutlineStrength, "OutlineStrength", "외곽선").Range(0.0f, 1.0f, 0.01f)
		.Property(&FPixelArtComponent::HighlightStrength, "HighlightStrength", "모서리 하이라이트").Range(0.0f, 2.0f, 0.01f)
		.Property(&FPixelArtComponent::DepthThreshold, "DepthThreshold", "외곽선 깊이 차 (cm)").Range(0.1f, 1000.0f, 0.5f)
		.Property(&FPixelArtComponent::ColorLevels, "ColorLevels", "색 단계 (0 = 끔)").Range(0.0f, 32.0f)
		.Property(&FPixelArtComponent::DitherStrength, "DitherStrength", "디더").Range(0.0f, 1.0f, 0.01f)
		.Property(&FPixelArtComponent::bSnapMovingObjects, "SnapMovingObjects", "움직이는 물체 도트 스냅")
		.AsComponent();

	Registry.RegisterType<FDepthOfFieldComponent>("DepthOfFieldComponent", "피사계 심도")
		.Property(&FDepthOfFieldComponent::bEnabled, "Enabled", "사용")
		.Property(&FDepthOfFieldComponent::FocusDistance, "FocusDistance", "초점 거리 (cm)").Range(1.0f, 1000000.0f, 5.0f)
		.Tooltip("카메라에서 초점면까지의 뷰 깊이")
		.Property(&FDepthOfFieldComponent::FocalRegion, "FocalRegion", "초점 영역 (cm)").Range(0.0f, 1000000.0f, 5.0f)
		.Tooltip("초점 거리 앞뒤로 이만큼은 완전히 선명")
		.Property(&FDepthOfFieldComponent::NearTransition, "NearTransition", "근경 전환 (cm)").Range(0.0f, 1000000.0f, 5.0f)
		.Tooltip("초점 영역 앞에서 최대 흐림까지의 거리 (0 = 바로 최대)")
		.Property(&FDepthOfFieldComponent::FarTransition, "FarTransition", "원경 전환 (cm)").Range(0.0f, 1000000.0f, 5.0f)
		.Tooltip("초점 영역 뒤에서 최대 흐림까지의 거리 (0 = 바로 최대)")
		.Property(&FDepthOfFieldComponent::NearBlurSize, "NearBlurSize", "근경 흐림 (화면 높이 %)").Range(0.0f, 4.0f, 0.01f)
		.Property(&FDepthOfFieldComponent::FarBlurSize, "FarBlurSize", "원경 흐림 (화면 높이 %)").Range(0.0f, 4.0f, 0.01f)
		.Property(&FDepthOfFieldComponent::bPreviewInEditor, "PreviewInEditor", "에디터 카메라에도 적용")
		.Tooltip("끄면 플레이 중 게임 카메라 시점(빙의)·런타임에서만 보인다")
		.Property(&FDepthOfFieldComponent::Mode, "Mode", "방식")
		.Enum({ { "Depth", "깊이" }, { "TiltShift", "틸트시프트 (화면 위치)" }, { "DepthAndTiltShift", "깊이 + 틸트시프트" } })
		.Tooltip("틸트시프트 = 화면 위아래로 갈수록 흐린 미니어처 사진 (HD-2D). 둘 다 = 두 흐림 중 큰 쪽")
		.Property(&FDepthOfFieldComponent::TiltShiftCenter, "TiltShiftCenter", "틸트시프트 초점 위치").Range(0.0f, 1.0f, 0.005f)
		.Tooltip("선명한 띠 가운데의 화면 높이 (0 = 위, 1 = 아래)")
		.Property(&FDepthOfFieldComponent::TiltShiftBand, "TiltShiftBand", "틸트시프트 선명 폭").Range(0.0f, 1.0f, 0.005f)
		.Tooltip("선명한 띠 반폭 (화면 높이 비율)")
		.Property(&FDepthOfFieldComponent::TiltShiftTransition, "TiltShiftTransition", "틸트시프트 전환 폭").Range(0.0f, 2.0f, 0.005f)
		.Tooltip("띠 밖에서 최대 흐림(근경/원경 흐림)까지의 거리 (화면 높이 비율, 0 = 바로 최대). 띠 아래 = 근경, 위 = 원경")
		.Property(&FDepthOfFieldComponent::TiltShiftAngle, "TiltShiftAngle", "틸트시프트 기울기 (도)").Range(-90.0f, 90.0f, 0.5f)
		.Property(&FDepthOfFieldComponent::BokehBladeCount, "BokehBladeCount", "보케 조리개 날 수").Range(0.0f, 12.0f, 1.0f)
		.Tooltip("0 = 둥근 보케, 5~8 = 다각형 빛망울 (카메라 조리개 모양)")
		.Property(&FDepthOfFieldComponent::BokehRotation, "BokehRotation", "보케 회전 (도)").Range(-180.0f, 180.0f, 0.5f)
		.Property(&FDepthOfFieldComponent::BokehHighlightBoost, "BokehHighlightBoost", "보케 하이라이트 강조").Range(0.0f, 20.0f, 0.05f)
		.Tooltip("흐린 영역의 밝은 점(등불·불티·반딧불)을 또렷한 빛망울로 남긴다 (0 = 끔)")
		.Property(&FDepthOfFieldComponent::BokehHighlightThreshold, "BokehHighlightThreshold", "보케 하이라이트 문턱").Range(0.0f, 100.0f, 0.05f)
		.Tooltip("강조가 시작되는 HDR 밝기 (노출 전 최대 채널)")
		.AsComponent();

	Registry.RegisterType<FColorGradingComponent>("ColorGradingComponent", "색 보정")
		.Property(&FColorGradingComponent::bEnabled, "Enabled", "사용")
		.Property(&FColorGradingComponent::Temperature, "Temperature", "색온도").Range(-1.0f, 1.0f, 0.005f).Tooltip("- 차갑게 / + 따뜻하게 (밝기 유지)")
		.Property(&FColorGradingComponent::Tint, "Tint", "틴트").Range(-1.0f, 1.0f, 0.005f).Tooltip("- 초록 / + 자홍")
		.Property(&FColorGradingComponent::Saturation, "Saturation", "채도").Range(0.0f, 3.0f, 0.005f)
		.Property(&FColorGradingComponent::Contrast, "Contrast", "대비").Range(0.0f, 3.0f, 0.005f)
		.Property(&FColorGradingComponent::Lift, "Lift", "Lift (어두운 쪽)").Tooltip("채널별 어두운 쪽 올림 (0 = 그대로)")
		.Property(&FColorGradingComponent::Gamma, "Gamma", "Gamma (중간톤)").Tooltip("채널별 중간톤 (1 = 그대로, 크면 밝게)")
		.Property(&FColorGradingComponent::Gain, "Gain", "Gain (밝은 쪽)").Tooltip("채널별 배율 (1 = 그대로)")
		.Property(&FColorGradingComponent::LookupTable, "LookupTable", "LUT 이미지").AssetFilter(".png;.tga;.bmp;.jpg")
		.Tooltip("색 조회 띠 (가로 = N², 세로 = N — 256x16, 1024x32). 보정 뒤에 적용")
		.Property(&FColorGradingComponent::LookupTableIntensity, "LookupTableIntensity", "LUT 세기").Range(0.0f, 1.0f, 0.01f)
		.AsComponent();

	Registry.RegisterType<FVignetteComponent>("VignetteComponent", "비네트")
		.Property(&FVignetteComponent::bEnabled, "Enabled", "사용")
		.Property(&FVignetteComponent::Intensity, "Intensity", "세기").Range(0.0f, 1.0f, 0.005f)
		.Property(&FVignetteComponent::Size, "Size", "크기").Range(0.0f, 1.5f, 0.005f).Tooltip("어두워지기 시작하는 반경 (가운데 0 ~ 모서리 1)")
		.Property(&FVignetteComponent::Smoothness, "Smoothness", "부드러움").Range(0.01f, 2.0f, 0.005f)
		.Property(&FVignetteComponent::Roundness, "Roundness", "둥글기").Range(0.0f, 1.0f, 0.01f).Tooltip("1 = 원, 0 = 화면 모양 타원")
		.Property(&FVignetteComponent::Color, "Color", "색", PF_Color)
		.AsComponent();

	// 스크립트 Properties 오버라이드는 인스펙터가 스크립트 선언을 읽어 전용 UI로 편집한다
	Registry.RegisterType<FScriptComponent>("ScriptComponent", "스크립트")
		.Property(&FScriptComponent::ScriptAsset, "ScriptAsset", "스크립트").AssetFilter(".lua")
		.Property(&FScriptComponent::PropertyOverrides, "PropertyOverrides", "프로퍼티 오버라이드", PF_Hidden)
		.Property(&FScriptComponent::ExecutionLocation, "ExecutionLocation", "실행 위치 (0 서버, 1 클라이언트, 2 양쪽)").Range(0.0f, 2.0f, 1.0f)
		.AsComponent();

	// 파티클: 에셋(.eparticle)은 콘텐츠 브라우저의 파티클 편집기에서 고친다
	Registry.RegisterType<FParticleSystemComponent>("ParticleSystemComponent", "파티클")
		.Property(&FParticleSystemComponent::Asset, "Asset", "파티클 에셋").AssetFilter(".eparticle")
		.Property(&FParticleSystemComponent::bPlaying, "Playing", "재생")
		.Property(&FParticleSystemComponent::Speed, "Speed", "속도").Range(0.0f, 10.0f, 0.01f)
		.AsComponent();

	// 프리팹: 인스턴스 루트 표식 + 엔티티 연결. 인스펙터는 전용 머리글로 보여 주고(목록에서 숨김), 연결 해제로만 없앤다.
	// 복제는 프리팹 생성 메시지(경로 + 링크 ID)로 클라이언트가 직접 만들므로 이 컴포넌트들은 보내지 않는다
	Registry.RegisterType<FPrefabInstanceComponent>("PrefabInstanceComponent", "프리팹 인스턴스")
		.Property(&FPrefabInstanceComponent::Asset, "Asset", "프리팹", PF_ReadOnly).AssetFilter(".eprefab")
		.Property(&FPrefabInstanceComponent::Overrides, "Overrides", "오버라이드", PF_Hidden | PF_ReadOnly)
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	Registry.RegisterType<FPrefabLinkComponent>("PrefabLinkComponent", "프리팹 연결")
		.Property(&FPrefabLinkComponent::Id, "Id", "프리팹 안 ID", PF_ReadOnly)
		.Property(&FPrefabLinkComponent::Root, "Root", "인스턴스 루트", PF_ReadOnly)
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	// 점광원/스포트라이트 (Phase 23): 색은 sRGB, 렌더러가 선형으로 바꾼다
	Registry.RegisterType<FPointLightComponent>("PointLightComponent", "점광원")
		.Property(&FPointLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FPointLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 1000.0f, 0.05f).Tooltip("1m 거리 밝기 (거리 제곱 반비례)")
		.Property(&FPointLightComponent::Radius, "Radius", "반경 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FPointLightComponent::bCastShadows, "CastShadows", "그림자")
		.Property(&FPointLightComponent::SpecularScale, "SpecularScale", "정반사 배율").Range(0.0f, 1.0f, 0.01f).Tooltip("1 = 물리적, 0 = 하이라이트 없이 확산만 (보조광 — 발광 메시가 반사를 대신할 때)")
		.Property(&FPointLightComponent::IesProfile, "IesProfile", "IES 프로필").AssetFilter(".ies").Tooltip("배광 분포 (IESNA LM-63). 0° = 라이트 Forward(+X)")
		.Property(&FPointLightComponent::bUseIesIntensity, "UseIesIntensity", "IES 밝기 사용").Tooltip("켜면 Intensity 대신 프로필 최대 칸델라 × 0.01 × 배율")
		.Property(&FPointLightComponent::IesIntensityScale, "IesIntensityScale", "IES 밝기 배율").Range(0.0f, 100.0f, 0.01f)
		.Property(&FPointLightComponent::CookieTexture, "CookieTexture", "쿠키 텍스처").AssetFilter(".png;.jpg;.jpeg;.tga;.bmp").Tooltip("빛 색에 곱하는 투영 텍스처 (창살·패턴·깜빡임)")
		.Property(&FPointLightComponent::CookieScale, "CookieScale", "쿠키 반복").Tooltip("0이면 그 축은 한 점만 읽는다 (패닝과 함께 깜빡임)")
		.Property(&FPointLightComponent::CookiePanSpeed, "CookiePanSpeed", "쿠키 이동 (uv/초)")
		.AsComponent();

	Registry.RegisterType<FSpotLightComponent>("SpotLightComponent", "스포트라이트")
		.Property(&FSpotLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FSpotLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 1000.0f, 0.05f).Tooltip("1m 거리 밝기 (거리 제곱 반비례)")
		.Property(&FSpotLightComponent::Radius, "Radius", "반경 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FSpotLightComponent::InnerConeAngle, "InnerConeAngle", "내부 원뿔 (도)").Range(0.0f, 80.0f, 0.1f)
		.Property(&FSpotLightComponent::OuterConeAngle, "OuterConeAngle", "외부 원뿔 (도)").Range(1.0f, 80.0f, 0.1f)
		.Property(&FSpotLightComponent::bCastShadows, "CastShadows", "그림자")
		.Property(&FSpotLightComponent::SpecularScale, "SpecularScale", "정반사 배율").Range(0.0f, 1.0f, 0.01f).Tooltip("1 = 물리적, 0 = 하이라이트 없이 확산만 (보조광 — 발광 메시가 반사를 대신할 때)")
		.Property(&FSpotLightComponent::IesProfile, "IesProfile", "IES 프로필").AssetFilter(".ies").Tooltip("배광 분포 (IESNA LM-63). 0° = 라이트 Forward(+X)")
		.Property(&FSpotLightComponent::bUseIesIntensity, "UseIesIntensity", "IES 밝기 사용").Tooltip("켜면 Intensity 대신 프로필 최대 칸델라 × 0.01 × 배율")
		.Property(&FSpotLightComponent::IesIntensityScale, "IesIntensityScale", "IES 밝기 배율").Range(0.0f, 100.0f, 0.01f)
		.Property(&FSpotLightComponent::CookieTexture, "CookieTexture", "쿠키 텍스처").AssetFilter(".png;.jpg;.jpeg;.tga;.bmp").Tooltip("빛 색에 곱하는 투영 텍스처 (창살·패턴·깜빡임)")
		.Property(&FSpotLightComponent::CookieScale, "CookieScale", "쿠키 반복").Tooltip("0이면 그 축은 한 점만 읽는다 (패닝과 함께 깜빡임)")
		.Property(&FSpotLightComponent::CookiePanSpeed, "CookiePanSpeed", "쿠키 이동 (uv/초)")
		.AsComponent();

	Registry.RegisterType<FSkyLightComponent>("SkyLightComponent", "하늘광")
		.Property(&FSkyLightComponent::Intensity, "Intensity", "환경광 배율").Range(0.0f, 10.0f, 0.01f).Tooltip("IBL 환경광과 하늘 배경 밝기 (씬에서 첫 하늘광만 사용)")
		.Property(&FSkyLightComponent::EnvironmentMap, "EnvironmentMap", "환경맵 (HDR)").AssetFilter(".hdr").Tooltip("비우면 절차적 하늘")
		.Property(&FSkyLightComponent::EnvironmentRotation, "EnvironmentRotation", "환경맵 회전 (도)").Range(-360.0f, 360.0f, 0.5f)
		.AsComponent();

	// 게임플레이 (Scene/Gameplay.h): 체력은 서버 권위 — 복제되면 클라이언트는 값만 읽는다
	Registry.RegisterType<FHealthComponent>("HealthComponent", "체력")
		.Property(&FHealthComponent::MaxHealth, "MaxHealth", "최대 체력").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FHealthComponent::Health, "Health", "체력").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FHealthComponent::bInvulnerable, "Invulnerable", "무적")
		.Property(&FHealthComponent::DeathAction, "DeathAction", "사망 시")
		.Enum({ { "Auto", "자동 (플레이어면 리스폰)" }, { "Destroy", "파괴" }, { "RespawnInPlace", "제자리 리스폰" }, { "RespawnAtPlayerStart", "PlayerStart 리스폰" } })
		.Property(&FHealthComponent::ScoreValue, "ScoreValue", "처치 점수").Tooltip("처치한 플레이어가 받는 점수 (게임 모드 진행 중)")
		.AsComponent();

	// 게임 모드 + 게임 상태: 규칙은 저장, 상태는 저장하지 않고 복제 (멀티플레이는 ReplicatedComponent도 붙인다)
	Registry.RegisterType<FGameModeComponent>("GameModeComponent", "게임 모드")
		.Property(&FGameModeComponent::StartDelay, "StartDelay", "시작 대기 (초)").Range(0.0f, 3600.0f, 0.1f)
		.Property(&FGameModeComponent::TimeLimit, "TimeLimit", "제한 시간 (초, 0 = 없음)").Range(0.0f, 36000.0f, 1.0f)
		.Property(&FGameModeComponent::ScoreToWin, "ScoreToWin", "목표 점수 (0 = 없음)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FGameModeComponent::RespawnDelay, "RespawnDelay", "리스폰 지연 (초)").Range(-1.0f, 600.0f, 0.1f).Tooltip("음수면 리스폰하지 않는다")
		.Property(&FGameModeComponent::PlayerPrefab, "PlayerPrefab", "플레이어 프리팹 (이 씬)").AssetFilter(".eprefab")
		.Tooltip("멀티플레이: 이 씬에서 입장한 플레이어에게 만들 프리팹. 비면 프로젝트 설정 → 맵 → 플레이어 프리팹")
		.Property(&FGameModeComponent::MatchState, "MatchState", "매치 상태", PF_Transient | PF_ReadOnly)
		.Enum({ { "WaitingToStart", "대기" }, { "InProgress", "진행" }, { "Ended", "끝" } })
		.Property(&FGameModeComponent::RemainingSeconds, "RemainingSeconds", "남은 시간 (초)", PF_Transient | PF_ReadOnly)
		.Property(&FGameModeComponent::WinnerPlayerId, "WinnerPlayerId", "승자", PF_Transient | PF_ReadOnly)
		.Property(&FGameModeComponent::Scores, "Scores", "점수", PF_Transient | PF_ReadOnly)
		.AsComponent();

	// 애니메이션 그래프 (Scene/AnimGraph.h): 모델 루트(애니메이션 컴포넌트와 같은 엔티티)에 붙인다. 파라미터는 런타임 값 (복제 안 됨)
	Registry.RegisterType<FAnimGraphComponent>("AnimGraphComponent", "애니메이션 그래프")
		.Property(&FAnimGraphComponent::Graph, "Graph", "그래프").AssetFilter(".eanimgraph")
		.Property(&FAnimGraphComponent::bUseCharacterMovement, "UseCharacterMovement", "캐릭터 이동 파라미터")
		.Tooltip("자신/조상의 캐릭터 이동 상태를 Speed·VerticalSpeed·Grounded 파라미터로 넣는다")
		.AsComponent();

	// 데칼 (Phase 33-4, Scene/Components.h FDecalComponent): 로컬 -Z로 찍는 상자. 머티리얼 핸들은 렌더러가 경로에서 해석
	Registry.RegisterType<FDecalComponent>("DecalComponent", "데칼")
		.Property(&FDecalComponent::MaterialAsset, "MaterialAsset", "머티리얼").AssetFilter(".emat")
		.Property(&FDecalComponent::Size, "Size", "크기 (cm)").Range(1.0f, 100000.0f, 1.0f).Tooltip("X, Y = 찍히는 면, Z = 투영 깊이 (로컬 -Z로 찍힘)")
		.Property(&FDecalComponent::Opacity, "Opacity", "불투명도").Range(0.0f, 1.0f, 0.01f)
		.Property(&FDecalComponent::SortOrder, "SortOrder", "정렬 순서").Tooltip("큰 값이 위에 그려진다")
		.Property(&FDecalComponent::bAffectBaseColor, "AffectBaseColor", "베이스 색")
		.Property(&FDecalComponent::bAffectNormal, "AffectNormal", "노멀")
		.Property(&FDecalComponent::bAffectRoughness, "AffectRoughness", "거칠기/금속")
		.Property(&FDecalComponent::FadeStartDistance, "FadeStartDistance", "페이드 시작 (cm)").Range(0.0f, 1000000.0f, 10.0f)
		.Property(&FDecalComponent::FadeEndDistance, "FadeEndDistance", "페이드 끝 (cm)").Range(0.0f, 1000000.0f, 10.0f).Tooltip("시작보다 작거나 같으면 페이드 없음")
		.Property(&FDecalComponent::Material, "Material", "머티리얼 핸들", PF_Transient | PF_ReadOnly)
		.AsComponent();

	// 높이 지수 안개 + 볼류메트릭 안개 (Phase 33-5, 씬 전역 — 첫 하나만). 기준 높이 = 엔티티 월드 Z
	Registry.RegisterType<FHeightFogComponent>("HeightFogComponent", "높이 안개")
		.Property(&FHeightFogComponent::Color, "Color", "안개 색", PF_Color)
		.Property(&FHeightFogComponent::Density, "Density", "밀도 (1/m)").Range(0.0f, 10.0f, 0.001f).Tooltip("엔티티 높이(월드 Z)에서의 밀도")
		.Property(&FHeightFogComponent::HeightFalloff, "HeightFalloff", "높이 감쇠 (1/m)").Range(0.0f, 10.0f, 0.005f)
		.Property(&FHeightFogComponent::StartDistance, "StartDistance", "시작 거리 (cm)").Range(0.0f, 1000000.0f, 10.0f)
		.Property(&FHeightFogComponent::MaxOpacity, "MaxOpacity", "최대 불투명도").Range(0.0f, 1.0f, 0.01f)
		.Property(&FHeightFogComponent::DirectionalInscatteringColor, "DirectionalInscatteringColor", "방향광 산란 색", PF_Color)
		.Property(&FHeightFogComponent::DirectionalInscatteringExponent, "DirectionalInscatteringExponent", "방향광 산란 지수").Range(1.0f, 64.0f, 0.1f)
		.Property(&FHeightFogComponent::DirectionalInscatteringStartDistance, "DirectionalInscatteringStartDistance", "방향광 산란 시작 (cm)")
		.Range(0.0f, 1000000.0f, 10.0f)
		.Property(&FHeightFogComponent::bVolumetric, "Volumetric", "볼류메트릭 안개").Tooltip("빛줄기·그림자가 보이는 3D 안개 (카메라 앞 거리까지)")
		.Property(&FHeightFogComponent::VolumetricDistance, "VolumetricDistance", "볼류메트릭 거리 (cm)").Range(500.0f, 100000.0f, 10.0f)
		.Property(&FHeightFogComponent::VolumetricAlbedo, "VolumetricAlbedo", "산란 비율 색", PF_Color)
		.Property(&FHeightFogComponent::VolumetricExtinctionScale, "VolumetricExtinctionScale", "소멸 배율").Range(0.0f, 10.0f, 0.01f)
		.Property(&FHeightFogComponent::VolumetricAnisotropy, "VolumetricAnisotropy", "비등방성 (g)").Range(-0.9f, 0.9f, 0.01f)
		.Property(&FHeightFogComponent::VolumetricDirectionalScale, "VolumetricDirectionalScale", "방향광 산란 배율").Range(0.0f, 20.0f, 0.01f)
		.Property(&FHeightFogComponent::VolumetricLocalLightScale, "VolumetricLocalLightScale", "로컬 라이트 산란 배율").Range(0.0f, 20.0f, 0.01f)
		.AsComponent();

	// 반사 캡처 (Phase 33-6): 에디터 도구 → 반사 캡처 굽기로 CaptureAsset(.ecapture)에 큐브맵을 굽는다
	Registry.RegisterType<FReflectionCaptureComponent>("ReflectionCaptureComponent", "반사 캡처")
		.Property(&FReflectionCaptureComponent::Shape, "Shape", "모양")
		.Enum({ { "Sphere", "구" }, { "Box", "상자 (시차 보정)" } })
		.Property(&FReflectionCaptureComponent::Radius, "Radius", "반경 (cm)").Range(10.0f, 100000.0f, 1.0f)
		.Property(&FReflectionCaptureComponent::BoxExtent, "BoxExtent", "상자 반 크기 (cm)").Range(10.0f, 100000.0f, 1.0f).Tooltip("월드 축 정렬 (회전 무시)")
		.Property(&FReflectionCaptureComponent::FadeDistance, "FadeDistance", "경계 페이드 (cm)").Range(1.0f, 10000.0f, 1.0f)
		.Property(&FReflectionCaptureComponent::Intensity, "Intensity", "세기").Range(0.0f, 10.0f, 0.01f)
		.Property(&FReflectionCaptureComponent::Priority, "Priority", "우선순위").Tooltip("겹치면 큰 값이 먼저")
		.Property(&FReflectionCaptureComponent::CaptureAsset, "CaptureAsset", "구운 큐브맵").AssetFilter(".ecapture")
		.AsComponent();
	// 서브 씬 스트리밍 (Scene/SubScene.h, Phase 31-2)
	Registry.RegisterType<FSubSceneVolumeComponent>("SubSceneVolumeComponent", "서브 씬 볼륨")
		.Property(&FSubSceneVolumeComponent::SubScene, "SubScene", "서브 씬").AssetFilter(".escene")
		.Property(&FSubSceneVolumeComponent::HalfExtents, "HalfExtents", "상자 반 크기 (cm)").Range(1.0f, 1.0e6f, 10.0f)
		.Property(&FSubSceneVolumeComponent::UnloadMargin, "UnloadMargin", "내리기 여유 (cm)").Range(0.0f, 1.0e6f, 10.0f)
		.Tooltip("기준이 상자 안이면 불러오고, 모든 기준이 상자 + 여유 밖이면 내린다")
		.AsComponent();
	Registry.RegisterType<FStreamingSourceComponent>("StreamingSourceComponent", "스트리밍 기준")
		.AsComponent();

	// 컷신 시퀀스 재생 (Scene/SequencePlayer.h, Phase 35-2): 복제하지 않는 로컬 연출 (각 프로세스가 자기 시계로 재생)
	Registry.RegisterType<FSequencePlayerComponent>("SequencePlayerComponent", "시퀀스 재생")
		.Property(&FSequencePlayerComponent::Sequence, "Sequence", "시퀀스").AssetFilter(".esequence")
		.Property(&FSequencePlayerComponent::bAutoPlay, "AutoPlay", "자동 재생")
		.Property(&FSequencePlayerComponent::bLoop, "Loop", "반복")
		.Property(&FSequencePlayerComponent::PlayRate, "PlayRate", "재생 속도").Range(0.0f, 10.0f, 0.01f)
		.Property(&FSequencePlayerComponent::bRestoreState, "RestoreState", "끝나면 원래 값으로")
		.Tooltip("끄면 마지막 값 유지 (열린 문 등). 카메라 컷은 항상 원래 카메라로 돌아간다")
		.NoReplicate()
		.AsComponent();
	// 지형 (Scene/Terrain.h, Phase 34)
	RegisterTerrainTypes();
	// 풀·나무 (Scene/Foliage.h, Phase 34-3)
	RegisterFoliageTypes();
	// 애니메이션 IK (Scene/AnimIK.h, Phase 42-4): 모델 루트(애니메이션 컴포넌트와 같은 엔티티)에 붙인다. 로컬 연출 (복제 안 함)
	Registry.RegisterType<FFootIkComponent>("FootIkComponent", "발 IK")
		.Property(&FFootIkComponent::bEnabled, "Enabled", "켜기")
		.Property(&FFootIkComponent::FootBones, "FootBones", "발 뼈 (쉼표)").Tooltip("끝 뼈 이름들. 각 끝 뼈의 부모(무릎)와 조부모(허벅지)가 2본 체인")
		.Property(&FFootIkComponent::PelvisBone, "PelvisBone", "골반 뼈").Tooltip("낮은 발에 맞춰 내린다 (비면 골반 보정 없음)")
		.Property(&FFootIkComponent::TraceUp, "TraceUp", "위로 탐색 (cm)").Range(0.0f, 500.0f, 1.0f)
		.Property(&FFootIkComponent::TraceDown, "TraceDown", "아래로 탐색 (cm)").Range(0.0f, 500.0f, 1.0f)
		.Property(&FFootIkComponent::MaxAdjust, "MaxAdjust", "최대 보정 (cm)").Range(0.0f, 200.0f, 0.5f)
		.Property(&FFootIkComponent::InterpSpeed, "InterpSpeed", "따라가기 속도").Range(0.0f, 100.0f, 0.1f)
		.Property(&FFootIkComponent::Weight, "Weight", "가중치").Range(0.0f, 1.0f, 0.01f)
		.Property(&FFootIkComponent::bAlignToGround, "AlignToGround", "바닥 기울기 맞춤")
		.Property(&FFootIkComponent::MaxAlignAngle, "MaxAlignAngle", "최대 기울기 (도)").Range(0.0f, 90.0f, 0.5f)
		.Property(&FFootIkComponent::KneeDirection, "KneeDirection", "무릎 방향 (모델)").Tooltip("0이면 애니메이션의 무릎 방향")
		.NoReplicate()
		.AsComponent();
	Registry.RegisterType<FLookAtComponent>("LookAtComponent", "시선")
		.Property(&FLookAtComponent::bEnabled, "Enabled", "켜기")
		.Property(&FLookAtComponent::Bones, "Bones", "뼈 (위 → 아래, 쉼표)").Tooltip("마지막 뼈가 목표를 본다. 회전은 뼈 개수로 나눠 위부터")
		.Property(&FLookAtComponent::Target, "Target", "목표 엔티티").Tooltip("Lua entity:SetLookAtTarget이 있으면 그쪽이 먼저")
		.Property(&FLookAtComponent::ForwardAxis, "ForwardAxis", "앞 방향 (모델)").Tooltip("기본 포즈에서 캐릭터가 보는 방향 (모델 공간)")
		.Property(&FLookAtComponent::MaxAngle, "MaxAngle", "최대 각 (도)").Range(0.0f, 180.0f, 0.5f)
		.Property(&FLookAtComponent::Weight, "Weight", "가중치").Range(0.0f, 1.0f, 0.01f)
		.Property(&FLookAtComponent::BlendSpeed, "BlendSpeed", "켜고 끄는 속도").Range(0.0f, 100.0f, 0.1f)
		.NoReplicate()
		.AsComponent();
	// 실내 절차적 생성 (Scene/Building/BuildingScene.h, Phase 50 사이드)
	RegisterBuildingTypes();
	// 하늘·대기·구름·시간대·물 (Scene/SkyAtmosphere.h, Phase 49)
	RegisterSkyTypes();
	// 능력 시스템 (Scene/Ability, Phase 53 사이드)
	RegisterAbilityTypes();

	// 면광원 (Phase 52, 식은 Renderer/AreaLightMath.h): 색은 sRGB, Intensity = 면 법선 방향 1m 조도 (점광원과 같은 단위)
	Registry.RegisterType<FAreaLightComponent>("AreaLightComponent", "면광원")
		.Property(&FAreaLightComponent::Shape, "Shape", "모양")
		.Enum({ { "Rect", "사각형" }, { "Disc", "원판" } })
		.Property(&FAreaLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FAreaLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 1000.0f, 0.05f).Tooltip("면 법선 방향 1m 거리 밝기 (점광원과 같은 단위, 면적이 커지면 휘도는 반비례)")
		.Property(&FAreaLightComponent::Width, "Width", "폭 (cm)").Range(1.0f, 10000.0f, 0.5f).Tooltip("로컬 Y 방향 (원판: 지름)")
		.Property(&FAreaLightComponent::Height, "Height", "높이 (cm)").Range(1.0f, 10000.0f, 0.5f).Tooltip("로컬 Z 방향 (원판: 세로 지름)")
		.Property(&FAreaLightComponent::Radius, "Radius", "반경 (cm)").Range(1.0f, 100000.0f, 1.0f).Tooltip("면에서 이 거리에서 빛이 0")
		.Property(&FAreaLightComponent::bTwoSided, "TwoSided", "양면")
		.Property(&FAreaLightComponent::BarnDoorAngle, "BarnDoorAngle", "문 덮개 각 (도)").Range(0.0f, 90.0f, 0.5f).Tooltip("법선 기준 반각, 90 = 없음")
		.Property(&FAreaLightComponent::BarnDoorLength, "BarnDoorLength", "문 덮개 길이 (cm)").Range(0.0f, 1000.0f, 0.5f).Tooltip("짧을수록 경계가 부드럽다")
		.Property(&FAreaLightComponent::bCastShadows, "CastShadows", "그림자")
		.Property(&FAreaLightComponent::SpecularScale, "SpecularScale", "정반사 배율").Range(0.0f, 1.0f, 0.01f).Tooltip("1 = 물리적, 0 = 하이라이트 없이 확산만 (보조광 — 발광 메시가 반사를 대신할 때)")
		.Property(&FAreaLightComponent::IesProfile, "IesProfile", "IES 프로필").AssetFilter(".ies").Tooltip("배광 분포 (IESNA LM-63). 0° = 라이트 Forward(+X)")
		.Property(&FAreaLightComponent::bUseIesIntensity, "UseIesIntensity", "IES 밝기 사용").Tooltip("켜면 Intensity 대신 프로필 최대 칸델라 × 0.01 × 배율")
		.Property(&FAreaLightComponent::IesIntensityScale, "IesIntensityScale", "IES 밝기 배율").Range(0.0f, 100.0f, 0.01f)
		.Property(&FAreaLightComponent::CookieTexture, "CookieTexture", "쿠키 텍스처").AssetFilter(".png;.jpg;.jpeg;.tga;.bmp").Tooltip("빛 색에 곱하는 투영 텍스처 (창살·패턴·깜빡임)")
		.Property(&FAreaLightComponent::CookieScale, "CookieScale", "쿠키 반복").Tooltip("0이면 그 축은 한 점만 읽는다 (패닝과 함께 깜빡임)")
		.Property(&FAreaLightComponent::CookiePanSpeed, "CookiePanSpeed", "쿠키 이동 (uv/초)")
		.AsComponent();
	// 동적 GI 프로브 볼륨 (Scene/IrradianceVolume.h, Phase 51 DDGI)
	RegisterIrradianceVolumeTypes();
	// 2D 스프라이트/플립북/타일맵 (Scene/Sprite/, Phase 56)
	RegisterSprite2DTypes();
}
