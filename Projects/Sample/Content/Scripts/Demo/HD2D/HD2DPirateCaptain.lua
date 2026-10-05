-- HD-2D 데모 항구 보스 "해적 선장 바렌" (Prefabs/Demo/HD2D/PirateCaptain.eprefab — 큰 캡슐 이동기 + Visual > Body·Shadow, 수치 = Enemies.etable "PirateCaptain").
--   잠듦(팔짱 끼고 졸고 있음 — 맞지 않음) → 플레이어가 발견 거리 안에 오면 깨어남(알림 + 흔들림 + HUD 보스 체력바) → 쫓으며 패턴을 돌린다:
--     Lunge   칼을 치켜들고(바닥에 붉은 길 경고) 플레이어 쪽으로 돌진 베기 — 지나가는 길 위면 피해, 잔상 (상태 이름 ChargeWindup — 자동 조종 회피 규약)
--     Volley  권총을 겨눠(총구 반짝) 부채꼴로 총알 3발 (2단계 5발 두 번) — 관리자 투사체 EnemyArrow, 그림만 총알
--     Keg     화약통을 던진다 — 착지점 붉은 원 경고 뒤 폭발 (관리자 투사체 Rock, 그림만 화약통). 2단계는 셋 (플레이어 자리 + 둘레)
--     Barrage 2단계 전용: 앞바다 해적선이 포격 — 플레이어 둘레 다섯 곳 경고 원에 포탄이 차례로 떨어진다
--   체력 절반에서 격노(2단계): 붉게 달아오르고 예비 동작·간격이 짧아지며 해적 졸개 둘을 부른다(Summon).
--   그림은 옆모습(오른쪽) — 플레이어가 왼쪽이면 좌우 반전. 쓰러지면 관리자 OnEnemyKilled + OnBossKilled 후 큰 연출과 함께 사라진다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DPirateCaptain = {
	Properties = {
		Kind = "PirateCaptain",
	},
}

local Book = "Sprites/HD2D/PirateCaptain_"
local FxAtlas = "Sprites/HD2D/HarborFx.esprite"
local Cycle1 = { "Lunge", "Volley", "Keg" }
local Cycle2 = { "Barrage", "Lunge", "Volley", "Keg", "Lunge", "Volley" }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function HD2DPirateCaptain:OnStart()
	self.Kind = self.Properties.Kind
	self.Row = D.Enemy(self.Kind)
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.BodyBase = self.Body:GetPosition()
	self.Foot = self.BodyBase.Z
	self.Radius = self.Row.Radius
	self.HitHeight = 190
	self.Health = self.Row.MaxHealth
	self.bBoss = true
	self.bDormant = true
	self.bDead = false
	self.Phase = 1
	self.State, self.Timer, self.Cooldown = "Dormant", 0, 1.0
	self.Flash, self.ContactCooldown, self.Time = 0, 0, 0
	self.PatternIndex = 0
	self.Anim = ""
	self.FlipX = false
	self.GM:RegisterEnemy(self)
	self:Play("Dormant")
end

function HD2DPirateCaptain:OnDestroy()
	if self.GM then self.GM:UnregisterEnemy(self) end
end

function HD2DPirateCaptain:Play(Anim)
	if Anim ~= self.Anim then
		self.Anim = Anim
		self.Body:PlayFlipbook(Book .. Anim .. ".eflipbook")
	end
end

function HD2DPirateCaptain:SetState(State, Time)
	self.State, self.Timer = State, Time or 0
end

function HD2DPirateCaptain:Count(Pattern)
	local P = self.GM.Report.BossPatterns
	P[Pattern] = (P[Pattern] or 0) + 1
	Log.Info("[HD2D] 보스 패턴: " .. Pattern)
end

function HD2DPirateCaptain:Windup()
	return self.Row.WindupTime * (self.Phase == 2 and 0.72 or 1.0)
end

function HD2DPirateCaptain:Sound(Path, Volume, Pitch)
	Audio.PlayOneShot(Path, self.entity:GetWorldPosition(), Volume or 1.0, Pitch or 1.0)
end

function HD2DPirateCaptain:Face(Dir)
	if Dir.X < -0.1 then self.FlipX = true elseif Dir.X > 0.1 then self.FlipX = false end
	self.Body:SetSpriteFlip(self.FlipX, false)
end

function HD2DPirateCaptain:BaseColor()
	return self.Phase == 2 and Vector4(1.3, 0.92, 0.85, 1) or Vector4(1.05, 1.02, 1.0, 1)
end

