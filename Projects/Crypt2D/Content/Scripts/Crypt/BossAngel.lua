-- Crypt2D 보스 "타락 천사 세라핌" 행동 (Enemy.lua의 Behavior = Boss가 부르는 모듈 — 함수는 적 인스턴스 self를 받는다).
--   키네마틱 비행 바디(위치를 직접 옮김), 방(Room.Rect) 안에서만 움직인다. 패턴 (맴돌기 사이에 무작위, 같은 패턴 연속 없음):
--     Dash   — 0.6초 예고(붉게 번쩍 + 공격 플립북) 뒤 플레이어가 있던 곳으로 돌진 (몸 닿음 데미지가 크다)
--     Ring   — 방사형 탄막 3파(2페이즈 4파), 파마다 각을 돌려 틈이 움직인다
--     Aimed  — 플레이어를 향해 부채꼴 5연사
--     Summon — 망령 2~3마리 소환 (살아 있는 망령이 3 미만일 때만)
--   체력 50% 아래 = 2페이즈 (빨라지고 탄이 늘어남, 알림). 보스 체력바는 HUD(GameManager → Hud:ShowBoss).
local U = Script.Require("Scripts/Crypt/Util.lua")

local BossAngel = {}

local IdleBook   = "Sprites/Crypt/Angel_Idle.eflipbook"
local AttackBook = "Sprites/Crypt/Angel_Attack.eflipbook"
local Patterns = { "Dash", "Ring", "Aimed", "Summon" }

function BossAngel.Start(self)
	self.Phase = 1
	self.State = "Intro"
	self.StateTime = 0
	self.LastPattern = nil
	self.Shots = 0
	self.Minions = {}
	self.PatternCount = {}
	self.Invulnerable = 1.6
	local GM = self.GM
	GM.Boss = self
	GM:Sound("BossRoar")
	GM:AddShake(10, 0.6)
	GM:Toast(self.Def.DisplayName, 2.2)
	if GM.Hud then GM.Hud:ShowBoss(self.Def.DisplayName, 1.0) end
	local P = self.entity:GetWorldPosition()
	self.HomeX, self.HomeZ = P.X, P.Z
	Log.Info("[Crypt2D] 보스 등장: " .. self.Def.DisplayName)
end

local function Bounds(self)
	local R = self.Room.Rect
	return R[1] + 3.5 * U.Cell, R[2] + 3.5 * U.Cell, R[3] - 3.5 * U.Cell, R[4] - 3.0 * U.Cell
end

local function MoveToward(self, P, TX, TZ, Speed, Dt)
	local X0, Z0, X1, Z1 = Bounds(self)
	TX, TZ = U.Clamp(TX, X0, X1), U.Clamp(TZ, Z0, Z1)
	local DX, DZ, L = U.Normalize(TX - P.X, TZ - P.Z)
	local Step = math.min(L, Speed * Dt)
	local NX, NZ = P.X + DX * Step, P.Z + DZ * Step
	self.entity:SetPosition(U.V(NX, NZ, 0))
	return L <= Speed * Dt + 1
end

local function SetState(self, State)
	self.State = State
	self.StateTime = 0
	self.Shots = 0
	if State == "Dash" or State == "Ring" or State == "Aimed" or State == "Summon" then
		self.PatternCount[State] = (self.PatternCount[State] or 0) + 1
		Log.Info(string.format("[Crypt2D] 보스 패턴 %s (%d번째, 체력 %.0f%%)", State, self.PatternCount[State], 100 * self.Health / self.MaxHealth))
	end
end

local function Fire(self, X, Z, Angle, Speed, Kind)
	local DX, DZ = U.DirOf(Angle)
	self.GM:SpawnProjectile({ Team = "Enemy", X = X, Z = Z, VX = DX * Speed, VZ = DZ * Speed, Damage = self.Def.AttackDamage,
	                          Kind = Kind or "Orb", Radius = 22, Range = 3000, Life = 7 })
