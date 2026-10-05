-- HD-2D 데모 동굴 보스 "수정 거미 여왕" (Prefabs/Demo/HD2D/SpiderQueen.eprefab — 큰 캡슐 이동기 + Visual > Body·Shadow, 수치 = Enemies.etable "SpiderQueen").
--   잠듦(다리를 웅크린 수정 덩어리 — 맞지 않음) → 플레이어가 발견 거리 안에 오면 깨어남(알림 + 흔들림 + HUD 보스 체력바) → 쫓으며 패턴을 돌린다:
--     Web     앞다리를 치켜든 뒤 거미줄 탄을 부채꼴로 (1단계 3발, 2단계 5발 — 관리자 투사체 Web)
--     Shards  플레이어 자리와 둘레 바닥에 푸른 경고 원 → 잠시 뒤 수정 가시가 솟는다 (관리자 SpawnEruption — 1단계 3곳, 2단계 6곳)
--     Pounce  웅크려 착지점(붉은 원)을 노린 뒤 높이 뛰어 덮친다 — 날아가는 동안 잔상(entity:GetSpriteSlice), 착지 충격파 원 안이면 피해
--   체력 절반에서 격노(2단계): 보랏빛, 예비 동작·간격이 짧아지고 수정 슬라임 둘을 부른다(Summon). 맞으면 하얗게 덮는다(SpriteComponent.FlashColor).
--   숨 고르기: 패턴마다 끝난 뒤 회복(Recover) 상태 + 다음 패턴까지 쿨다운(회복 중에도 흐름) → 패턴 사이 최소 약 2초(2단계 1.6초). 패턴 순서는
--     Cycle1/Cycle2 그대로 — 덮치기 차례에 너무 가까우면(BackstepRange) 뒤로 크게 뛰어 거리를 벌린 뒤 덮친다 (붙어 싸워도 세 패턴이 다 나온다).
--   효과음은 자리 있는 Audio.PlayOneShot(경로, 위치, 음량, 피치) — 멀면 작게 들린다.
--   쓰러지면 관리자 OnEnemyKilled(전리품·경험치) + OnBossKilled(보상 상자·문 열기·퀘스트 단계) 후 큰 연출과 함께 사라진다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DSpiderQueen = {
	Properties = {
		Kind = "SpiderQueen",
	},
}

local Book = "Sprites/HD2D/SpiderQueen_"
local Cycle1 = { "Web", "Pounce", "Shards" }
local Cycle2 = { "Shards", "Pounce", "Web", "Pounce", "Shards", "Web" }
local BackstepRange = 380  -- 덮치기 차례인데 이보다 가까우면 먼저 뒤로 뛴다
local RecoverTime = { Web = 0.9, Shards = 1.0, Pounce = 0.9 }  -- 패턴 뒤 숨 고르기 (초, 2단계 × 0.8)

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function HD2DSpiderQueen:OnStart()
	self.Kind = self.Properties.Kind
	self.Row = D.Enemy(self.Kind)
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.BodyBase = self.Body:GetPosition()
	self.Foot = self.BodyBase.Z
	self.Radius = self.Row.Radius
	self.HitHeight = 170
	self.Health = self.Row.MaxHealth
	self.bBoss = true
	self.bDormant = true
	self.bDead = false
	self.Phase = 1
	self.State, self.Timer, self.Cooldown = "Dormant", 0, 1.2
	self.Flash, self.ContactCooldown, self.Time = 0, 0, 0
	self.PatternIndex = 0
	self.Anim = ""
	self.GM:RegisterEnemy(self)
	self:Play("Dormant")
	self.GM:InitEnemyCombat(self) -- 약점·실드(2단계에 바뀜)·상태 이상 (HD2DCombat.lua)
end

function HD2DSpiderQueen:OnDestroy()
	if self.GM then self.GM:UnregisterEnemy(self) end
end

function HD2DSpiderQueen:Play(Anim)
	if Anim ~= self.Anim then
		self.Anim = Anim
		self.Body:PlayFlipbook(Book .. Anim .. ".eflipbook")
	end
end

function HD2DSpiderQueen:SetState(State, Time)
	self.State, self.Timer = State, Time or 0
end

function HD2DSpiderQueen:Count(Pattern)
	local P = self.GM.Report.BossPatterns
	P[Pattern] = (P[Pattern] or 0) + 1
	Log.Info("[HD2D] 보스 패턴: " .. Pattern)
end

function HD2DSpiderQueen:Windup()
	return self.Row.WindupTime * (self.Phase == 2 and 0.72 or 1.0)
end

function HD2DSpiderQueen:Sound(Path, Volume, Pitch)
	Audio.PlayOneShot(Path, self.entity:GetWorldPosition(), Volume or 1.0, Pitch or 1.0)
