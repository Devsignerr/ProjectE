-- 중력으로 떨어지고 바닥에서 튕기다가 수명이 다하면 스스로 파괴된다
local Faller = {
	Properties = {
		Gravity    = 980.0, -- cm/s²
		Bounce     = 0.55,  -- 반발 계수
		FloorZ     = 10.0,  -- 바닥 높이 (큐브 반 크기 포함, cm)
		Lifetime   = 3.0,   -- 초
		SpinSpeed  = 120.0, -- 도/초
	},
}

function Faller:OnStart()
	self.Velocity = Vector3(0, 0, 0)
	self.Age      = 0.0
end

function Faller:OnUpdate(dt)
	local P = self.Properties
	self.Age = self.Age + dt
	if self.Age >= P.Lifetime then
		self.entity:Destroy()
		return
	end

	self.Velocity.Z = self.Velocity.Z - P.Gravity * dt
	local Position  = self.entity:GetPosition() + self.Velocity * dt
	if Position.Z < P.FloorZ then
		Position.Z      = P.FloorZ
		self.Velocity.Z = -self.Velocity.Z * P.Bounce
	end
	self.entity:SetPosition(Position)
	self.entity:SetRotation(Quat.FromAxisAngle(Vector3(0, 0, 1), P.SpinSpeed * dt) * self.entity:GetRotation())
end

function Faller:OnDestroy()
	local Spawner = Scene.Find("Spawner")
	if Spawner ~= nil then
		local Script = Spawner:GetScript()
		if Script ~= nil and Script.OnChildDestroyed ~= nil then
			Script:OnChildDestroyed()
		end
	end
end

return Faller
