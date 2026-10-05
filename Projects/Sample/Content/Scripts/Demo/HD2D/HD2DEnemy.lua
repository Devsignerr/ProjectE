-- HD-2D 데모 일반 적 (Prefabs/Demo/HD2D/<종류>.eprefab — 캡슐 이동기 + Visual > Body·Shadow·HpBack·HpFill). 종류는 속성 Kind,
-- 수치는 Data/Demo/HD2D/Enemies.etable 행(HD2DData.Enemy), 행동은 행의 Behavior:
--   Hopper  (슬라임)   깡충 뛰며 다가와 몸으로 부딪힌다 (뛰는 동안만 이동 + 몸 스프라이트 포물선)
--   Flyer   (박쥐)     플레이어 주위를 맴돌다 예고(붉은 번쩍 + 날갯짓 빨라짐) 뒤 급강하 돌진 → 다시 떠오름
--   Charger (고블린)   다가가다 예비 동작(단검 치켜듦) 뒤 직선 돌진 찌르기
--   Archer  (해골 궁수) 거리를 두며 활시위를 당긴 뒤 화살 (관리자 투사체) — 너무 가까우면 물러난다
--   Spore   (독버섯)   느리게 다가가 부풀었다가 발밑에 독 웅덩이 (관리자 위험 지대 — 0.5초마다 피해)
-- 변형(동굴 박쥐·수정 슬라임): 행의 Look = 그림·몸 모양을 빌릴 바탕 종류(플립북 <Look>_<동작>), Tint = 몸 색 배율 (HD2DData.EnemyLook).
-- 공통: 발견 거리 밖이면 집 근처를 어슬렁, 맞으면 하얗게 번쩍 + 넉백 + 머리 위 체력바 3초, 예비 동작 중 맞으면 끊긴다(경직),
--       체력 0 → 관리자 OnEnemyKilled(전리품·경험치·부활 예약) 후 펑 효과와 함께 사라짐.
--   돌진·급강하는 entity:AddKnockback(방향 × ProjectileSpeed, 시간) — 이동기가 경직 동안 수평 속도를 덮어써 미끄러진다.
--   이동기가 루트를 이동 방향으로 돌리므로 Visual 회전을 매 프레임 상쇄한다 (스프라이트는 늘 카메라를 본다). 원본 그림은 오른쪽을 본다.
--   길찾기: 걷는 적(박쥐 제외)은 쫓기·집으로 돌아가기에 내비메시 경로(AI.FindPath — 지면 높이 점, 0.8~1.2초마다 다시)를 따라 울타리·건물·개울을
--     돌아간다. 씬에 내비메시가 없거나 경로가 없으면 곧장 걷는다 (HD2DGameplay.py 머리 주석의 굽기 순서).
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DEnemy = {
	Properties = {
		Kind = "Slime",
	},
}

-- 종류별 모양: 몸 스프라이트 기준 높이(발 위 — 박쥐는 공중), 숫자·효과 높이(캡슐 중심 위), 좌우 반전 쓰는가
local Shapes = {
	Slime    = { Lift = 0, HitHeight = 20, bFlip = true },
	Bat      = { Lift = 105, HitHeight = 110, bFlip = false },
	Goblin   = { Lift = 0, HitHeight = 70, bFlip = true },
	Archer   = { Lift = 0, HitHeight = 90, bFlip = true },
	Mushroom = { Lift = 0, HitHeight = 60, bFlip = false },
}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function HD2DEnemy:OnStart()
	self.Kind = self.Properties.Kind
	self.Row = D.Enemy(self.Kind)
	self.Look = D.EnemyLook(self.Kind)
	self.Shape = Shapes[self.Look] or Shapes.Slime
	local T = self.Row.Tint or {}
	self.Tint = Vector4(T[1] or 1, T[2] or 1, T[3] or 1, T[4] or 1)
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.HpBack = self.Visual:FindChild("HpBack")
	self.HpFill = self.Visual:FindChild("HpFill")
	self.BodyBase = self.Body:GetPosition()
	self.Foot = self.BodyBase.Z - self.Shape.Lift      -- 발 = 캡슐 중심 + Foot (음수)
	self.Radius = self.Row.Radius
	self.HitHeight = self.Shape.HitHeight
	self.Health = self.Row.MaxHealth
	self.Home = self.entity:GetWorldPosition()
	self.Seed = 7 + self.entity.Id * 977
	self.State, self.Timer = "Idle", 0.3 + self:Random() * 1.0
	self.Cooldown = 1.0 + self:Random()
	self.ContactCooldown, self.Flash, self.BarTime = 0.0, 0.0, 0.0
	self.Anim = ""
	self.FlipX = false
	self.Time = self:Random() * 10
	self.bDead = false
	self.GM:RegisterEnemy(self)
	self.GM:SpawnFx("Poof", self.entity:GetWorldPosition() + Vector3(0, 6, self.Foot + 20), { Scale = 0.8 })
	self:Play("Idle")
