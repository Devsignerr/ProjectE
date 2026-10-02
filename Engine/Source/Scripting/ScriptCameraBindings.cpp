#include "Scripting/LuaRuntime.h"

#include "Scene/CameraProjection.h"
#include "Scene/Scene.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"

#include <stdexcept>
#include <tuple>

// Camera 테이블 — 월드 ↔ 게임 UI 좌표
//   local X, Y, bVisible = Camera.WorldToScreen(worldPos [, uiEntity])
//   local Origin, Direction = Camera.ScreenToWorldRay(x, y [, uiEntity])   -- 실패하면 nil
// 좌표 공간 = 게임 UI 레이아웃 좌표 (.eui 루트 캔버스 좌표, UI 배율 적용 후): 왼쪽 위 (0, 0), +Y 아래, 단위 = UI 단위.
//   루트 캔버스의 자식 위젯을 앵커 (0, 0)에 두면 widget.Position = Vector2(X, Y)로 바로 그 자리에 놓인다
//   (가운데 정렬은 위젯 Alignment(피벗) (0.5, 1) 등을 에셋에서 지정).
// 어느 UI의 배율인가: uiEntity(UIComponent가 있는 엔티티)를 주면 그 UI, 없으면 호출한 스크립트의 엔티티 → 조상 중 첫 UIComponent,
//   그것도 없으면 씬의 첫 보이는 UIComponent. 화면 영역(종횡비)은 그 UI의 뷰포트(= 게임 화면: 런타임 백버퍼 / 에디터 플레이 뷰포트)
// 카메라 = 렌더러와 같은 활성 카메라(FCameraProjection::FindActiveCamera), 위치는 직전 트랜스폼 갱신 값 — 카메라를 OnLateUpdate에서
//   옮긴다면 위치 계산도 OnLateUpdate에서 한다. 픽셀 아트 모드도 최종 출력 기준(CameraProjection.h). 활성 카메라나 레이아웃된 UI가 없으면
//   WorldToScreen은 (0, 0, false), ScreenToWorldRay는 nil
// bVisible = 카메라 앞(근·원평면 사이) && 화면 안

void FLuaRuntime::RegisterCameraBindings()
{
	// 좌표 기준 UI: 레이아웃이 끝난(뷰포트 크기가 있는) 인스턴스만
	const auto ResolveUI = [this](const sol::optional<FScriptEntity>& UIEntity) -> const FUIInstance* {
		if (Scene == nullptr)
		{
			throw std::runtime_error("씬이 없습니다 (플레이 중에만 Camera를 사용할 수 있습니다)");
		}
		FRegistry& Registry = Scene->GetRegistry();
		const auto Usable   = [](FUIComponent* Component) -> const FUIInstance* {
			const FUIInstance* Instance = Component != nullptr ? Component->Runtime.Instance.get() : nullptr;
			return Instance != nullptr && !Instance->GetViewport().IsEmpty() ? Instance : nullptr;
		};
		if (UIEntity)
		{
			if (!Registry.IsValid(UIEntity->Entity) || Registry.TryGet<FUIComponent>(UIEntity->Entity) == nullptr)
			{
				throw std::runtime_error("Camera: uiEntity에 UIComponent가 없습니다");
			}
			return Usable(Registry.TryGet<FUIComponent>(UIEntity->Entity));
		}
		for (FEntity Current = CurrentInstance; Current.IsValid() && Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			if (const FUIInstance* Instance = Usable(Registry.TryGet<FUIComponent>(Current)))
			{
				return Instance;
			}
		}
		const FUIInstance* First = nullptr;
		Registry.View<FUIComponent>().Each([&](FEntity, FUIComponent& Component) {
			if (First == nullptr && Component.bVisible)
			{
				First = Usable(&Component);
			}
		});
		return First;
	};
	// 활성 카메라 뷰-투영 (UI 뷰포트 종횡비)
	const auto ComputeViewProjection = [this](const FUIInstance& UI, FMatrix4x4& Out) {
		const FVector2 Size   = UI.GetViewport().GetSize();
		const FEntity  Camera = FCameraProjection::FindActiveCamera(*Scene);
		return Camera.IsValid() && FCameraProjection::ComputeViewProjection(*Scene, Camera, Size.X / Size.Y, Out);
	};

	sol::table CameraTable = Lua.create_named_table("Camera");
	CameraTable["WorldToScreen"] = [ResolveUI, ComputeViewProjection](const FVector3& WorldPosition, sol::optional<FScriptEntity> UIEntity) {
		const FUIInstance* UI = ResolveUI(UIEntity);
		FMatrix4x4         ViewProjection;
		if (UI == nullptr || !ComputeViewProjection(*UI, ViewProjection))
		{
			return std::make_tuple(0.0f, 0.0f, false);
		}
		FVector2       Pixel;
		const bool     bVisible = FCameraProjection::WorldToViewport(ViewProjection, WorldPosition, UI->GetViewport().GetSize(), Pixel);
		const FVector2 Layout   = UI->GetTransform().ToUi(Pixel + UI->GetViewport().Min);
		return std::make_tuple(Layout.X, Layout.Y, bVisible);
	};
	CameraTable["ScreenToWorldRay"] = [this, ResolveUI, ComputeViewProjection](float X, float Y, sol::optional<FScriptEntity> UIEntity) {
		const FUIInstance* UI = ResolveUI(UIEntity);
		FMatrix4x4         ViewProjection;
		FVector3           Origin;
		FVector3           Direction;
		if (UI == nullptr || !ComputeViewProjection(*UI, ViewProjection) ||
		    !FCameraProjection::ViewportToWorldRay(ViewProjection, UI->GetTransform().ToPixels(FVector2(X, Y)) - UI->GetViewport().Min,
		                                           UI->GetViewport().GetSize(), Origin, Direction))
		{
			return std::make_tuple(sol::object(sol::lua_nil), sol::object(sol::lua_nil));
		}
		return std::make_tuple(sol::make_object(Lua, Origin), sol::make_object(Lua, Direction));
	};
}
