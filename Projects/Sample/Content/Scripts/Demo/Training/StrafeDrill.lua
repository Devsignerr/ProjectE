-- 방향 이동 훈련 (Training 데모, ServerOnly): 훈련 기둥을 바라본 채 원을 따라 옆걸음 → 다가서기 → 물러서기 → 반대 옆걸음 → 대각선.
--   몸 기준 속도(MoveX = 오른쪽, MoveY = 앞, cm/s)를 2D 블렌드 스페이스(Animations/Demo/Training/StrafeDrill.eanimgraph) 파라미터로 넣는다.
--   이동은 이 스크립트가 루트 트랜스폼을 직접 옮긴다 (키네마틱 캡슐). 속도는 부드럽게 바뀌고 애니메이션과 실제 이동이 같은 값을 쓴다
local StrafeDrill = {
	Properties = {
		CenterX = 0.0,
		CenterY = 0.0,
		Radius = 280.0,
		StrafeSpeed = 300.0,
		StepSpeed = 140.0,
	},
}

-- 단계: {오른쪽 속도, 앞 속도, 시간}
local function Program(P)
	return {
		{ P.StrafeSpeed, 0.0, 3.2 },
		{ 0.0, 0.0, 1.0 },
		{ 0.0, P.StepSpeed, 1.0 },
		{ 0.0, -P.StepSpeed, 1.0 },
		{ -P.StrafeSpeed, 0.0, 3.2 },
		{ 0.0, 0.0, 1.0 },
		{ P.StrafeSpeed * 0.5, P.StepSpeed * 0.6, 1.4 }, -- 대각선 (2D 블렌드 사이 값)
		{ -P.StrafeSpeed * 0.5, -P.StepSpeed * 0.6, 1.4 },
		{ 0.0, 0.0, 0.8 },
	}
end

function StrafeDrill:OnStart()
	self.Steps = Program(self.Properties)
	self.Step = 1
	self.Time = 0.0
	self.VX, self.VY = 0.0, 0.0
	self.Center = Vector3(self.Properties.CenterX, self.Properties.CenterY, 0.0)
end

function StrafeDrill:OnUpdate(dt)
	local Step = self.Steps[self.Step]
	self.Time = self.Time + dt
	if self.Time >= Step[3] then
		self.Time = 0.0
		self.Step = self.Step % #self.Steps + 1
		Step = self.Steps[self.Step]
	end
	-- 목표 속도로 부드럽게 (가속 약 0.25초)
	local Alpha = 1.0 - math.exp(-dt / 0.25)
	self.VX = self.VX + (Step[1] - self.VX) * Alpha
	self.VY = self.VY + (Step[2] - self.VY) * Alpha

	local Pos = self.entity:GetWorldPosition()
	local ToCenter = Vector3(self.Center.X - Pos.X, self.Center.Y - Pos.Y, 0.0)
	local Dist = math.max(ToCenter:Length(), 1.0)
	local Forward = ToCenter * (1.0 / Dist)
	local Right = Vector3(-Forward.Y, Forward.X, 0.0)
	local NewPos = Pos + Right * (self.VX * dt)
	-- 옆걸음은 원을 따라간다 (반지름은 앞뒤 걸음만 바꾼다, 범위 제한)
	local Offset = Vector3(NewPos.X - self.Center.X, NewPos.Y - self.Center.Y, 0.0)
	local Len = math.max(Offset:Length(), 1.0)
	local Radius = math.max(self.Properties.Radius * 0.45, math.min(self.Properties.Radius, Dist - self.VY * dt))
	NewPos = Vector3(self.Center.X + Offset.X / Len * Radius, self.Center.Y + Offset.Y / Len * Radius, Pos.Z)
	self.entity:SetPosition(NewPos)
	local Face = Vector3(self.Center.X - NewPos.X, self.Center.Y - NewPos.Y, 0.0)
	self.entity:SetRotation(Quat.FromEuler(0.0, math.deg(math.atan(Face.Y, Face.X)), 0.0))
	self.entity:SetAnimParam("MoveX", self.VX)
	self.entity:SetAnimParam("MoveY", self.VY)
end

return StrafeDrill