end

function HD2DSpiderQueen:BaseColor()
	-- 동굴 조명에서 어둡게 묻히지 않게 조금 밝게, 격노하면 보랏빛
	return self.Phase == 2 and Vector4(1.35, 0.95, 1.5, 1) or Vector4(1.15, 1.1, 1.2, 1)
end

function HD2DSpiderQueen:OnUpdate(Dt)
	if self.bDead or Dt <= 0 then return end
	local bSkip
	bSkip, Dt = self.GM:UpdateEnemyCombat(self, Dt) -- 브레이크·기절이면 건너뜀, 빙결이면 느리게
	if bSkip then return end
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
			self:SetState("Wake", 1.3)
			self:Play("Rear")
			self.GM:AddShake(14, 0.6)
			for _, Side in ipairs({ -1, 1 }) do
				self.GM:SpawnFx("Poof", Ground + Vector3(Side * 160, 30, 20), { Scale = 2.0, Color = { 0.75, 0.85, 1, 1 } })
			end
			self.GM:SpawnFx("Sparkle", Ground + Vector3(0, 30, 260), { Blend = 2, Scale = 2.4, Color = { 0.6, 0.9, 1, 1 } })
			self.GM:Hud():Announce(R.DisplayName, "유적의 주인이 눈을 떴다!", 2.5)
			self.GM:Hud():ShowBoss(R.DisplayName, 1.0)
			self:Sound("Audio/RPG/HitHeavy.wav", 1.0, 0.6)
			Log.Info("[HD2D] 보스 깨어남: " .. R.DisplayName)
		end
		return
	end

	if self.State ~= "Dormant" and self.State ~= "Wake" then self.Cooldown = self.Cooldown - Dt end -- 회복 중에도 흐른다
	if self.State == "Wake" or self.State == "Recover" then
		if self.Anim ~= "Rear" or self.State == "Recover" then self:Play("Idle") end
		if self.Timer <= 0 then self:SetState("Chase", 0) end
	elseif self.State == "Backstep" then
		-- 덮치기 전에 뒤로 크게 뛰어 거리를 벌린다 (몸이 낮게 떴다 내려앉음)
		local T = 1.0 - math.max(self.Timer, 0) / 0.45
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, math.sin(T * math.pi) * 90))
		if self.Timer <= 0 then
			self.Body:SetPosition(self.BodyBase)
			self.GM:SpawnFx("Dust", Ground + Vector3(0, 30, 2), { Scale = 1.6 })
			self:BeginPounce(Player, Ground)
		end
	elseif self.State == "Chase" then
		if self.Cooldown <= 0 and Dist < 1400 then
			self:StartPattern(Dist, DirP, Ground, Player)
		elseif Dist > 340 and not E:IsStunned() then
			E:AddMovementInput(DirP * (self.Phase == 2 and 1.0 or 0.75))
			self:Play("Walk")
		else
			self:Play("Idle")
		end
	elseif self.State == "WebWindup" then
		if self.Timer <= 0 then
			self:Play("Spit")
			local Count = self.Phase == 2 and 5 or 3
			local Spread = math.rad(self.Phase == 2 and 13 or 16)
			for I = 1, Count do
				local A = (I - (Count + 1) * 0.5) * Spread
				local Dir = Vector3(DirP.X * math.cos(A) - DirP.Y * math.sin(A), DirP.X * math.sin(A) + DirP.Y * math.cos(A), 0)
				self.GM:SpawnProjectile({ Kind = "Web", Pos = Ground + Dir * 110 + Vector3(0, 10, 120), Dir = Dir, Speed = R.ProjectileSpeed,
				                          Range = 1400, Damage = R.AttackDamage * 0.7, Team = "Enemy", HitOpt = { Status = "Freeze", Chance = 1.0 } })
			end
			self:Sound("Audio/RPG/Swing2.wav", 1.0, 0.7)
			self:EndPattern("Web")
		end
	elseif self.State == "ShardsWindup" then
		if self.Timer <= 0 then
			self:Play("Idle")
			local PP = Player and Player.entity:GetWorldPosition() or Pos
			local Center = Vector3(PP.X, PP.Y, Ground.Z)
			local Count = self.Phase == 2 and 6 or 3
			for I = 1, Count do
				local Offset = Vector3(0, 0, 0)
				if I > 1 then
					local A = (I - 2) / (Count - 1) * math.pi * 2 + self.Time
					Offset = Vector3(math.cos(A), math.sin(A), 0) * (self.Phase == 2 and 260 or 230)
				end
				self.GM:SpawnEruption(Center + Offset, 0.85 + (I - 1) * 0.1, 115, R.AttackDamage, { Status = "Freeze", Chance = 0.5 })
			end
			self:Sound("Audio/RPG/Spin.wav", 0.9, 0.6)
			self:EndPattern("Shards")
		end
	elseif self.State == "PounceWindup" then
		local Blink = math.floor(self.Time * 14) % 2 == 0
		self.Sprite.Color = Blink and Vector4(1.7, 0.75, 0.8, 1) or self:BaseColor()
		if self.Timer <= 0 then
			local To = Flat(self.PounceTarget - Pos)
			local L = To:Length()
			self.PounceDir = L > 1 and To * (1.0 / L) or DirP
			self:SetState("Pounce", 0.6)
			self:Play("Leap")
			E:AddKnockback(self.PounceDir * (L / 0.6), 0.58)
			self.AfterTimer = 0
			self:Sound("Audio/RPG/Dash.wav", 1.0, 0.75)
		end
	elseif self.State == "Pounce" then
		-- 몸이 포물선으로 떠올랐다가 내려앉는다 + 잔상 (지금 그려지는 프레임을 그대로 복사)
		local T = 1.0 - math.max(self.Timer, 0) / 0.6
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, math.sin(T * math.pi) * 240))
		self.AfterTimer = self.AfterTimer - Dt
		if self.AfterTimer <= 0 then
			self.AfterTimer = 0.05
			local Slice, Atlas = self.Body:GetSpriteSlice()
			if Slice then
				self.GM:SpawnAfterimage(Atlas, Slice, self.Body:GetWorldPosition() + Vector3(0, -3, 0), false, { 0.75, 0.6, 1.0, 0.5 }, 0.3)
				self.GM.Report.Afterimages = self.GM.Report.Afterimages + 1
			end
		end
		if self.Timer <= 0 then
			self.Body:SetPosition(self.BodyBase)
			self.GM:KillFx(self.Warn)
			self:Play("Idle")
			self.GM:AddShake(15, 0.35)
			self.GM:SpawnFx("Ring", Ground + Vector3(0, 0, 4), { Flat = true, Blend = 2, Scale = 0.7, Grow = 5.5, Life = 0.4, Fade = true,
			                                                    Color = { 0.75, 0.85, 1, 1 } })
			for I = -2, 2 do
				self.GM:SpawnFx("Dust", Ground + Vector3(I * 80, 40, 2), { Scale = 1.8, FlipX = I < 0 })
			end
			self:Sound("Audio/RPG/HitHeavy.wav", 1.0, 0.8)
			local PP = Player and Player.entity:GetWorldPosition()
			if PP and Flat(PP - Ground):Length() < 240 then
				Player:TakeDamage(R.AttackDamage * 1.2, Pos, self.HitOpt) -- 착지 = 기절 (표 Inflict)
			end
			self:EndPattern("Pounce")
		end
	end

	-- 몸 접촉 (덮치는 중이면 피하지 않으면 그대로 맞는다 — 착지 판정이 따로 있어 접촉은 평소 값)
	if Player and Dist < self.Radius + 45 and self.ContactCooldown <= 0 and self.State ~= "Pounce" then
		if Player:TakeDamage(R.ContactDamage, Pos) then self.ContactCooldown = 1.0 end
	end

	if self.Flash > 0 then
		self.Flash = self.Flash - Dt
		self.Sprite.FlashColor = Vector4(1, 1, 1, self.Flash > 0 and math.min(0.85, self.Flash * 9) or 0)
	end
	if self.State ~= "PounceWindup" then self.Sprite.Color = self:BaseColor() end
