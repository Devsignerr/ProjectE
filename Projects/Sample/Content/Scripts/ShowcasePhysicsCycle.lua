-- 쇼케이스 물리 구역: 상자 피라미드를 쌓고(Scene.Create + 콜라이더/강체) LaunchDelay초 뒤 프리팹 공(Scene.SpawnPrefab)을 쏴서 무너뜨린다.
--   Period초마다 치우고 다시 쌓으므로 투어 카메라가 언제 와도 움직임이 보인다. 엔티티 앞(+X)이 발사 방향
--   바디는 만든 다음 물리 갱신에 생기므로 공 속도는 생성 콜백 다음 프레임에 준다
local ShowcasePhysicsCycle = {
	Properties = {
		Rows         = 4,       -- 피라미드 바닥 줄 상자 수
		CrateSize    = 50.0,    -- cm
		Period       = 10.0,    -- 초 (다시 쌓는 주기)
		LaunchDelay  = 2.5,     -- 초 (쌓은 뒤 발사까지)
		LaunchOffset = 650.0,   -- cm (피라미드 뒤쪽 발사 위치까지 거리)
		LaunchHeight = 60.0,    -- cm
		LaunchSpeed  = 1300.0,  -- cm/s
		Ball         = Prefab("Prefabs/PhysicsBall.eprefab"),
	},
}

local CrateMaterials = { "Showcase/Wood.emat", "Materials/Orange.emat" }

function ShowcasePhysicsCycle:OnStart()
	self.Time    = 0.0
	self.Crates  = {}
	self.Balls   = {}
	self.Launched = false
	self:Build()
end

function ShowcasePhysicsCycle:Clear()
	for _, Entity in ipairs(self.Crates) do
		if Entity:IsValid() then Entity:Destroy() end
	end
	for _, Entity in ipairs(self.Balls) do
		if Entity:IsValid() then Entity:Destroy() end
	end
	self.Crates = {}
	self.Balls  = {}
end

-- 엔티티 위치에 Y축으로 늘어선 피라미드 (발사 방향 = +X 이므로 공이 정면으로 친다)
function ShowcasePhysicsCycle:Build()
	local P      = self.Properties
	local Origin = self.entity:GetPosition()
	local Right  = self.entity:GetRight()
	local Size   = P.CrateSize
	local Count  = 0
	for Row = 0, P.Rows - 1 do
		local InRow = P.Rows - Row
		for Index = 0, InRow - 1 do
			local Offset = (Index - (InRow - 1) * 0.5) * (Size + 1.0)
			local Crate  = Scene.Create("Crate")
			Crate:SetPosition(Origin + Right * Offset + Vector3(0, 0, Size * 0.5 + Row * (Size + 0.5) + 1.0))
			Crate:SetRotation(self.entity:GetRotation())
			Crate:SetScale(Vector3(Size / 100.0, Size / 100.0, Size / 100.0))
			local Mesh         = Crate:AddComponent("StaticMeshComponent")
			Mesh.MeshAsset     = "primitive:cube"
			Mesh.MaterialAsset = CrateMaterials[(Count % #CrateMaterials) + 1]
			Crate:AddComponent("BoxColliderComponent") -- 반 크기 50 × 스케일
			local Body    = Crate:AddComponent("RigidBodyComponent")
			Body.Density  = 120.0
			Body.Friction = 0.6
			table.insert(self.Crates, Crate)
			Count = Count + 1
		end
	end
	self.Time     = 0.0
	self.Launched = false
end

function ShowcasePhysicsCycle:Launch()
	local P     = self.Properties
	local From  = self.entity:GetPosition() - self.entity:GetForward() * P.LaunchOffset + Vector3(0, 0, P.LaunchHeight)
	Scene.SpawnPrefab(P.Ball, From, function(Root)
		table.insert(self.Balls, Root)
		self.PendingBall = Root
		self.PendingFrames = 2 -- 생성 → 바디 생성(물리 갱신) → 속도
	end)
end

function ShowcasePhysicsCycle:OnUpdate(dt)
	local P = self.Properties
	self.Time = self.Time + dt
	if self.PendingBall then
		self.PendingFrames = self.PendingFrames - 1
		if self.PendingFrames <= 0 then
			if self.PendingBall:IsValid() then
				self.PendingBall:SetVelocity(self.entity:GetForward() * P.LaunchSpeed + Vector3(0, 0, 120))
			end
			self.PendingBall = nil
		end
	end
	if not self.Launched and self.Time >= P.LaunchDelay then
		self.Launched = true
		self:Launch()
	end
	if self.Time >= P.Period then
		self:Clear()
		self:Build()
	end
end

function ShowcasePhysicsCycle:OnDestroy()
	self:Clear()
end

return ShowcasePhysicsCycle
