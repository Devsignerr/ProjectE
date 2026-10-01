-- 맵 전환 포털 (Demo_Travel_A/B, ServerOnly): 플레이어 폰이나 TravelerName 엔티티가 반경 안에 들어오면 TargetScene으로 간다.
--   Game.OpenScene은 서버/Standalone에서만 동작하고 이번 프레임 끝에 씬을 바꾼다 — 접속한 클라이언트도 따라오고 폰은 새 맵에서 다시 생긴다.
--   Game.SetPersistent 값은 맵을 바꿔도(새 Lua 상태) 남는다 → 몇 번째 이동인지 센다
local TravelPortal = {
	Properties = {
		TargetScene  = Asset("Scenes/Demo_Travel_B.escene", ".escene"),
		Radius       = 170.0, -- cm (포털 중심에서 수평 거리)
		TravelerName = "Traveler",
	},
}

function TravelPortal:OnStart()
	self.Pawns   = {}
	self.Leaving = false
	Log.Info("맵:", Game.GetCurrentScene(), "— 포털 이동 횟수", Game.GetPersistent("PortalTrips", 0))
end

-- 멀티플레이: 서버가 이 맵에 들어온 플레이어 폰을 알려 준다 (맵을 옮기면 새 맵에서 다시 불린다)
function TravelPortal:OnPlayerJoined(id, pawn) self.Pawns[id] = pawn end
function TravelPortal:OnPlayerLeft(id) self.Pawns[id] = nil end

function TravelPortal:IsInside(Entity)
	if Entity == nil or not Entity:IsValid() then return false end
	local Offset = Entity:GetWorldPosition() - self.entity:GetWorldPosition()
	Offset.Z = 0
	return Offset:Length() < self.Properties.Radius
end

function TravelPortal:OnUpdate(dt)
	if self.Leaving then return end
	local Who = nil
	if self:IsInside(Scene.Find(self.Properties.TravelerName)) then Who = self.Properties.TravelerName end
	for _, Pawn in pairs(self.Pawns) do
		if self:IsInside(Pawn) then Who = Pawn:GetName() end
	end
	if Who == nil then return end

	self.Leaving = true
	local Trips = Game.GetPersistent("PortalTrips", 0) + 1
	Game.SetPersistent("PortalTrips", Trips)
	Log.Info(Who, "포털 통과 →", self.Properties.TargetScene.Path, "(", Trips, "번째 이동)")
	Game.OpenScene(self.Properties.TargetScene)
end

return TravelPortal