end

-- 패턴 끝: 숨 고르기(회복) + 다음 패턴까지 쿨다운 (회복 중에도 흐른다 — 둘 중 긴 쪽이 실제 간격)
function HD2DSpiderQueen:EndPattern(Pattern)
	local Scale = self.Phase == 2 and 0.8 or 1.0
	self:SetState("Recover", RecoverTime[Pattern] * Scale)
	self.Cooldown = self.Row.AttackCooldown * (self.Phase == 2 and 0.85 or 1.0) + RecoverTime[Pattern] * Scale
end

-- 덮치기 예고: 착지점 = 플레이어 자리 (보스 방 안으로 — 너무 멀면 줄인다)
function HD2DSpiderQueen:BeginPounce(Player, Ground)
	local PP = Player and Player.entity:GetWorldPosition() or Ground
	local To = Flat(PP - Ground)
	if To:Length() > 900 then To = To:Normalized() * 900 end
	self.PounceTarget = Vector3(Ground.X + To.X, Ground.Y + To.Y, Ground.Z)
	self:SetState("PounceWindup", self:Windup())
	self:Play("Crouch")
	self.Warn = self.GM:SpawnSprite({ Sprite = "Sprites/HD2D/Fx.esprite", Slice = "Warn", Position = self.PounceTarget + Vector3(0, 0, 3), Flat = true,
	                                  Blend = 0, Life = self:Windup() + 0.7, Scale = 240 / 108.0, Color = { 1, 1, 1, 0.9 } })
