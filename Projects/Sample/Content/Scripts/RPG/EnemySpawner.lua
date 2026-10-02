-- 적 스포너 (Phase 45 트랙 C): 엔티티 위치 주변에 EnemyPrefab을 MaxAlive마리 유지한다.
--   죽은(IsDead) 적은 바로 빈자리로 치고 RespawnDelay초 뒤 새로 만든다 (시체는 적 스크립트가 스스로 치운다).
--   ActivationRange > 0이면 플레이어가 그 거리 안에 처음 들어올 때부터 만든다 (멀리 있는 스포너는 비용 없음).
--   생성 위치는 내비메시 위 임의 지점 (AI.FindPath 끝점), 적의 "집"은 생성 위치 — 순찰/귀환 기준이 된다.
local EnemySpawner = {
	Properties = {
		EnemyPrefab     = Prefab("Prefabs/RPG/Enemy_SkeletonMinion.eprefab"),
		MaxAlive        = 3,
		SpawnRadius     = 250.0, -- cm
		RespawnDelay    = 8.0,   -- 초 (한 마리 죽은 뒤 다음 생성까지)
		InitialDelay    = 0.0,   -- 첫 생성까지 (초)
		SpawnInterval   = 0.6,   -- 여러 마리를 만들 때 사이 간격 (초)
		ActivationRange = 0.0,   -- cm, 0 = 처음부터
		TargetName      = "Player",
	},
}

function EnemySpawner:OnStart()
	self.Alive     = {}          -- 살아 있는 적 루트
	self.Pending   = 0           -- 생성 요청 후 콜백 대기 수
	self.NextSpawn = self.Properties.InitialDelay
	self.Respawns  = {}          -- 예약된 생성 시각 (스포너 시간)
	self.Time      = 0
	self.bActive   = self.Properties.ActivationRange <= 0
	self.Player    = Scene.Find(self.Properties.TargetName)
	self.Center    = self.entity:GetWorldPosition()
end

local function IsEnemyDead(Entity)
	if not Entity:IsValid() then return true end
	local Script = Entity:GetScript()
	if Script ~= nil and type(Script.IsDead) == "function" then
		return Script:IsDead()
	end
	return Entity:IsDead()
end

function EnemySpawner:OnUpdate(dt)
	local P = self.Properties
	self.Time = self.Time + dt
	if not self.bActive then
		if self.Player == nil or not self.Player:IsValid() then
			self.Player = Scene.Find(P.TargetName)
		end
		if self.Player ~= nil and (self.Player:GetWorldPosition() - self.Center):Length() <= P.ActivationRange then
			self.bActive   = true
			self.NextSpawn = self.Time + P.InitialDelay
			Log.Info("스포너: 활성화 " .. self.entity:GetName())
		end
		return
	end

	-- 죽은 적은 빼고 리스폰 예약
	local Alive = {}
	for _, Enemy in ipairs(self.Alive) do
		if IsEnemyDead(Enemy) then
			table.insert(self.Respawns, self.Time + P.RespawnDelay)
		else
			table.insert(Alive, Enemy)
		end
	end
	self.Alive = Alive

	-- 처음 채우기 / 예약된 리스폰 (예약이 없어도 모자라면 채운다 — 외부에서 파괴된 경우)
	if self.Time < self.NextSpawn or #self.Alive + self.Pending >= P.MaxAlive then
		return
	end
	local bDue = #self.Respawns == 0
	for Index, At in ipairs(self.Respawns) do
		if self.Time >= At then
			table.remove(self.Respawns, Index)
			bDue = true
			break
		end
	end
	if bDue then
		self:SpawnOne()
		self.NextSpawn = self.Time + P.SpawnInterval
	end
end

function EnemySpawner:SpawnOne()
	local P      = self.Properties
	local Angle  = math.random() * math.pi * 2
	local Radius = P.SpawnRadius * math.sqrt(math.random())
	local Wanted = self.Center + Vector3(math.cos(Angle) * Radius, math.sin(Angle) * Radius, 0)
	-- 내비메시 위로: 가운데에서 그 지점까지 경로의 끝 (막혔으면 닿을 수 있는 가장 가까운 곳)
	local Position = Wanted
	local Path = AI.FindPath(self.Center, Wanted)
	if Path ~= nil and #Path > 0 then
		Position = Path[#Path]
	end
	self.Pending = self.Pending + 1
	Scene.SpawnPrefab(P.EnemyPrefab, Position, function(Root)
		self.Pending = math.max(0, self.Pending - 1)
		Root:SetRotation(Quat.FromEuler(0, math.random() * 360, 0))
		table.insert(self.Alive, Root)
	end)
end

return EnemySpawner
