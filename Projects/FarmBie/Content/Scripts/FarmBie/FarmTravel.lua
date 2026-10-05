-- FarmBie 맵 이동 트리거 (생성기 AddTravel — BoxColliderComponent(IsTrigger) + 이 스크립트).
--   플레이어가 상자 안에 들어오면 관리자 TravelTo(TargetScene, SpawnName): 화면 어둡게 → 상태를 세션 슬롯에 저장 → Game.OpenScene.
--   도착 씬의 관리자가 세션을 불러와 "Spawn_<SpawnName>" 엔티티(발 자리)에 플레이어를 둔다 (FarmGame.lua TryResumeSession).
--   판정: 물리 트리거 알림 + 매 프레임 상자 안 검사(축 정렬). 씬 시작 1초 동안은 무시 (도착 자리가 겹쳐도 바로 되돌아가지 않게)
local FarmTravel = {
	Properties = {
		TargetScene = "", -- Content 기준 씬 경로
		SpawnName   = "", -- 도착 씬의 Spawn_<이름>
	},
}

function FarmTravel:OnStart()
	self.Time = 0
	self.GM = Scene.Find("FarmGame"):GetScript()
	local Box = self.entity:GetComponent("BoxColliderComponent")
	self.Half = Box and Box.HalfExtents or Vector3(60, 230, 150)
	self.Center = self.entity:GetWorldPosition()
end

function FarmTravel:Go()
	if self.bGone or self.Time < 1.0 or self.Properties.TargetScene == "" or self.GM:IsMenuOpen() or self.GM.Phase == "Sleep" then return end
	self.bGone = true
	self.GM:TravelTo(self.Properties.TargetScene, self.Properties.SpawnName)
end

function FarmTravel:OnTriggerEnter(Other)
	if Other and Other:GetName() == "Player" then self:Go() end
end

function FarmTravel:OnUpdate(Dt)
	self.Time = self.Time + Time.GetUnscaledDelta()
	local P = self.GM:Player()
	if not P or self.bGone then return end
	local Pos = P.entity:GetWorldPosition()
	local C, H = self.Center, self.Half
	if math.abs(Pos.X - C.X) <= H.X and math.abs(Pos.Y - C.Y) <= H.Y and math.abs(Pos.Z - C.Z) <= H.Z + 120 then
		self:Go()
	end
end

return FarmTravel