end

function HD2DEnemy:OnDestroy()
	if self.GM then self.GM:UnregisterEnemy(self) end
end

function HD2DEnemy:Random()
	self.Seed = (self.Seed * 1103515245 + 12345) % 2147483648
	return (self.Seed % 100000) / 100000.0
end

function HD2DEnemy:Play(Anim)
	local Book = Anim
	if self.Look == "Slime" then Book = (Anim == "Move" or Anim == "Attack") and "Hop" or "Idle" end
	if Book ~= self.Anim then
		self.Anim = Book
		self.Body:PlayFlipbook("Sprites/HD2D/" .. self.Look .. "_" .. Book .. ".eflipbook")
	end
end

function HD2DEnemy:Face(Dir)
	if not self.Shape.bFlip then return end
	if Dir.X < -0.15 then self.FlipX = true elseif Dir.X > 0.15 then self.FlipX = false end
	self.Body:SetSpriteFlip(self.FlipX, false)
end

-- 목표(지면 점)로 가는 다음 방향 (내비메시 경로 따라가기, 없으면 곧장)
function HD2DEnemy:PathDir(Pos, Goal)
	local Direct = Flat(Goal - Pos)
	local L = Direct:Length()
	if L < 1 then return Vector3(0, 0, 0) end
	self.PathTimer = (self.PathTimer or 0) - (self.LastDt or 0)
	if self.PathTimer <= 0 or not self.PathGoal or Flat(self.PathGoal - Goal):Length() > 150 then
		self.PathTimer = 0.8 + self:Random() * 0.4
		self.PathGoal = Goal
		local Foot = Vector3(Pos.X, Pos.Y, Pos.Z + self.Foot)
		self.Path = AI.FindPath(Foot, Vector3(Goal.X, Goal.Y, Foot.Z))
		self.PathIndex = 2
		if self.Path then self.GM.Report.Paths = (self.GM.Report.Paths or 0) + 1 end
	end
	if self.Path then
		while self.Path[self.PathIndex] and Flat(self.Path[self.PathIndex] - Pos):Length() < 70 do
			self.PathIndex = self.PathIndex + 1
		end
		local Next = self.Path[self.PathIndex]
		if Next then
			local D_ = Flat(Next - Pos)
			if D_:Length() > 1 then return D_:Normalized() end
		end
	end
	return Direct * (1.0 / L)
end

function HD2DEnemy:SetState(State, Time)
	self.State, self.Timer = State, Time or 0
end

function HD2DEnemy:OnUpdate(Dt)
	if self.bDead or Dt <= 0 then return end
	local E = self.entity
	local Pos = E:GetWorldPosition()
	local R = self.Row
	self.Time = self.Time + Dt
	self.Timer = self.Timer - Dt
	self.Cooldown = self.Cooldown - Dt
	self.ContactCooldown = math.max(0.0, self.ContactCooldown - Dt)

	local Player = self.GM:GetPlayer()
	local ToPlayer, Dist = Vector3(0, 0, 0), 1.0e9
	if Player and not Player.bDead then
		ToPlayer = Flat(Player.entity:GetWorldPosition() - Pos)
		Dist = ToPlayer:Length()
	end
	local DirP = Dist > 1 and ToPlayer * (1.0 / Dist) or Vector3(0, 1, 0)
	self.LastDt = Dt
	self.PlayerPos = Pos + ToPlayer
	self.bAggro = Dist < R.AggroRange or (self.bAggro and Dist < R.AggroRange * 1.6)
	-- 목줄: 집에서 너무 멀어지면 쫓기를 그만두고 돌아간다 (마을 안까지 따라오지 않게)
	if Flat(Pos - self.Home):Length() > 1400 then self.bAggro = false end

	local B = R.Behavior
	if B == "Hopper" then self:UpdateHopper(Dt, Pos, DirP, Dist)
	elseif B == "Flyer" then self:UpdateFlyer(Dt, Pos, DirP, Dist)
	elseif B == "Charger" then self:UpdateCharger(Dt, Pos, DirP, Dist)
	elseif B == "Archer" then self:UpdateArcher(Dt, Pos, DirP, Dist)
	elseif B == "Spore" then self:UpdateSpore(Dt, Pos, DirP, Dist)
	end

	-- 몸 접촉 피해 (돌진·급강하는 AttackDamage)
	local Contact = self.Radius + 38
	if Player and Dist < Contact and self.ContactCooldown <= 0 then
		local Damage = (self.State == "Attack" and (B == "Charger" or B == "Flyer")) and R.AttackDamage or R.ContactDamage
		if Damage > 0 and Player:TakeDamage(Damage, Pos) then
			self.ContactCooldown = 0.9
		end
	end

	-- 피격 번쩍임 (하얗게 → 원래 색)
	if self.Flash > 0 then
		self.Flash = self.Flash - Dt
		-- 하얗게 덮었다가 빠르게 풀림 (SpriteComponent.FlashColor)
		self.Sprite.FlashColor = Vector4(1, 1, 1, self.Flash > 0 and math.min(0.9, self.Flash * 10) or 0)
		self.Sprite.Color = self.Tint
	elseif self.State == "Windup" then
		-- 예비 동작: 붉게 깜빡 (피할 신호)
		local Blink = math.floor(self.Time * 14) % 2 == 0
		self.Sprite.Color = Blink and Vector4(1.6, 0.7, 0.6, 1) or self.Tint
	else
		self.Sprite.Color = self.Tint
	end
	-- 머리 위 체력바
	if self.BarTime > 0 then
		self.BarTime = self.BarTime - Dt
		if self.BarTime <= 0 then self:ShowBar(false) end
	end
