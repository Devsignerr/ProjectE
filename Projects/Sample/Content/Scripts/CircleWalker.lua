-- 시작 위치를 중심으로 수평 원을 그리며 걷고, 진행 방향을 바라본다 (픽셀 아트 데모의 걷는 여우 등)
local CircleWalker = {
	Properties = {
		Radius     = 120.0, -- cm
		Speed      = 30.0,  -- 도/초 (음수면 시계 방향)
		StartAngle = 0.0,   -- 도
		FacingYaw  = 0.0,   -- 모델 앞 방향 보정 (도). Fox.glb는 -X를 바라봄
	},
}

function CircleWalker:OnStart()
	self.Center = self.entity:GetPosition()
	self.Angle  = self.Properties.StartAngle
end

function CircleWalker:OnUpdate(dt)
	local P    = self.Properties
	self.Angle = self.Angle + P.Speed * dt
	local A    = math.rad(self.Angle)
	self.entity:SetPosition(self.Center + Vector3(math.cos(A) * P.Radius, math.sin(A) * P.Radius, 0))
	-- 진행 방향(접선) = 반지름 방향에서 ±90도
	local Heading = self.Angle + (P.Speed >= 0 and 90.0 or -90.0)
	self.entity:SetRotation(Quat.FromAxisAngle(Vector3(0, 0, 1), Heading + P.FacingYaw))
end

return CircleWalker
