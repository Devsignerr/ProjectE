-- 데모 맵 포털 (ServerOnly): 플레이어(PlayerName 엔티티)가 반경 안에 들어오면 TargetScene으로 이동한다 (Game.OpenScene — 이번 프레임 끝에 교체).
--   Ready = false인 포털은 아직 만들지 않은 서브맵 — 들어가면 안내만 남긴다.
--   서브맵에서 돌아오는 포털은 TargetScene = Hub. 돌아올 때 Hub의 같은 포털 앞에 서도록 Game.SetPersistent("DemoReturnPortal")를 남긴다
local DemoPortal = {
	Properties = {
		TargetScene = Asset("Scenes/Demo/Hub.escene", ".escene"),
		Label       = "",
		Ready       = true,
		Radius      = 140.0, -- cm (포털 중심에서 수평 거리)
		PlayerName  = "Player",
		RememberReturn = true, -- Hub 포털만 true (서브맵의 돌아가기 포털은 false)
	},
}

function DemoPortal:OnStart()
	self.Leaving = false
	self.WasInside = false
	-- Hub로 돌아온 경우: 마지막으로 들어간 포털 앞(포털 정면 3m)에 플레이어를 세운다
	if Game.GetPersistent("DemoReturnPortal", "") == self.entity:GetName() then
		Game.SetPersistent("DemoReturnPortal", "")
		local Player = Scene.Find(self.Properties.PlayerName)
		if Player then
			local Front = self.entity:GetForward()
			Player:SetPosition(self.entity:GetWorldPosition() + Front * 300 + Vector3(0, 0, 120))
		end
	end
end

function DemoPortal:IsInside(Entity)
	if Entity == nil or not Entity:IsValid() then return false end
	local Offset = Entity:GetWorldPosition() - self.entity:GetWorldPosition()
	Offset.Z = 0
	return Offset:Length() < self.Properties.Radius
end

function DemoPortal:OnUpdate(dt)
	if self.Leaving then return end
	local Inside = self:IsInside(Scene.Find(self.Properties.PlayerName))
	local Entered = Inside and not self.WasInside
	self.WasInside = Inside
	if not Entered then return end

	if not self.Properties.Ready then
		Log.Info("포털 '" .. self.Properties.Label .. "' — 서브맵 준비 중")
		return
	end
	self.Leaving = true
	if self.Properties.RememberReturn then
		Game.SetPersistent("DemoReturnPortal", self.entity:GetName())
	end
	Log.Info("포털 →", self.Properties.Label, self.Properties.TargetScene.Path)
	Game.OpenScene(self.Properties.TargetScene)
end

return DemoPortal