end

-- 집 근처 어슬렁 방향 (멀어지면 집으로)
function HD2DEnemy:WanderDir(Pos)
	local FromHome = Flat(Pos - self.Home)
	if FromHome:Length() > 380 then
		return self:PathDir(Pos, self.Home)
	end
	local A = self:Random() * math.pi * 2
	return Vector3(math.cos(A), math.sin(A), 0)
end

-- ---- 슬라임: 깡충
function HD2DEnemy:UpdateHopper(Dt, Pos, DirP, Dist)
	local E = self.entity
	if self.State == "Hop" then
		local T = math.min(1.0 - self.Timer / 0.42, 1.0)
		if not E:IsStunned() then E:AddMovementInput(self.HopDir) end
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, math.sin(T * math.pi) * 46.0))
		if self.Timer <= 0 then
			self.Body:SetPosition(self.BodyBase)
			self:Play("Idle")
			self:SetState("Idle", (self.bAggro and 0.55 or 1.1) * (0.7 + self:Random() * 0.6))
			self.GM:SpawnFx("Dust", Pos + Vector3(0, 4, self.Foot + 2), { Scale = 0.8 })
		end
	elseif self.Timer <= 0 and not E:IsStunned() then
		self.HopDir = self.bAggro and self:PathDir(Pos, self.PlayerPos) or self:WanderDir(Pos)
		self:SetState("Hop", 0.42)
		self:Play("Move")
		self:Face(self.HopDir)
	end
end

-- ---- 박쥐: 맴돌기 → 예고 → 급강하 → 떠오름
function HD2DEnemy:UpdateFlyer(Dt, Pos, DirP, Dist)
	local E = self.entity
	local R = self.Row
	local Hover = math.sin(self.Time * 5.0) * 12.0
	if self.State == "Windup" then
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, 20 + Hover * 0.3))
		if self.Timer <= 0 then
			self.DiveDir = DirP
			self:SetState("Attack", 0.42)
			self:Play("Attack")
			E:AddKnockback(DirP * R.ProjectileSpeed, 0.4)
			Audio.PlayOneShot("Audio/RPG/Swing2.wav")
		end
	elseif self.State == "Attack" then
		-- 급강하: 몸이 바닥 가까이로
		local T = 1.0 - math.max(self.Timer, 0) / 0.42
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, -70 * math.sin(T * math.pi)))
		if self.Timer <= 0 then
			self:SetState("Recover", 0.6)
			self:Play("Move")
			self.Cooldown = R.AttackCooldown
		end
	else
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, Hover))
		if self.State == "Recover" and self.Timer <= 0 then self:SetState("Idle", 0) end
		local Move
		if self.bAggro then
			-- 플레이어 둘레 250cm 원 위의 점을 쫓는다 (시계 방향으로 돎)
			local Ang = math.atan(-DirP.Y, -DirP.X) + 0.9
			local PP = Pos + DirP * Dist
			local Goal = PP + Vector3(math.cos(Ang), math.sin(Ang), 0) * 260
			local To = Flat(Goal - Pos)
			Move = To:Length() > 30 and To:Normalized() or Vector3(0, 0, 0)
			if self.Cooldown <= 0 and Dist < R.AttackRange and self.State == "Idle" then
				self:SetState("Windup", R.WindupTime)
				self:Play("Windup")
				self.GM:SpawnFx("Alert", Pos + Vector3(0, 20, self.HitHeight + 70), { Blend = 2, Scale = 1.4 })
				Move = nil
			end
		else
			if self.Timer <= 0 then
				self.WanderMove = self:WanderDir(Pos)
				self.Timer = 1.2 + self:Random()
			end
			Move = self.WanderMove and self.WanderMove * 0.5
		end
		if Move and not E:IsStunned() then E:AddMovementInput(Move) end
		self:Play("Move")
	end
