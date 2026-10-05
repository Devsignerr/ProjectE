-- HD-2D 데모 맵 이동 트리거 (맵 생성기가 엔티티에 BoxColliderComponent(IsTrigger) + 이 스크립트를 붙인다 — HD2DGameplay.AddTravel).
--   플레이어가 상자 안에 들어오면 관리자 TravelTo(TargetScene, SpawnName): 화면 페이드 → 상태를 세션 슬롯에 저장 → Game.OpenScene.
--   도착 씬의 관리자가 세션을 불러와 "Spawn_<SpawnName>" 엔티티(위치 = 발 자리) 위에 플레이어를 둔다 (HD2DParty.lua 머리 주석).
--   판정: 물리 트리거 알림(OnTriggerEnter) + 매 프레임 상자 안 검사(수평 회전 무시 — 축 정렬 상자로 쓴다). 씬 시작 1초 동안은 무시
--   (도착 자리가 트리거와 겹쳐도 바로 되돌아가지 않게 — 그래도 도착 자리는 트리거 밖에 둔다).
local HD2DTravel = {
	Properties = {
		TargetScene = "",  -- Content 기준 씬 경로 (예: Scenes/Demo/HD2DCave.escene)
		SpawnName   = "",  -- 도착 씬의 Spawn_<이름>
	},
}

function HD2DTravel:OnStart()
	self.Time = 0
	self.GM = Scene.Find("HD2DGame"):GetScript()
	local Box = self.entity:GetComponent("BoxColliderComponent")
	self.Half = Box and Box.HalfExtents or Vector3(150, 100, 150)
	self.Center = self.entity:GetWorldPosition()
	if self.Properties.TargetScene == "" then
		Log.Warn("[HD2D] 맵 이동 트리거에 TargetScene이 없음: " .. self.entity:GetName())
	end
end

function HD2DTravel:Go()
	if self.bGone or self.Time < 1.0 or self.Properties.TargetScene == "" or self.GM:IsMenuOpen() then return end
	self.bGone = true
	self.GM:TravelTo(self.Properties.TargetScene, self.Properties.SpawnName)
end

function HD2DTravel:OnTriggerEnter(Other)
	if Other and Other:GetName() == "Player" then self:Go() end
end

function HD2DTravel:OnUpdate(Dt)
	self.Time = self.Time + Time.GetUnscaledDelta()
	local Player = self.GM:GetPlayer()
	if not Player or self.bGone then return end
	local P = Player.entity:GetWorldPosition()
	local C, H = self.Center, self.Half
	if math.abs(P.X - C.X) <= H.X and math.abs(P.Y - C.Y) <= H.Y and math.abs(P.Z - C.Z) <= H.Z + 120 then
		self:Go()
	end
end

return HD2DTravel
