-- 대상 주위를 천천히 도는 카메라 (카메라 컴포넌트가 있는 엔티티에 붙인다)
local OrbitCamera = {
	Properties = {
		Target     = Vector3(0, 0, 80), -- 바라볼 지점 (cm)
		Radius     = 900.0,             -- cm
		Height     = 350.0,             -- 대상 기준 높이 (cm)
		Speed      = 8.0,               -- 도/초
		StartAngle = -145.0,            -- 도 (0 = 대상의 +X 쪽)
	},
}

function OrbitCamera:OnStart()
	self.Angle = self.Properties.StartAngle
	self:Place()
end

function OrbitCamera:OnUpdate(dt)
	self.Angle = (self.Angle + self.Properties.Speed * dt) % 360
	self:Place()
end

function OrbitCamera:Place()
	local P        = self.Properties
	local Radians  = math.rad(self.Angle)
	local Position = P.Target + Vector3(math.cos(Radians) * P.Radius, math.sin(Radians) * P.Radius, P.Height)
	self.entity:SetPosition(Position)
	self.entity:SetRotation(Quat.LookRotation(P.Target - Position))
end

return OrbitCamera