end

-- ---- 고블린: 접근 → 예비 동작 → 돌진
function HD2DEnemy:UpdateCharger(Dt, Pos, DirP, Dist)
	local E = self.entity
	local R = self.Row
	if self.State == "Windup" then
		self:Face(self.ChargeDir)
		if self.Timer <= 0 then
			self:SetState("Attack", 0.34)
			self:Play("Attack")
			E:AddKnockback(self.ChargeDir * R.ProjectileSpeed, 0.32)
			self.GM:SpawnFx("Dust", Pos + Vector3(-self.ChargeDir.X * 40, 4, self.Foot + 2), { FlipX = self.ChargeDir.X < 0 })
			Audio.PlayOneShot("Audio/RPG/Swing1.wav")
		end
	elseif self.State == "Attack" then
		if self.Timer <= 0 then
			self:SetState("Recover", 0.5)
			self:Play("Idle")
			self.Cooldown = R.AttackCooldown
		end
	elseif self.State == "Recover" then
		if self.Timer <= 0 then self:SetState("Idle", 0) end
	else
		if self.bAggro then
			if self.Cooldown <= 0 and Dist < R.AttackRange and not E:IsStunned() then
				self.ChargeDir = DirP
				self:SetState("Windup", R.WindupTime)
				self:Play("Windup")
				self:Face(DirP)
				self.GM:SpawnFx("Alert", Pos + Vector3(0, 20, self.HitHeight + 60), { Blend = 2, Scale = 1.4 })
				return
			end
			if Dist > R.AttackRange * 0.6 and not E:IsStunned() then
				E:AddMovementInput(self:PathDir(Pos, self.PlayerPos))
				self:Play("Move")
			else
				self:Play("Idle")
			end
			self:Face(DirP)
		else
			self:Patrol(Pos, 0.45)
		end
	end
end

function HD2DEnemy:Patrol(Pos, Speed)
	local E = self.entity
	if self.Timer <= 0 then
		self.WanderMove = self:Random() < 0.4 and Vector3(0, 0, 0) or self:WanderDir(Pos)
		self.Timer = 1.0 + self:Random() * 1.5
	end
	if self.WanderMove and self.WanderMove:Length() > 0.1 and not E:IsStunned() then
		E:AddMovementInput(self.WanderMove * Speed)
		self:Face(self.WanderMove)
		self:Play("Move")
	else
		self:Play("Idle")
	end
end

-- ---- 해골 궁수: 거리 유지 → 활시위 → 화살
function HD2DEnemy:UpdateArcher(Dt, Pos, DirP, Dist)
	local E = self.entity
	local R = self.Row
	if self.State == "Windup" then
		self:Face(DirP)
		if self.Timer <= 0 then
			local Player = self.GM:GetPlayer()
			local Aim = DirP
			if Player then
				-- 조금 앞을 내다보고 쏜다
				local V = Player.entity:GetMovementVelocity()
				local Lead = Flat(Player.entity:GetWorldPosition() + Vector3(V.X, V.Y, 0) * (Dist / R.ProjectileSpeed) * 0.5 - Pos)
				if Lead:Length() > 1 then Aim = Lead:Normalized() end
			end
			self.GM:SpawnProjectile({ Kind = "EnemyArrow", Pos = Pos + Aim * 50 + Vector3(0, 10, 15), Dir = Aim, Speed = R.ProjectileSpeed,
			                          Range = R.AttackRange * 1.4, Damage = R.AttackDamage, Team = "Enemy" })
			Audio.PlayOneShot("Audio/RPG/Swing2.wav")
			self:SetState("Attack", 0.35)
			self:Play("Attack")
		end
	elseif self.State == "Attack" then
		if self.Timer <= 0 then
			self:SetState("Idle", 0)
			self.Cooldown = R.AttackCooldown
		end
	else
		if self.bAggro then
			local Move = nil
			if Dist < 450 then Move = DirP * -1 elseif Dist > R.AttackRange * 0.8 then Move = self:PathDir(Pos, self.PlayerPos) end
			if self.Cooldown <= 0 and Dist < R.AttackRange and Dist > 200 and not E:IsStunned() then
				self:SetState("Windup", R.WindupTime)
				self:Play("Windup")
				self:Face(DirP)
				self.GM:SpawnFx("Alert", Pos + Vector3(0, 20, self.HitHeight + 70), { Blend = 2, Scale = 1.3 })
				return
			end
			if Move and not E:IsStunned() then
				E:AddMovementInput(Move * 0.8)
				self:Play("Move")
			else
				self:Play("Idle")
			end
			self:Face(DirP)
		else
			self:Patrol(Pos, 0.4)
		end
	end
