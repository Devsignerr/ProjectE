-- 훈련장 경비병 (Training 데모, ServerOnly): 이동·판단은 비헤이비어 트리(AI/Demo/TrainingGuard.ebt)가 하고, 이 스크립트는
--   ① 순찰 지점 목록(Waypoints "x,y,z; ...") — 트리의 Lua 태스크 GuardNextWaypoint.lua가 NextWaypoint()로 받아 블랙보드 PatrolPoint에 넣는다
--   ② 감각 — 트리의 Lua 서비스 GuardSenses.lua가 매 틱 UpdateSenses()를 불러 블랙보드 Target(플레이어)을 넣고 뺀다
--   ③ 표시 — 위치 변화로 잰 속도를 애니메이션 그래프 Speed에, 플레이어를 보면 시선 IK(LookAtComponent)를 플레이어 머리로
local TrainingGuard = {
	Properties = {
		Waypoints   = "",
		SightRange  = 850.0,  -- cm, 이 안 + 시야각 안 + 가리는 것 없음 → 발견
		LoseRange   = 1300.0, -- 이보다 멀어지면 놓친다
		NoticeRange = 300.0,  -- 이 안이면 등 뒤여도 알아챈다
		FovDegrees  = 140.0,
		WalkSpeed   = 150.0,
		ChaseSpeed  = 250.0,
		PlayerName  = "Player",
	},
}

local function ParseWaypoints(Text)
	local Points = {}
	for Item in string.gmatch(Text or "", "[^;]+") do
		local Values = {}
		for Number in string.gmatch(Item, "[%-%d%.]+") do
			table.insert(Values, tonumber(Number))
		end
		if #Values >= 2 then
			table.insert(Points, Vector3(Values[1], Values[2], Values[3] or 0.0))
		end
	end
	return Points
end

function TrainingGuard:OnStart()
	self.Points = ParseWaypoints(self.Properties.Waypoints)
	self.NextIndex = 1
	self.Agent = self.entity:GetComponent("NavAgentComponent")
	self.Mesh = self.entity:FindChild("Mesh")
	self.PrevPos = self.entity:GetWorldPosition()
	self.Speed = 0.0
	self.Target = nil
	self.TargetDirty = false
end

function TrainingGuard:NextWaypoint()
	if #self.Points == 0 then return nil end
	local Point = self.Points[self.NextIndex]
	self.NextIndex = self.NextIndex % #self.Points + 1
	return Point
end

function TrainingGuard:CanSee(Player, Distance)
	local Eye = self.entity:GetWorldPosition() + Vector3(0, 0, 160)
	local Chest = Player:GetWorldPosition() + Vector3(0, 0, 40) -- 플레이어 루트 = 캡슐 중심
	local Dir = Chest - Eye
	local Len = Dir:Length()
	if Len < 1.0 then return true end
	Dir = Dir * (1.0 / Len)
	if Distance > self.Properties.NoticeRange then
		local Forward = self.entity:GetForward()
		local FlatLen = math.sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y)
		if FlatLen > 0.01 then
			local Cos = (Forward.X * Dir.X + Forward.Y * Dir.Y) / FlatLen
			if Cos < math.cos(math.rad(self.Properties.FovDegrees * 0.5)) then return false end
		end
	end
	-- 벽·성벽이 가리는지 (자기 캡슐 밖에서 시작)
	local Start = Eye + Dir * 45.0
	local Hit = Physics.Raycast(Start, Dir, Len - 45.0)
	return Hit == nil or Hit.distance > Len - 110.0
end

-- GuardSenses.lua(Lua 서비스)가 매 틱 부른다. 반환 = 지금 쫓는 대상(엔티티) 또는 nil
function TrainingGuard:UpdateSenses()
	local Player = Scene.Find(self.Properties.PlayerName)
	local Previous = self.Target
	if Player == nil or not Player:IsValid() then
		self.Target = nil
	else
		local Delta = Player:GetWorldPosition() - self.entity:GetWorldPosition()
		local Distance = math.sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y)
		if math.abs(Delta.Z) > 400.0 then
			self.Target = nil -- 성벽 위/아래는 따라가지 않는다
		elseif self.Target then
			if Distance > self.Properties.LoseRange then self.Target = nil end
		elseif Distance < self.Properties.SightRange and self:CanSee(Player, Distance) then
			self.Target = Player
			Log.Info("[Training]", self.entity:GetName(), "플레이어 발견 — 따라갑니다")
		end
	end
	if (Previous == nil) ~= (self.Target == nil) then
		self.TargetDirty = true
		if self.Agent then
			self.Agent.MaxSpeed = self.Target and self.Properties.ChaseSpeed or self.Properties.WalkSpeed
		end
	end
	return self.Target
end

function TrainingGuard:OnUpdate(dt)
	-- 위치 변화 → 속도 (AI가 트랜스폼을 직접 옮긴다) → 애니메이션 그래프 Speed
	local Pos = self.entity:GetWorldPosition()
	if dt > 0.0001 then
		local DX, DY = Pos.X - self.PrevPos.X, Pos.Y - self.PrevPos.Y
		local Raw = math.sqrt(DX * DX + DY * DY) / dt
		if Raw > 2000.0 then Raw = 0.0 end
		local Alpha = 1.0 - math.exp(-dt / 0.15)
		self.Speed = self.Speed + (Raw - self.Speed) * Alpha
	end
	self.PrevPos = Pos
	self.entity:SetAnimParam("Speed", self.Speed)
	if self.Mesh then
		if self.Target and self.Target:IsValid() then
			self.Mesh:SetLookAtTarget(self.Target:GetWorldPosition() + Vector3(0, 0, 65))
		else
			self.Mesh:ClearLookAtTarget()
		end
	end
end

return TrainingGuard