-- 관리자 투사체를 만들고 그림만 바꾼다 (총알·화약통·포탄)
function HD2DPirateCaptain:Reskin(P, Slice, Flipbook, Scale)
	self.GM:KillFx(P.Fx)
	P.Fx = self.GM:SpawnSprite({ Sprite = FxAtlas, Slice = Slice, Flipbook = Flipbook, Position = P.Pos or P.From, Blend = 3, Lit = Slice ~= "Bullet",
	                             Life = (P.Duration or 3.0) + 0.3, Scale = Scale or 1.4,
	                             Rotation = (Slice == "Bullet" and P.Dir) and self.GM.ScreenAngle(P.Dir) or nil })
end

function HD2DPirateCaptain:OnUpdate(Dt)
	if self.bDead or Dt <= 0 then return end
	local E = self.entity
	local R = self.Row
	local Pos = E:GetWorldPosition()
	local Ground = Vector3(Pos.X, Pos.Y, Pos.Z + self.Foot)
	self.Time = self.Time + Dt
	self.Timer = self.Timer - Dt
	self.ContactCooldown = math.max(0, self.ContactCooldown - Dt)
	local Player = self.GM:GetPlayer()
	local ToPlayer, Dist = Vector3(0, 1, 0), 1.0e9
	if Player and not Player.bDead then
		ToPlayer = Flat(Player.entity:GetWorldPosition() - Pos)
		Dist = ToPlayer:Length()
	end
	local DirP = Dist > 1 and ToPlayer * (1.0 / Dist) or Vector3(0, 1, 0)

	if self.State == "Dormant" then
		if Dist < R.AggroRange then
			self.bDormant = false
			self:SetState("Wake", 1.4)
			self:Play("Raise")
			self:Face(DirP)
			self.GM:AddShake(12, 0.5)
			self.GM:SpawnFx("Alert", Ground + Vector3(0, 30, 300), { Blend = 2, Scale = 2.2 })
			self.GM:Hud():Announce(R.DisplayName, "\"내 배를 노리는 놈이 또 왔군!\"", 2.6)
			self.GM:Hud():ShowBoss(R.DisplayName, 1.0)
			self:Sound("Audio/RPG/Swing3.wav", 1.0, 0.7)
			Log.Info("[HD2D] 보스 깨어남: " .. R.DisplayName)
		end
		return
	end

	if self.State == "Wake" or self.State == "Recover" then
		if self.Timer <= 0 then self:SetState("Chase", 0) end
		if self.State ~= "Wake" then self:Play("Idle") end
		self:Face(DirP)
	elseif self.State == "Chase" then
		self.Cooldown = self.Cooldown - Dt
		self:Face(DirP)
		if self.Cooldown <= 0 and Dist < 1400 then
			self:StartPattern(Dist, DirP, Ground, Player)
		elseif Dist > 380 and not E:IsStunned() then
			E:AddMovementInput(DirP * (self.Phase == 2 and 1.0 or 0.8))
			self:Play("Walk")
		else
			self:Play("Idle")
		end
	elseif self.State == "ChargeWindup" then
		-- 칼 돌진 예고: 붉게 깜빡이며 칼을 치켜듦
		local Blink = math.floor(self.Time * 14) % 2 == 0
		self.Sprite.Color = Blink and Vector4(1.7, 0.8, 0.7, 1) or self:BaseColor()
		if self.Timer <= 0 then
			self:SetState("Charge", 0.42)
			self:Play("Slash")
			E:AddKnockback(self.LungeDir * 1500, 0.4)
			self.AfterTimer, self.bLungeHit = 0, false
			self:Sound("Audio/RPG/Dash.wav", 1.0, 0.8)
		end
	elseif self.State == "Charge" then
		self.AfterTimer = self.AfterTimer - Dt
		if self.AfterTimer <= 0 then
			self.AfterTimer = 0.08
			local Slice, Atlas = self.Body:GetSpriteSlice()
			if Slice then
				self.GM:SpawnAfterimage(Atlas, Slice, self.Body:GetWorldPosition() + Vector3(0, -6, 0), self.FlipX, { 0.9, 0.35, 0.3, 0.32 }, 0.2)
				self.GM.Report.Afterimages = self.GM.Report.Afterimages + 1
			end
		end
		if Player and not self.bLungeHit and Dist < self.Radius + 90 then
			self.bLungeHit = Player:TakeDamage(R.AttackDamage * 1.25, Pos) or self.bLungeHit
		end
		if self.Timer <= 0 then
			self.GM:KillFx(self.Warn)
			self:Play("Idle")
			self:SetState("Recover", 0.7)
			self.GM:AddShake(6, 0.15)
		end
	elseif self.State == "VolleyWindup" then
		if self.Timer <= 0 then
			self:Play("Aim")
			local Count = self.Phase == 2 and 5 or 3
			local Spread = math.rad(self.Phase == 2 and 12 or 15)
			local Aim = DirP
			for I = 1, Count do
				local A = (I - (Count + 1) * 0.5) * Spread
				local Dir = Vector3(Aim.X * math.cos(A) - Aim.Y * math.sin(A), Aim.X * math.sin(A) + Aim.Y * math.cos(A), 0)
				local P = self.GM:SpawnProjectile({ Kind = "EnemyArrow", Pos = Ground + Dir * 120 + Vector3(0, 10, 150), Dir = Dir, Speed = R.ProjectileSpeed,
				                                    Range = 1500, Damage = R.AttackDamage * 0.6, Team = "Enemy" })
				self:Reskin(P, "Bullet", nil, 1.5)
			end
			self.GM:SpawnSprite({ Sprite = FxAtlas, Flipbook = "Sprites/HD2D/HarborFx_Muzzle.eflipbook", Position = Ground + DirP * 140 + Vector3(0, 12, 152),
			                      Blend = 2, Life = 0.16, Scale = 1.8 })
			self:Sound("Audio/RPG/HitHeavy.wav", 0.8, 1.6)
			self.Volleys = (self.Volleys or 1) - 1
			if self.Volleys > 0 then
				self:SetState("VolleyWindup", 0.45)
			else
				self:SetState("Recover", 0.8)
			end
		end
	elseif self.State == "KegWindup" then
		if self.Timer <= 0 then
			self:Play("Idle")
			local PP = Player and Player.entity:GetWorldPosition() or Pos
			local Center = Vector3(PP.X, PP.Y, Ground.Z)
			local Count = self.Phase == 2 and 3 or 1
			for I = 1, Count do
				local Off = Vector3(0, 0, 0)
				if I > 1 then
					local A = (I - 2) * math.pi + self.Time
					Off = Vector3(math.cos(A), math.sin(A), 0) * 230
				end
				local P = self.GM:SpawnProjectile({ Kind = "Rock", Pos = Ground + Vector3(0, 10, 230), Target = Center + Off, Duration = 0.95 + (I - 1) * 0.12,
				                                    Height = 380, Damage = R.AttackDamage, Team = "Enemy" })
				self:Reskin(P, "", "Sprites/HD2D/HarborFx_Keg.eflipbook", 1.6)
			end
			self:Sound("Audio/RPG/Swing2.wav", 1.0, 0.7)
			self:SetState("Recover", 0.9)
		end
	elseif self.State == "BarrageWindup" then
		if self.Timer <= 0 then
			self:Play("Idle")
			local PP = Player and Player.entity:GetWorldPosition() or Pos
			for I = 1, 5 do
				local A = I / 5 * math.pi * 2 + self.Time
				local Off = I == 1 and Vector3(0, 0, 0) or Vector3(math.cos(A), math.sin(A), 0) * 260
				local Target = Vector3(PP.X + Off.X, PP.Y + Off.Y, Ground.Z)
				local P = self.GM:SpawnProjectile({ Kind = "Rock", Pos = Target + Vector3(500, 1600, 900), Target = Target, Duration = 1.0 + I * 0.18,
				                                    Height = 250, Damage = R.AttackDamage * 1.1, Team = "Enemy" })
				self:Reskin(P, "Cannonball", nil, 1.6)
			end
			self:Sound("Audio/RPG/HitHeavy.wav", 1.0, 0.5)
			self.GM:AddShake(5, 0.2)
			self:SetState("Recover", 1.3)
		end
	end

	-- 몸 접촉
	if Player and Dist < self.Radius + 45 and self.ContactCooldown <= 0 and self.State ~= "Charge" then
		if Player:TakeDamage(R.ContactDamage, Pos) then self.ContactCooldown = 1.0 end
	end
	if self.Flash > 0 then
		self.Flash = self.Flash - Dt
		-- 피격 번쩍임은 짧고 반만 (자주 맞아도 몸이 하얗게 묻히지 않게 — 맞았다는 신호만)
		self.Sprite.FlashColor = Vector4(1, 0.95, 0.85, self.Flash > 0 and math.min(0.5, self.Flash * 9) or 0)
	end
	if self.State ~= "ChargeWindup" then self.Sprite.Color = self:BaseColor() end