end

-- ---- 독버섯: 느린 접근 → 부풀기 → 독 웅덩이
function HD2DEnemy:UpdateSpore(Dt, Pos, DirP, Dist)
	local E = self.entity
	local R = self.Row
	if self.State == "Windup" then
		if self.Timer <= 0 then
			self.GM:SpawnPoison(Vector3(Pos.X, Pos.Y, Pos.Z + self.Foot), 190, 4.5, R.AttackDamage)
			Audio.PlayOneShot("Audio/RPG/Dodge.wav")
			self:SetState("Attack", 0.45)
			self:Play("Attack")
		end
	elseif self.State == "Attack" then
		if self.Timer <= 0 then
			self:SetState("Idle", 0)
			self.Cooldown = R.AttackCooldown
		end
	else
		if self.bAggro then
			if self.Cooldown <= 0 and Dist < R.AttackRange and not E:IsStunned() then
				self:SetState("Windup", R.WindupTime)
				self:Play("Windup")
				self.GM:SpawnFx("Alert", Pos + Vector3(0, 20, self.HitHeight + 50), { Blend = 2, Scale = 1.3 })
				return
			end
			if Dist > 120 and not E:IsStunned() then
				E:AddMovementInput(self:PathDir(Pos, self.PlayerPos))
				self:Play("Move")
			else
				self:Play("Idle")
			end
		else
			self:Patrol(Pos, 0.5)
		end
	end
end

function HD2DEnemy:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
end

function HD2DEnemy:ShowBar(bShow)
	self.HpBack:GetComponent("SpriteComponent").Visible = bShow
	self.HpFill:GetComponent("SpriteComponent").Visible = bShow
end

-- 플레이어 공격 (Dir = 맞은 방향 — 수평 단위 벡터). 맞았으면 true
function HD2DEnemy:TakeHit(Damage, Dir, Knockback)
	if self.bDead then return false end
	self.Health = self.Health - Damage
	self.Flash = 0.1
	self.BarTime = 3.0
	self:ShowBar(true)
	self.HpFill:SetScale(Vector3(math.max(0.0, self.Health / self.Row.MaxHealth), 1, 1))
	-- 예비 동작은 끊긴다 (경직), 슬라임은 뛰다 떨어진다
	if self.State == "Windup" or self.State == "Hop" then
		self.Body:SetPosition(self.BodyBase)
		self:SetState("Idle", 0.5)
		self:Play("Idle")
		self.Cooldown = math.max(self.Cooldown, 0.8)
	end
	self.entity:AddKnockback(Dir * (Knockback or 700), 0.22)
	if self.Health <= 0 then
		self.bDead = true
		local P = self.entity:GetWorldPosition()
		self.GM:SpawnFx("Poof", P + Vector3(0, 10, self.Foot + 30), { Scale = 1.3 })
		-- 지금 그려지는 프레임(플립북 반영)으로 떠오르며 사라지는 하얀 잔상
		local Slice, Atlas = self.Body:GetSpriteSlice()
		if Slice then
			self.GM:SpawnAfterimage(Atlas, Slice, self.Body:GetWorldPosition() + Vector3(0, -2, 0), self.Sprite.FlipX, { 1, 0.95, 0.9, 0.7 }, 0.4)
		end
		self.GM:SpawnFx("Sparkle", P + Vector3(0, 20, self.HitHeight), { Blend = 2, Scale = 1.1, Color = { 1, 0.9, 0.7, 1 } })
		self.GM:OnEnemyKilled(self)
		self.entity:Destroy()
	end
	return true
end

return HD2DEnemy
