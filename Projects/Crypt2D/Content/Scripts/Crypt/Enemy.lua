-- Crypt2D 적 (Prefabs/Crypt/<Kind>.eprefab, 속성 Kind = Enemies.etable 행). 수치는 데이터 표, 행동은 Behavior:
--   Flyer(망령: 키네마틱 바디 — 벽을 통과해 위아래로 흔들리며 추적), Melee(해골: 땅에서 솟아남 → 순찰 → 발견하면 추적 → 멈칫 후 돌진 베기),
--   Charger(구울: 질주 — 방향을 늦게 바꿔 지나친다, 벽이면 점프), Leaper(고양이: 다가와 도약), Caster(사제: 거리 유지, 시전 플립북
--   "Shoot" 이벤트에 불덩이 — 2층부터 3갈래), Boss(BossAngel.lua). 걷는 적은 2D 이동기(입력만 넣는다), 나는 적은 위치를 직접 옮긴다.
--   피격: 흰 번쩍임 + 넉백(걷는 적 = 짧은 경직 동안 뒤로 이동 입력, 나는 적 = 속도) + 데미지 숫자, 죽으면 GameManager:OnEnemyDied.
--   GameManager가 멈추면(일시정지·히트스톱) AI도 멈춘다.
local U         = Script.Require("Scripts/Crypt/Util.lua")
local CryptData = Script.Require("Scripts/Crypt/CryptData.lua")
local BossAngel = Script.Require("Scripts/Crypt/BossAngel.lua")

local Enemy = {
	Properties = {
		Kind = "Skeleton",
	},
}

local Flipbooks = {
	SkeletonWalk = "Sprites/Crypt/Skeleton_Walk.eflipbook", SkeletonRise = "Sprites/Crypt/Skeleton_Rise.eflipbook",
	WizardIdle = "Sprites/Crypt/Wizard_Idle.eflipbook", WizardCast = "Sprites/Crypt/Wizard_Cast.eflipbook",
}