end

function HD2DPirateCaptain:StartPattern(Dist, DirP, Ground, Player)
	local Cycle = self.Phase == 2 and Cycle2 or Cycle1
	self.PatternIndex = self.PatternIndex % #Cycle + 1
	local Pattern = Cycle[self.PatternIndex]
	if Pattern == "Lunge" and Dist > 1100 then Pattern = "Volley" end -- 너무 멀면 쏜다
	if Pattern == "Lunge" then
		self.LungeDir = DirP
		self:SetState("ChargeWindup", self:Windup())
		self:Play("Raise")
		-- 바닥 경고 (돌진 길 위 붉은 원 셋)
		self.Warn = nil
		for I = 1, 3 do
			local W = self.GM:SpawnSprite({ Sprite = "Sprites/HD2D/Fx.esprite", Slice = "Warn", Position = Ground + DirP * (I * 210) + Vector3(0, 0, 3), Flat = true,
			                                Blend = 0, Life = self:Windup() + 0.45, Scale = 1.1, Color = { 1, 1, 1, 0.85 } })
			self.Warn = self.Warn or W
		end
		self.GM:SpawnFx("Alert", Ground + Vector3(0, 30, 320), { Blend = 2, Scale = 2.0 })
	elseif Pattern == "Volley" then
		self.Volleys = self.Phase == 2 and 2 or 1
		self:SetState("VolleyWindup", self:Windup())
		self:Play("Aim")
		self.GM:SpawnFx("Sparkle", Ground + DirP * 120 + Vector3(0, 30, 160), { Blend = 2, Scale = 1.2, Color = { 1, 0.9, 0.6, 1 } })
	elseif Pattern == "Keg" then
		self:SetState("KegWindup", self:Windup() * 0.9)
		self:Play("Throw")
	else
		self:SetState("BarrageWindup", 0.6)
		self:Play("Raise")
		self.GM:Hud():Announce("포격이다!", "앞바다 해적선이 대포를 쏜다", 1.6)
	end
	self:Count(Pattern)
	self.Cooldown = self.Row.AttackCooldown * (self.Phase == 2 and 0.7 or 1.0)