end

function BossAngel.Update(self, Dt, P, PX, PZ, bPlayer)
	local GM = self.GM
	local E = self.entity
	local SpeedScale = self.Phase == 2 and 1.25 or 1.0
	self.Invulnerable = math.max(0, (self.Invulnerable or 0) - Dt)
	self.Bob = (self.Bob or 0) + Dt * 2.2
	self:Face(U.Sign(PX - P.X))
	if GM.Hud then GM.Hud:ShowBoss(self.Def.DisplayName, self.Health / self.MaxHealth) end

	local X0, Z0, X1, Z1 = Bounds(self)
	if self.State == "Intro" then
		MoveToward(self, P, (X0 + X1) * 0.5, self.Room.Rect[2] + 13 * U.Cell, 220, Dt)
		if self.StateTime > 1.8 then SetState(self, "Hover") end
	elseif self.State == "Hover" then
		local TX = U.Clamp(PX + math.sin(self.Bob * 0.7) * 260, X0, X1)
		local TZ = self.Room.Rect[2] + 12 * U.Cell + math.sin(self.Bob) * 60 -- 방 가운데 위 (카메라가 플레이어와 함께 담는 높이)
		MoveToward(self, P, TX, TZ, self.Def.MoveSpeed * 0.8 * SpeedScale, Dt)
		if self.StateTime > (self.Phase == 2 and 0.9 or 1.4) and bPlayer then
			local Choices = {}
			for _, Name in ipairs(Patterns) do
				local bSummonOk = Name ~= "Summon" or #BossAngel.LiveMinions(self) < 3
				if Name ~= self.LastPattern and bSummonOk then Choices[#Choices + 1] = Name end
			end
			local Next = GM.Rng:Pick(Choices)
			self.LastPattern = Next
			SetState(self, Next)
			if Next ~= "Summon" then E:PlayFlipbook(AttackBook) end
		end
	elseif self.State == "Dash" then
		if self.StateTime < 0.6 then
			-- 예고: 제자리 떨림 + 붉은 번쩍임, 목표 기억
			self.Sprite.Color = Vector4(1.8, 0.5 + 0.4 * math.sin(self.StateTime * 50), 0.5, 1)
			self.DashX, self.DashZ = PX, PZ
		else
			local Done = MoveToward(self, P, self.DashX, self.DashZ, 1500 * SpeedScale, Dt)
			if GM.Rng:Chance(Dt * 30) then
				GM:SpawnAfterimage("Sprites/Crypt/Angel.esprite", "Angel0", P.X, P.Z, self.bFacingRight ~= self.Def.FacesRight,
				                   { 1, 0.5, 0.6, 0.5 }, 0.3)
			end
			if Done or self.StateTime > 1.8 then
				self.Sprite.Color = Vector4(1, 1, 1, 1)
				GM:AddShake(6, 0.2)
				SetState(self, "Recover")
				E:PlayFlipbook(IdleBook)
			end
		end
	elseif self.State == "Ring" then
		local Waves = self.Phase == 2 and 4 or 3
		local Count = self.Phase == 2 and 20 or 14
		MoveToward(self, P, P.X, P.Z, 0, Dt)
		if self.Shots < Waves and self.StateTime > 0.45 + self.Shots * 0.38 then
			local Offset = self.Shots * (180 / Count) + GM.Rng:Range(0, 8)
			for I = 0, Count - 1 do
				Fire(self, P.X, P.Z, Offset + I * 360 / Count, self.Def.ProjectileSpeed * SpeedScale)
			end
			self.Shots = self.Shots + 1
			GM:Sound("Fireball")
		end
		if self.StateTime > 0.45 + Waves * 0.38 + 0.4 then
			SetState(self, "Recover")
			E:PlayFlipbook(IdleBook)
		end
	elseif self.State == "Aimed" then
		local Total = self.Phase == 2 and 7 or 5
		MoveToward(self, P, P.X + math.sin(self.StateTime * 3) * 60, P.Z, 120, Dt)
		if self.Shots < Total and self.StateTime > 0.4 + self.Shots * 0.17 then
			local Base = U.AngleOf(PX - P.X, PZ - P.Z)
			for _, A in ipairs({ -12, 0, 12 }) do
				Fire(self, P.X, P.Z - 20, Base + A, self.Def.ProjectileSpeed * 1.4 * SpeedScale, "Fireball")
			end
			self.Shots = self.Shots + 1
			GM:Sound("Shoot")
		end
		if self.StateTime > 0.4 + Total * 0.17 + 0.3 then
			SetState(self, "Recover")
			E:PlayFlipbook(IdleBook)
		end
	elseif self.State == "Summon" then
		MoveToward(self, P, P.X, P.Z + 20 * Dt, 40, Dt)
		if self.Shots == 0 and self.StateTime > 0.5 then
			local N = self.Phase == 2 and 3 or 2
			for I = 1, N do
				local SX = P.X + (I - (N + 1) / 2) * 260
				local SZ = P.Z - 80
				GM:SpawnFx("Fx_Death", SX, SZ, { Color = { 0.6, 0.5, 1, 1 } })
				BossAngel.SpawnMinion(self, SX, SZ)
			end
			self.Shots = 1
			GM:Sound("Portal")
		end
		if self.StateTime > 1.2 then SetState(self, "Recover") end
	elseif self.State == "Recover" then
		MoveToward(self, P, P.X, self.Room.Rect[2] + 12 * U.Cell, self.Def.MoveSpeed * 0.6, Dt)
		if self.StateTime > (self.Phase == 2 and 0.35 or 0.6) then SetState(self, "Hover") end
	end
end

function BossAngel.SpawnMinion(self, X, Z)
	local GM = self.GM
	Scene.SpawnPrefab("Prefabs/Crypt/Ghost.eprefab", U.V(X, Z, 0), function(Root)
		self.Room.Enemies[#self.Room.Enemies + 1] = Root
		self.Minions[#self.Minions + 1] = Root
		GM.SpawnInit[Root.Id] = { Room = self.Room, Kind = "Ghost" }
	end)
end

function BossAngel.LiveMinions(self)
	local Alive = {}
	for _, M in ipairs(self.Minions) do
		if M:IsValid() then Alive[#Alive + 1] = M end
	end
	self.Minions = Alive
	return Alive
end

function BossAngel.OnDamaged(self)
	if self.Phase == 1 and self.Health < self.MaxHealth * 0.5 then
		self.Phase = 2
		self.GM:Toast("세라핌이 분노한다!", 1.8)
		self.GM:Sound("BossRoar")
		self.GM:AddShake(8, 0.4)
		Log.Info("[Crypt2D] 보스 2페이즈")
	end
end

function BossAngel.OnDeath(self)
	local GM = self.GM
	-- 남은 하수인과 적 탄을 정리한다
	for _, M in ipairs(BossAngel.LiveMinions(self)) do
		local S = M:GetScript()
		if S and S.Die and not S.bDead then S:Die() end
	end
	for _, P in ipairs(GM.Projectiles) do
		if P.Team == "Enemy" and not P.bDone then GM:FinishProjectile(P, P.X, P.Z) end
	end
	local P = self.entity:GetWorldPosition()
	for I = 1, 5 do
		GM:SpawnFx("Fx_Death", P.X + GM.Rng:Range(-150, 150), P.Z + GM.Rng:Range(-120, 120), { Scale = 1.5 })
	end
	GM:AddShake(14, 0.8)
	GM.BossPatternCount = self.PatternCount -- 자동 검증 확인용 (보스 엔티티는 곧 사라진다)
	GM:OnBossDefeated()
end

return BossAngel
