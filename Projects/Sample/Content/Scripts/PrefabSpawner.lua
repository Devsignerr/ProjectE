-- 프리팹을 일정 간격으로 만들어 떨어뜨린다 (Properties의 Prefab 값 + Scene.SpawnPrefab 예제)
local PrefabSpawner = {
	Properties = {
		Prefab   = Prefab("Prefabs/Ball.eprefab"), -- 인스펙터에서 다른 .eprefab을 끌어 놓아 바꿀 수 있다
		Interval = 0.5,   -- 초
		Spread   = 120.0, -- 생성 위치 반경 (cm)
		MaxCount = 12,    -- 동시에 존재하는 최대 개수
	},
}

function PrefabSpawner:OnStart()
	self.Timer   = 0.0
	self.Spawned = {}
end

function PrefabSpawner:OnUpdate(dt)
	self.Timer = self.Timer + dt
	if self.Timer < self.Properties.Interval then
		return
	end
	self.Timer = 0.0

	-- 수명이 끝나 스스로 파괴된 것은 목록에서 뺀다
	local Alive = {}
	for _, Entity in ipairs(self.Spawned) do
		if Entity:IsValid() then
			table.insert(Alive, Entity)
		end
	end
	self.Spawned = Alive
	if #self.Spawned >= self.Properties.MaxCount then
		return
	end

	local Spread   = self.Properties.Spread
	local Position = self.entity:GetPosition() + Vector3((math.random() * 2 - 1) * Spread, (math.random() * 2 - 1) * Spread, 0)
	-- 생성은 이번 프레임 스크립트 갱신이 끝난 뒤에 된다. 만든 루트 엔티티는 콜백으로 받는다
	Scene.SpawnPrefab(self.Properties.Prefab, Position, function(Root)
		Root:SetName("Spawned " .. Root:GetName())
		table.insert(self.Spawned, Root)
	end)
end

return PrefabSpawner
