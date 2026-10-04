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
//
// 마우스 커서 (Input 테이블 — 같은 좌표 공간이라 여기서 등록)
//   local X, Y, bInside = Input.GetMouseUIPosition([uiEntity])  -- 게임 UI 레이아웃 좌표 (위와 같은 UI 고르기·배율·뷰포트 — 에디터 플레이 뷰포트도 맞음).
//       입력 모드(GameOnly 포함)·커서 잠금과 무관하게 OS 커서 위치 (잠금 중에는 가둔 점에 머문다 — 조준은 커서를 숨기고(Game.SetCursorVisible(false))
//       잠그지 않는 쪽이 맞다). bInside = 커서가 게임 화면 안. 레이아웃된 UI가 없으면 (0, 0, false). 로컬 화면 값 — 서버의 원격 플레이어에는 없다
//   Input.IsMouseOverUI()  -- 이번 프레임 게임 UI가 포인터를 가져갔는가 (보이는 위젯 위 — 그 프레임 게임 입력에서 마우스 버튼이 빠진다).
//       UI가 포인터를 받지 않는 입력 모드(GameOnly)에서는 항상 false
//   (창 클라이언트 픽셀은 Input.GetMousePosition() — LuaRuntime.cpp)

void FLuaRuntime::RegisterCameraBindings()
{
	// 좌표 기준 UI: 레이아웃이 끝난(뷰포트 크기가 있는) 인스턴스만
	const auto ResolveUIComponent = [this](const sol::optional<FScriptEntity>& UIEntity) -> const FUIComponent* {
		if (Scene == nullptr)
		{
			throw std::runtime_error("씬이 없습니다 (플레이 중에만 Camera를 사용할 수 있습니다)");
		}
		FRegistry& Registry = Scene->GetRegistry();
		const auto Usable   = [](FUIComponent* Component) -> const FUIComponent* {
			const FUIInstance* Instance = Component != nullptr ? Component->Runtime.Instance.get() : nullptr;
			return Instance != nullptr && !Instance->GetViewport().IsEmpty() ? Component : nullptr;
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
			if (const FUIComponent* Found = Usable(Registry.TryGet<FUIComponent>(Current)))
			{
				return Found;
			}
		}
		const FUIComponent* First = nullptr;
		Registry.View<FUIComponent>().Each([&](FEntity, FUIComponent& Component) {
			if (First == nullptr && Component.bVisible)
			{
				First = Usable(&Component);
			}
		});
		return First;
	};
	const auto ResolveUI = [ResolveUIComponent](const sol::optional<FScriptEntity>& UIEntity) -> const FUIInstance* {
		const FUIComponent* Component = ResolveUIComponent(UIEntity);
		return Component != nullptr ? Component->Runtime.Instance.get() : nullptr;
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

	sol::table InputTable = Lua["Input"];
	InputTable["GetMouseUIPosition"] = [ResolveUIComponent](sol::optional<FScriptEntity> UIEntity) {
		const FUIComponent* Component = ResolveUIComponent(UIEntity);
		if (Component == nullptr)
		{
			return std::make_tuple(0.0f, 0.0f, false);
		}
		const FVector2 Layout = Component->Runtime.Instance->GetTransform().ToUi(Component->Runtime.CursorPixels);
		return std::make_tuple(Layout.X, Layout.Y, Component->Runtime.bCursorInside);
	};
	InputTable["IsMouseOverUI"] = [this]() {
		if (Scene == nullptr)
		{
			return false;
		}
		bool bOver = false;
		Scene->GetRegistry().View<FUIComponent>().Each([&bOver](FEntity, FUIComponent& Component) { bOver = bOver || Component.Runtime.bPointerOver; });
		return bOver;
	};
}