end

function HD2DSpiderQueen:StartPattern(Dist, DirP, Ground, Player)
	local R = self.Row
	local Cycle = self.Phase == 2 and Cycle2 or Cycle1
	self.PatternIndex = self.PatternIndex % #Cycle + 1
	local Pattern = Cycle[self.PatternIndex]
	self.Cooldown = 99 -- 패턴이 끝날 때 EndPattern이 다시 정한다
	if Pattern == "Pounce" and Dist < BackstepRange then
		-- 붙어 있으면 뒤로 크게 뛰어 거리를 벌린 뒤 덮친다
		self:SetState("Backstep", 0.45)
		self:Play("Leap")
		self.entity:AddKnockback(DirP * -(560 / 0.45), 0.43)
		self:Sound("Audio/RPG/Dash.wav", 0.8, 1.1)
		self:Count("Pounce")
		return
	end
	if Pattern == "Web" then
		self:SetState("WebWindup", self:Windup() * 0.8)
		self:Play("Rear")
		self.GM:SpawnFx("Alert", Ground + Vector3(0, 30, 360), { Blend = 2, Scale = 2.0 })
	elseif Pattern == "Shards" then
		self:SetState("ShardsWindup", 0.45)
		self:Play("Rear")
		self.GM:SpawnFx("Sparkle", Ground + Vector3(0, 30, 300), { Blend = 2, Scale = 2.0, Color = { 0.6, 0.9, 1, 1 } })
	else
		self:BeginPounce(Player, Ground)
	end
	self:Count(Pattern)
end

function HD2DSpiderQueen:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
end

function HD2DSpiderQueen:TakeHit(Damage, Dir, Knockback)
	if self.bDead or self.bDormant then return false end
	self.Health = self.Health - Damage
	self.Flash = 0.09
	if self.State ~= "Pounce" then self.entity:AddKnockback(Dir * (Knockback or 0) * 0.06, 0.05) end
	self.GM:Hud():ShowBoss(self.Row.DisplayName, self.Health / self.Row.MaxHealth)
	if self.Phase == 1 and self.Health <= self.Row.MaxHealth * 0.5 then
		self.Phase = 2
		self.PatternIndex = 0
		local P = self.entity:GetWorldPosition()
		self.GM:Hud():Announce("여왕이 격노했다!", "등의 수정이 보랏빛으로 타오른다", 2.0)
		self.GM:AddShake(12, 0.4)
		self.GM:Hud():ScreenFlash(0.3, 0.25)
		for _, Side in ipairs({ -1, 1 }) do
			self.GM:SpawnEnemy("CrystalSlime", Vector3(P.X + Side * 280, P.Y + 140, P.Z + self.Foot + 46))
		end
		self:Count("Summon")
		Log.Info("[HD2D] 보스 2단계 (격노)")
	end
	if self.Health <= 0 then
		self.bDead = true
		local P = self.entity:GetWorldPosition()
		local Ground = Vector3(P.X, P.Y, P.Z + self.Foot)
		self.GM:KillFx(self.Warn)
		self.Body:SetPosition(self.BodyBase)
		local Slice, Atlas = self.Body:GetSpriteSlice()
		if Slice then
			self.GM:SpawnAfterimage(Atlas, Slice, self.Body:GetWorldPosition() + Vector3(0, -3, 0), false, { 1, 0.95, 1, 0.8 }, 0.8)
		end
		for I = 0, 4 do
			self.GM:SpawnFx("Poof", Ground + Vector3((I - 2) * 90, 40, 50 + (I % 2) * 110), { Scale = 2.4, Color = { 0.8, 0.85, 1, 1 } })
		end
		for I = 0, 5 do
			local A = I / 6 * math.pi * 2
			self.GM:SpawnFx("Sparkle", Ground + Vector3(math.cos(A) * 160, 30, 120 + math.sin(A) * 80), { Blend = 2, Scale = 1.6, Color = { 0.6, 0.9, 1, 1 } })
		end
		self.GM:SpawnFx("Burst", Ground + Vector3(0, 50, 170), { Blend = 2, Scale = 4.0, Color = { 0.7, 0.85, 1, 1 } })
		self.GM:AddShake(20, 0.7)
		Game.HitStop(0.25)
		self:Sound("Audio/RPG/HitHeavy.wav", 1.0, 0.55)
		self.GM:OnEnemyKilled(self)
		self.GM:OnBossKilled(self)
		Log.Info("[HD2D] 보스 처치: " .. self.Row.DisplayName)
		self.entity:Destroy()
	end
	return true
end

return HD2DSpiderQueen
