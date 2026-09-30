-- 일정 간격으로 공을 만들어 +X(엔티티 앞) 방향으로 발사한다 (물리: 콜라이더/강체 추가 + 충격량 + 레이캐스트 예제)
local Launcher = {
	Properties = {
		Interval = 1.2,    -- 초
		Speed    = 1500.0, -- 발사 속도 (cm/s)
		Lift     = 0.08,   -- 위쪽 성분 비율
		MaxCount = 8,      -- 동시에 존재하는 최대 공 수 (넘으면 가장 오래된 공 제거)
	},
}

function Launcher:OnStart()
	self.Timer = 0.0
	self.Balls = {}
end

-- 아래쪽 바닥까지 거리 (레이캐스트 예제). 바디는 첫 물리 갱신 뒤에 생기므로 OnStart가 아니라 첫 발사 때 쏜다
function Launcher:LogGroundDistance()
	local Hit = Physics.Raycast(self.entity:GetPosition(), Vector3(0, 0, -1), 10000)
	if Hit then
		Log.Info("발사대 아래", Hit.entity and Hit.entity:GetName() or "?", "까지", string.format("%.1f", Hit.distance), "cm")
	end
end

function Launcher:OnUpdate(dt)
	-- 지난 프레임에 만든 공은 이제 바디가 있으므로 발사
	if self.Pending then
		local Mass      = self.Pending:GetMass() -- 실제 바디 질량 (Mass가 0이면 밀도로 계산된 값)
		local Direction = self.entity:GetForward() + Vector3(0, 0, self.Properties.Lift)
		self.Pending:AddImpulse(Direction * (self.Properties.Speed * Mass))
		self.Pending = nil
	end

	self.Timer = self.Timer + dt
	if self.Timer < self.Properties.Interval then
		return
	end
	self.Timer = 0.0
	if not self.bLogged then
		self.bLogged = true
		self:LogGroundDistance()
	end

	if #self.Balls >= self.Properties.MaxCount then
		local Oldest = table.remove(self.Balls, 1)
		if Oldest:IsValid() then
			Oldest:Destroy()
		end
	end

	local Ball = Scene.Create("Launched Ball")
	Ball:SetPosition(self.entity:GetPosition())
	Ball:SetScale(Vector3(0.4, 0.4, 0.4)) -- 지름 40cm

	local Mesh         = Ball:AddComponent("StaticMeshComponent")
	Mesh.MeshAsset     = "primitive:sphere"
	Mesh.MaterialAsset = "Materials/Orange.emat"

	Ball:AddComponent("SphereColliderComponent") -- 반지름 50cm × 스케일
	local Body       = Ball:AddComponent("RigidBodyComponent")
	Body.Mass        = 5.0 -- 지름 40cm 공 (상자는 밀도 기반 약 26kg)
	Body.Restitution = 0.3

	table.insert(self.Balls, Ball)
	self.Pending = Ball
end

return Launcher