function Enemy:OnStart()
	self.GM = Scene.Find("GameManager"):GetScript()
	local Init = self.GM:TakeSpawnInit(self.entity) or {}
	self.Room = Init.Room or self.GM.CurrentRoom
	self.Def = CryptData.Enemy(self.Properties.Kind)
	local Def = self.Def
	local Mult = self.GM.Balance.HealthMultiplier[math.min(self.GM.Floor, #self.GM.Balance.HealthMultiplier)] or 1
	self.MaxHealth = Def.MaxHealth * Mult
	self.Health = self.MaxHealth
	self.Radius = Def.Radius
	self.Behavior = Def.Behavior
	self.Sprite = self.entity:GetComponent("SpriteComponent")
	self.bWalker = self.entity:HasComponent("CharacterMovement2DComponent")
	if self.bWalker then
		self.entity:GetComponent("CharacterMovement2DComponent").MaxSpeed = Def.MoveSpeed
	end
	self.Flash = 0.0
	self.Stun = 0.0
	self.KnockDir = 0
	self.Cooldown = self.GM.Rng:Range(0.4, 1.0) * Def.AttackCooldown
	self.Dir = self.GM.Rng:Chance(0.5) and 1 or -1
	self.State = "Idle"
	self.StateTime = 0.0
	self.VX, self.VZ = 0.0, 0.0
	self.Bob = self.GM.Rng:Range(0, 6.28)
	local P = self.entity:GetWorldPosition()
	self.HomeX, self.HomeZ = P.X, P.Z
	if self.Behavior == "Melee" then
		self:SetState("Rise")
		self.entity:PlayFlipbook(Flipbooks.SkeletonRise)
	elseif self.Behavior == "Boss" then
		BossAngel.Start(self)
	else
		self:SetState("Idle")
		self.GM:SpawnFx("Fx_Flame", P.X, P.Z - 60, { Scale = 0.8 })
	end
end

function Enemy:SetState(State)
	self.State = State
	self.StateTime = 0.0
end

function Enemy:PlayerInfo()
	local GM = self.GM
	local PX, PZ = GM:PlayerPos()
	local P = self.entity:GetWorldPosition()
	local Player = GM:PlayerScript()
	local bValid = Player ~= nil and not Player.bDead
	return P, PX, PZ, bValid
end

function Enemy:Face(DirX)
	if DirX ~= 0 then
		local bRight = DirX > 0
		self.bFacingRight = bRight
		-- 시트 원본 방향과 다르면 좌우 반전
		self.entity:SetSpriteFlip(bRight ~= self.Def.FacesRight, false)
	end
end

function Enemy:OnUpdate(Dt)
	local GM = self.GM
	if self.bDead or GM:IsFrozen() then return end
	self.StateTime = self.StateTime + Dt
	self.Cooldown = math.max(0, self.Cooldown - Dt)
	-- 번쩍임
	if self.Flash > 0 then
		self.Flash = self.Flash - Dt
		local C = self.Flash > 0 and 3.0 or 1.0
		self.Sprite.Color = Vector4(C, C, C, 1)
	end
	local P, PX, PZ, bPlayer = self:PlayerInfo()
	-- 몸 닿음 데미지
	if bPlayer and self.Def.ContactDamage > 0 and self.State ~= "Rise" then
		local CZ = U.Clamp(P.Z, PZ - 48, PZ + 48)
		if U.Length(P.X - PX, P.Z - CZ) < self.Radius + 26 then
			GM:DamagePlayer(self.Def.ContactDamage, P.X, P.Z)
		end
	end
	if self.Behavior == "Boss" then
		BossAngel.Update(self, Dt, P, PX, PZ, bPlayer)
		return
	end
	-- 넉백 경직: 걷는 적은 뒤로 이동 입력만
	if self.Stun > 0 then
		self.Stun = self.Stun - Dt
		if self.bWalker then self.entity:AddMovementInput(Vector3(self.KnockDir, 0, 0)) end
		if self.Behavior == "Flyer" then self:MoveFlyer(Dt, P) end
		return
	end
	local B = self.Behavior
	if B == "Flyer" then self:UpdateFlyer(Dt, P, PX, PZ, bPlayer)
	elseif B == "Melee" then self:UpdateMelee(Dt, P, PX, PZ, bPlayer)
	elseif B == "Charger" then self:UpdateCharger(Dt, P, PX, PZ, bPlayer)
	elseif B == "Leaper" then self:UpdateLeaper(Dt, P, PX, PZ, bPlayer)
	elseif B == "Caster" then self:UpdateCaster(Dt, P, PX, PZ, bPlayer) end
end

-- ---- 공용 감지
function Enemy:WallAhead(P, Dir, Height)
	return Physics2D.Raycast(U.V(P.X, P.Z + (Height or 0)), U.V(Dir, 0), self.Radius + 30, "Terrain") ~= nil
end

function Enemy:GroundAhead(P, Dir)
	return Physics2D.Raycast(U.V(P.X + Dir * (self.Radius + 30), P.Z), U.V(0, -1), 160, "Terrain") ~= nil
end

function Enemy:CanSee(P, PX, PZ)
	local DX, DZ, Dist = U.Normalize(PX - P.X, PZ - P.Z)
	return Physics2D.Raycast(U.V(P.X, P.Z), U.V(DX, DZ), Dist, "Terrain") == nil
end

-- ---- 망령: 플레이어 위쪽을 향해 부드럽게, 흔들림 (벽 통과)
function Enemy:UpdateFlyer(Dt, P, PX, PZ, bPlayer)
	self.Bob = self.Bob + Dt * 3
	local Dist = U.Length(PX - P.X, PZ - P.Z)
	local TX, TZ = self.HomeX, self.HomeZ
	if bPlayer and Dist < self.Def.AggroRange then
		TX, TZ = PX, PZ + 30
	end
	local DX, DZ, L = U.Normalize(TX - P.X, TZ - P.Z)
	local Speed = (L > 30) and self.Def.MoveSpeed or 0
	self.VX = U.Lerp(self.VX, DX * Speed, U.Smooth(2.5, Dt))
	self.VZ = U.Lerp(self.VZ, DZ * Speed, U.Smooth(2.5, Dt))
	self:Face(self.VX)
	self:MoveFlyer(Dt, P)
end

function Enemy:MoveFlyer(Dt, P)
	self.VX = self.VX * (1 - math.min(1, Dt * (self.Stun > 0 and 4 or 0)))
	local NX = P.X + self.VX * Dt
	local NZ = P.Z + self.VZ * Dt + math.sin(self.Bob) * 40 * Dt
	-- 방 밖으로는 나가지 않는다
	local R = self.Room and self.Room.Rect
	if R then
		NX = U.Clamp(NX, R[1] + 2.5 * U.Cell, R[3] - 2.5 * U.Cell)
		NZ = U.Clamp(NZ, R[2] + 2.5 * U.Cell, R[4] - 2.5 * U.Cell)
	end
	self.entity:SetPosition(U.V(NX, NZ, 0))
end

-- ---- 해골: 솟아남 → 순찰/추적 → 멈칫 → 돌진 베기
function Enemy:UpdateMelee(Dt, P, PX, PZ, bPlayer)
	local E = self.entity
	local Def = self.Def
	if self.State == "Rise" then
		if self.StateTime > 0.75 then
			self:SetState("Patrol")
			E:PlayFlipbook(Flipbooks.SkeletonWalk)
		end
		return
	end
	local DX = PX - P.X
	local bSees = bPlayer and math.abs(DX) < Def.AggroRange and math.abs(PZ - P.Z) < 260
	if self.State == "Windup" then
		self.Sprite.Color = Vector4(1.8, 0.55 + 0.3 * math.sin(self.StateTime * 40), 0.55, 1)
		if self.StateTime > 0.38 then
			self:SetState("Lunge")
			E:Dash(Vector3(self.LungeDir, 0, 0))
			self.bLungeHit = false
			self.GM:Sound("Swing2")
		end
		return
	elseif self.State == "Lunge" then
		E:AddMovementInput(Vector3(self.LungeDir, 0, 0))
		if bPlayer and not self.bLungeHit and math.abs(DX) < self.Radius + 70 and math.abs(PZ - P.Z) < 120 then
			self.bLungeHit = self.GM:DamagePlayer(Def.AttackDamage, P.X, P.Z)
		end
		if self.StateTime > 0.3 then
			self:SetState("Chase")
			self.Cooldown = Def.AttackCooldown
			self.Sprite.Color = Vector4(1, 1, 1, 1)
		end
		return
	end
	if bSees then
		self:SetState("Chase")
		local Dir = U.Sign(DX)
		self:Face(Dir)
		if math.abs(DX) < Def.AttackRange and self.Cooldown <= 0 and E:IsGrounded() then
			self.LungeDir = Dir
			self:SetState("Windup")
			return
		end
		if math.abs(DX) > 40 then
			E:AddMovementInput(Vector3(Dir, 0, 0))
			if self:WallAhead(P, Dir, -20) and E:IsGrounded() then E:Jump() end
		end
	else
		-- 순찰: 벽이나 낭떠러지 앞에서 돈다
		if self:WallAhead(P, self.Dir, -20) or not self:GroundAhead(P, self.Dir) then
			self.Dir = -self.Dir
		end
		self:Face(self.Dir)
		E:AddMovementInput(Vector3(self.Dir * 0.45, 0, 0))
	end
end

-- ---- 구울: 질주, 방향은 0.7초마다만 다시 정한다
function Enemy:UpdateCharger(Dt, P, PX, PZ, bPlayer)
	local E = self.entity
	local Dist = U.Length(PX - P.X, PZ - P.Z)
	if self.State == "Idle" then
		if bPlayer and Dist < self.Def.AggroRange then self:SetState("Charge") end
		E:AddMovementInput(Vector3(0, 0, 0))
		return
	end
	if self.StateTime > 0.7 or self.ChargeDir == nil then
		self.StateTime = 0
		self.ChargeDir = U.Sign(PX - P.X)
		if self.ChargeDir == 0 then self.ChargeDir = 1 end
	end
	self:Face(self.ChargeDir)
	E:AddMovementInput(Vector3(self.ChargeDir, 0, 0))
	if E:IsGrounded() and (self:WallAhead(P, self.ChargeDir, -20) or (PZ - P.Z > 200 and math.abs(PX - P.X) < 300)) then
		E:Jump()
	end
	if self.GM.Rng:Chance(Dt * 6) then
		self.GM:SpawnFx("Fx_Spark", P.X - self.ChargeDir * 30, P.Z + 40, { Color = { 1, 0.6, 0.2, 1 } })
	end
end

-- ---- 고양이: 다가와 도약
function Enemy:UpdateLeaper(Dt, P, PX, PZ, bPlayer)
	local E = self.entity
	local DX = PX - P.X
	local Dist = math.abs(DX)
	if not bPlayer or Dist > self.Def.AggroRange then
		E:AddMovementInput(Vector3(0, 0, 0))
		return
	end
	local Dir = U.Sign(DX)
	if E:IsGrounded() then
		self:Face(Dir)
		if Dist < self.Def.AttackRange and self.Cooldown <= 0 then
			E:Jump()
			self.LeapDir = Dir
			self.Cooldown = self.Def.AttackCooldown
			self.GM:Sound("Swing1")
		elseif Dist > 150 then
			E:AddMovementInput(Vector3(Dir * 0.7, 0, 0))
			if self:WallAhead(P, Dir, -10) then E:Jump() end
		end
	elseif self.LeapDir then
		E:AddMovementInput(Vector3(self.LeapDir, 0, 0))
	end
end

-- ---- 사제: 거리 유지 + 시전
function Enemy:UpdateCaster(Dt, P, PX, PZ, bPlayer)
	local E = self.entity
	local Def = self.Def
	local DX = PX - P.X
	local Dist = U.Length(DX, PZ - P.Z)
	if self.State == "Cast" then
		E:AddMovementInput(Vector3(0, 0, 0))
		self:Face(U.Sign(DX))
		-- 이벤트가 오지 않아도 (플립북 해석 지연 등) 시간으로 쏜다
		if not self.bShot and self.StateTime > 0.62 then self:OnFlipbookEvent_Shoot() end
		if self.StateTime > 0.9 then
			self:SetState("Idle")
			E:PlayFlipbook(Flipbooks.WizardIdle)
		end
		return
	end
	if not bPlayer or Dist > Def.AggroRange then
		E:AddMovementInput(Vector3(0, 0, 0))
		return
	end
	self:Face(U.Sign(DX))
	local Move = 0
	if Dist < 420 then Move = -U.Sign(DX) elseif Dist > Def.AttackRange * 0.8 then Move = U.Sign(DX) end
	if Move ~= 0 and (self:WallAhead(P, Move, -20) or not self:GroundAhead(P, Move)) then Move = 0 end
	E:AddMovementInput(Vector3(Move * 0.6, 0, 0))
	if self.Cooldown <= 0 and Dist < Def.AttackRange and self:CanSee(P, PX, PZ) then
		self:SetState("Cast")
		self.bShot = false
		E:PlayFlipbook(Flipbooks.WizardCast)
		self.Cooldown = Def.AttackCooldown
	end
end

function Enemy:OnFlipbookEvent_Shoot(Frame)
	if self.bDead or self.State ~= "Cast" or self.bShot then return end
	self.bShot = true
	local GM = self.GM
	local P = self.entity:GetWorldPosition()
	local PX, PZ = GM:PlayerPos()
	local OX = P.X + (self.bFacingRight and 40 or -40)
	local OZ = P.Z + 40
	local Base = U.AngleOf(PX - OX, PZ - OZ)
	local Spread = GM.Floor >= 2 and { -14, 0, 14 } or { 0 }
	for _, A in ipairs(Spread) do
		local DX, DZ = U.DirOf(Base + A)
		GM:SpawnProjectile({ Team = "Enemy", X = OX, Z = OZ, VX = DX * self.Def.ProjectileSpeed, VZ = DZ * self.Def.ProjectileSpeed,
		                     Damage = self.Def.AttackDamage, Kind = "Fireball", Radius = 26, Range = 1800 })
	end
	GM:Sound("Fireball")
end

-- ---- 피격 / 사망
function Enemy:TakeDamage(Amount, bCrit, KX, KZ, Knockback)
	if self.bDead or self.State == "Rise" or (self.Invulnerable and self.Invulnerable > 0) then return false end
	local GM = self.GM
	self.Health = self.Health - Amount
	self.Flash = 0.08
	local P = self.entity:GetWorldPosition()
	GM:ShowNumber(P.X, P.Z + 90, tostring(math.floor(Amount)), bCrit and { 1, 0.85, 0.3, 1 } or { 1, 1, 1, 1 }, bCrit and 1.4 or 1.0)
	GM:SpawnFx("Fx_Spark", P.X - (KX or 0) * 20, P.Z + 10, {})
	GM:Sound(bCrit and "HitHeavy" or "Hit")
	if self.Behavior ~= "Boss" then
		local Strength = (Knockback or 300) / 600
		self.Stun = 0.12 + 0.12 * Strength
		self.KnockDir = U.Sign(KX or 0)
		if self.Behavior == "Flyer" then
			self.VX, self.VZ = (KX or 0) * (Knockback or 300), (KZ or 0) * (Knockback or 300)
		end
		if self.State == "Windup" or self.State == "Lunge" then
			self:SetState("Chase")
			self.Sprite.Color = Vector4(1, 1, 1, 1)
		end
	end
	if self.Behavior == "Boss" then BossAngel.OnDamaged(self) end
	if self.Health <= 0 then
		self:Die()
	end
	return true
end

function Enemy:Die()
	self.bDead = true
	local P = self.entity:GetWorldPosition()
	self.GM:OnEnemyDied(self.entity, self.Def, P.X, P.Z)
	if self.Behavior == "Boss" then
		BossAngel.OnDeath(self)
	end
	self.entity:Destroy()
end

return Enemy
