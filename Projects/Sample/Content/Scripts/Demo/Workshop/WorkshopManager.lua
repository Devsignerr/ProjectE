-- 데모 서브맵 Workshop (물리 창고) 관리 (ServerOnly): 안내 + "밀어내기 사격".
--   E 키(누르고 있으면 Interval 간격 연사, 또는 마우스가 잠긴 동안 왼쪽 클릭) = 카메라(PlayerCamera — DemoPlayer.lua가 만듦)가 보는 방향으로 레이캐스트해
--   맞은 동적 바디에 충격량(질량 × ShotSpeed, 무거운 것은 MaxShotMass까지만)을 준다. 마네킹을 맞히면 래그돌로 쓰러진다
local WorkshopManager = {
	Properties = {
		ShotSpeed   = 650.0,  -- cm/s
		MaxShotMass = 80.0,   -- kg
		MaxDistance = 4000.0, -- cm
		Interval    = 0.35,   -- 초, E를 누르고 있으면 이 간격으로 연사
	},
}

function WorkshopManager:OnStart()
	self.WasMouseDown = false
	self.Cooldown = 0.0
	Log.Info("창고: 상자 더미를 밀어 보고, E(또는 마우스 잠금 중 왼쪽 클릭)로 보는 물체를 밀어낸다")
end

function WorkshopManager:OnUpdate(dt)
	local MouseDown = Input.IsMouseDown("Left")
	local Clicked = MouseDown and not self.WasMouseDown and Game.IsMouseLocked()
	self.WasMouseDown = MouseDown
	self.Cooldown = self.Cooldown - dt
	if Clicked or (Input.IsKeyDown("E") and self.Cooldown <= 0.0) then
		if self:Shoot() then
			self.Cooldown = self.Properties.Interval
		end
	end
end

function WorkshopManager:Shoot()
	local Camera = Scene.Find("PlayerCamera")
	if Camera == nil then return false end
	local Dir = Camera:GetForward()
	local Origin = Camera:GetWorldPosition()
	local Hit = nil
	-- 3인칭이면 카메라와 물체 사이에 플레이어 캡슐이 있으므로 건너뛴다
	for _ = 1, 3 do
		Hit = Physics.Raycast(Origin, Dir, self.Properties.MaxDistance)
		if Hit == nil or Hit.entity == nil or Hit.entity:GetName() ~= "Player" then break end
		Origin = Hit.position + Dir * 100.0 -- 캡슐 안에서 시작하면 같은 캡슐을 다시 맞히므로 지름보다 멀리 건너뛴다
	end
	if Hit == nil or Hit.entity == nil then return true end
	Debug.DrawLine(Camera:GetWorldPosition() + Dir * 30.0, Hit.position, Vector3(1.0, 0.75, 0.2), 0.25)
	local Target = Hit.entity
	local Script = Target:GetScript()
	if Script and Script.Collapse then
		Script:Collapse("사격")
		return true
	end
	local Mass = Target:GetMass()
	if Mass > 0 then
		Target:AddImpulse(Dir * (math.min(Mass, self.Properties.MaxShotMass) * self.Properties.ShotSpeed))
		Log.Info(string.format("밀어냄: %s (%.0f kg, %.0f cm)", Target:GetName(), Mass, Hit.distance))
	end
	return true
end

return WorkshopManager