end

function HD2DPirateCaptain:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
end

function HD2DPirateCaptain:TakeHit(Damage, Dir, Knockback)
	if self.bDead or self.bDormant then return false end
	self.Health = self.Health - Damage
	self.Flash = 0.06
	if self.State ~= "Charge" then self.entity:AddKnockback(Dir * (Knockback or 0) * 0.08, 0.06) end
	self.GM:Hud():ShowBoss(self.Row.DisplayName, self.Health / self.Row.MaxHealth)
	if self.Phase == 1 and self.Health <= self.Row.MaxHealth * 0.5 then
		self.Phase = 2
		self.PatternIndex = 0
		local P = self.entity:GetWorldPosition()
		self.GM:Hud():Announce("선장이 격노했다!", "\"얘들아, 쓸어버려라!\"", 2.0)
		self.GM:AddShake(12, 0.4)
		self.GM:Hud():ScreenFlash(0.3, 0.25)
		for _, Side in ipairs({ -1, 1 }) do
			self.GM:SpawnEnemy("Pirate", Vector3(P.X + Side * 300, P.Y + 160, P.Z + self.Foot + 74))
		end
		self:Count("Summon")
		Log.Info("[HD2D] 보스 2단계 (격노)")
	end
	if self.Health <= 0 then
		self.bDead = true
		local P = self.entity:GetWorldPosition()
		local Ground = Vector3(P.X, P.Y, P.Z + self.Foot)
		self.GM:KillFx(self.Warn)
		local Slice, Atlas = self.Body:GetSpriteSlice()
		if Slice then
			self.GM:SpawnAfterimage(Atlas, Slice, self.Body:GetWorldPosition() + Vector3(0, -3, 0), self.FlipX, { 1, 0.95, 0.9, 0.8 }, 0.8)
		end
		for I = 0, 4 do
			self.GM:SpawnFx("Poof", Ground + Vector3((I - 2) * 80, 40, 50 + (I % 2) * 100), { Scale = 2.2 })
		end
		self.GM:SpawnFx("Burst", Ground + Vector3(0, 50, 150), { Blend = 2, Scale = 3.6, Color = { 1, 0.85, 0.6, 1 } })
		self.GM:AddShake(18, 0.6)
		Game.HitStop(0.25)
		self:Sound("Audio/RPG/HitHeavy.wav", 1.0, 0.55)
		self.GM:OnEnemyKilled(self)
		self.GM:OnBossKilled(self)
		Log.Info("[HD2D] 보스 처치: " .. self.Row.DisplayName)
		self.entity:Destroy()
	end
	return true
end

return HD2DPirateCaptain
