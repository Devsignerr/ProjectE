-- 일정 간격으로 작은 큐브를 만들어 떨어뜨린다 (엔티티 생성 + 컴포넌트 추가 + 스크립트 부착 예제)
local Spawner = {
	Properties = {
		Interval = 0.4,   -- 초
		Spread   = 150.0, -- 생성 위치 반경 (cm)
		MaxCount = 24,    -- 동시에 존재하는 최대 개수
	},
}

function Spawner:OnStart()
	self.Timer = 0.0
	self.Count = 0
end

function Spawner:OnUpdate(dt)
	self.Timer = self.Timer + dt
	if self.Timer < self.Properties.Interval then
		return
	end
	self.Timer = 0.0

	-- Faller가 파괴될 때 Count를 줄인다
	if self.Count >= self.Properties.MaxCount then
		return
	end
	self.Count = self.Count + 1

	local Origin = self.entity:GetPosition()
	local Spread = self.Properties.Spread
	local Cube   = Scene.Create("Falling Cube")
	Cube:SetPosition(Origin + Vector3((math.random() * 2 - 1) * Spread, (math.random() * 2 - 1) * Spread, 0))
	Cube:SetRotation(Quat.FromEuler(math.random() * 360, math.random() * 360, 0))
	Cube:SetScale(Vector3(0.2, 0.2, 0.2))

	local Mesh         = Cube:AddComponent("StaticMeshComponent")
	Mesh.MeshAsset     = "primitive:cube"
	Mesh.MaterialAsset = "Materials/Orange.emat"

	local Script       = Cube:AddComponent("ScriptComponent")
	Script.ScriptAsset = "Scripts/Faller.lua"
	Script.PropertyOverrides = '{"Lifetime": 4.0}'

	-- 생성된 큐브가 자기 스포너를 알 수 있게 이름으로 연결
	Cube:SetName("Falling Cube (" .. self.entity:GetName() .. ")")
end

-- Faller가 호출
function Spawner:OnChildDestroyed()
	self.Count = math.max(self.Count - 1, 0)
end

return Spawner
