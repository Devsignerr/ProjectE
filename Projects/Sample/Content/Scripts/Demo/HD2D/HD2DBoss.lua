-- HD-2D 데모 보스 "고대의 바위 골렘" (Prefabs/Demo/HD2D/Golem.eprefab — 큰 캡슐 이동기 + Visual > Body·Shadow, 수치 = Enemies.etable "Golem").
--   잠듦(웅크린 바위 — 맞지 않음) → 플레이어가 발견 거리 안에 오면 깨어남(큰 알림 + 흔들림 + HUD 보스 체력바) → 쫓으며 패턴:
--     Slam   가까우면: 두 팔을 치켜들고(바닥에 붉은 경고 원) 내리쳐 충격파 — 원 안이면 피해
--     Throw  멀면: 바위를 플레이어 자리(2단계는 5개 — 둘레에도)로 던진다 — 착지점 경고 원, 착지 폭발 (관리자 투사체 Rock)
--     Charge 2단계(체력 절반 이하): 예고 뒤 몸통 돌진 — 닿으면 큰 피해
--   체력 절반에서 분노(붉은 빛, 예비 동작 짧아짐) + 슬라임 둘 소환. 넉백은 거의 받지 않는다.
--   숨 고르기: 패턴마다 끝난 뒤 회복(Recover) + 다음 패턴까지 쿨다운(회복 중에도 흐름). 붙어 싸워도 내리치기만 반복하지 않게
--     내리치기 두 번 뒤에는 거리와 상관없이 바위 던지기(플레이어 둘레), 2단계는 돌진·내리치기·던지기를 번갈아 (돌진은 거리와 상관없이).
--   쓰러지면 관리자 OnEnemyKilled(전리품·경험치) + OnBossKilled(보상 상자·퀘스트 단계) 후 큰 연출과 함께 사라진다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DBoss = {
	Properties = {
		Kind = "Golem",
	},
}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function HD2DBoss:OnStart()
	self.Kind = self.Properties.Kind
	self.Row = D.Enemy(self.Kind)
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.Foot = self.Body:GetPosition().Z
	self.Radius = self.Row.Radius
	self.HitHeight = 210
	self.Health = self.Row.MaxHealth
	self.bBoss = true
	self.bDormant = true
	self.bDead = false
	self.Phase = 1
	self.State, self.Timer, self.Cooldown = "Dormant", 0, 1.5
	self.Flash, self.ContactCooldown, self.Time = 0, 0, 0
	self.NextPattern = "Throw"
	self.Anim = ""
	self.GM:RegisterEnemy(self)
	self:Play("Dormant")
	self.GM:InitEnemyCombat(self) -- 약점·실드(2단계에 바뀜)·상태 이상 (HD2DCombat.lua)
end

function HD2DBoss:OnDestroy()
	if self.GM then self.GM:UnregisterEnemy(self) end
end

function HD2DBoss:Play(Anim)
	if Anim ~= self.Anim then
		self.Anim = Anim
		self.Body:PlayFlipbook("Sprites/HD2D/Golem_" .. Anim .. ".eflipbook")
	end
end

function HD2DBoss:SetState(State, Time)
	self.State, self.Timer = State, Time or 0
end

function HD2DBoss:Count(Pattern)
	local P = self.GM.Report.BossPatterns
	P[Pattern] = (P[Pattern] or 0) + 1
	Log.Info("[HD2D] 보스 패턴: " .. Pattern)
end

function HD2DBoss:Windup()
	return self.Row.WindupTime * (self.Phase == 2 and 0.75 or 1.0)
end

function HD2DBoss:OnUpdate(Dt)
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
			self:SetState("Wake", 1.4)
			self:Play("Idle")
			self.GM:AddShake(16, 0.6)
			self.GM:SpawnFx("Poof", Ground + Vector3(-120, 30, 20), { Scale = 2.2 })
			self.GM:SpawnFx("Poof", Ground + Vector3(120, 30, 20), { Scale = 2.2 })
			self.GM:Hud():Announce(R.DisplayName, "들판의 수호자가 깨어났다!", 2.5)
			self.GM:Hud():ShowBoss(R.DisplayName, 1.0)
			Audio.PlayOneShot("Audio/RPG/HitHeavy.wav")
			Log.Info("[HD2D] 보스 깨어남: " .. R.DisplayName)
		end
		return
	end

	if self.State ~= "Dormant" and self.State ~= "Wake" then self.Cooldown = self.Cooldown - Dt end -- 회복 중에도 흐른다
	if self.State == "Wake" or self.State == "Recover" then
		self:Play("Idle")
		if self.Timer <= 0 then self:SetState("Chase", 0) end
	elseif self.State == "Chase" then
		if self.Cooldown <= 0 then
			self:StartPattern(Dist, DirP, Ground)
		elseif Dist > 260 and not E:IsStunned() then
			E:AddMovementInput(DirP * (self.Phase == 2 and 1.0 or 0.8))
			self:Play("Walk")
		else
			self:Play("Idle")
		end
	elseif self.State == "SlamWindup" then
		if self.Timer <= 0 then
			self:SetState("Slam", 0.9)
			self:Play("Slam")
			self.GM:KillFx(self.Warn)
			self.GM:AddShake(16, 0.35)
			self.GM:SpawnFx("Ring", Ground + Vector3(0, 0, 4), { Flat = true, Blend = 2, Scale = 0.6, Grow = 6.0, Life = 0.4, Fade = true,
			                                                    Color = { 1, 0.85, 0.6, 1 } })
			for I = -2, 2 do
				self.GM:SpawnFx("Dust", Ground + Vector3(I * 90, 40, 2), { Scale = 2.0, FlipX = I < 0 })
			end
			Audio.PlayOneShot("Audio/RPG/HitHeavy.wav")
			if Player and Dist < R.AttackRange + 40 then
				Player:TakeDamage(R.AttackDamage, Pos, self.HitOpt) -- 내리치기 = 기절 (표 Inflict)
			end
		end
	elseif self.State == "Slam" then
		if self.Timer <= 0 then self:EndPattern(0.7) end
	elseif self.State == "ThrowWindup" then
		if self.Timer <= 0 then
			self:Play("Idle")
			local Count = self.Phase == 2 and 5 or 3
			local PP = Player and Player.entity:GetWorldPosition() or Pos
			local Ground_ = Vector3(PP.X, PP.Y, Ground.Z)
			for I = 1, Count do
				local Offset = Vector3(0, 0, 0)
				if I > 1 then
					local A = (I - 2) / (Count - 1) * math.pi * 2 + 0.5
					Offset = Vector3(math.cos(A), math.sin(A), 0) * 230
				end
				self.GM:SpawnProjectile({ Kind = "Rock", Pos = Pos + Vector3(110, 10, 230), Target = Ground_ + Offset, Duration = 0.95 + I * 0.08,
				                          Height = 380, Damage = R.AttackDamage * 0.8, Team = "Enemy",
				                          HitOpt = self.Phase == 2 and { Status = "Burn", Chance = 0.5 } or nil }) -- 분노한 골렘의 바위는 달아올라 화상
			end
			Audio.PlayOneShot("Audio/RPG/Swing3.wav")
			self:EndPattern(1.1)
		end
	elseif self.State == "ChargeWindup" then
		local Blink = math.floor(self.Time * 14) % 2 == 0
		self.Sprite.Color = Blink and Vector4(1.7, 0.7, 0.6, 1) or self:BaseColor()
		if self.Timer <= 0 then
			self.ChargeDir = DirP
			self:SetState("Charge", 0.75)
			self:Play("Walk")
			E:AddKnockback(DirP * R.ProjectileSpeed, 0.72)
			Audio.PlayOneShot("Audio/RPG/Dash.wav")
		end
	elseif self.State == "Charge" then
		if math.floor(self.Time * 20) % 2 == 0 then
			self.GM:SpawnFx("Dust", Ground + Vector3(-self.ChargeDir.X * 100, 30, 2), { Scale = 1.6, FlipX = self.ChargeDir.X < 0 })
		end
		if self.Timer <= 0 then self:EndPattern(0.9) end
	end

	-- 몸 접촉 (돌진 중이면 큰 피해)
	if Player and Dist < self.Radius + 45 and self.ContactCooldown <= 0 then
		local Damage = self.State == "Charge" and R.AttackDamage or R.ContactDamage
		if Player:TakeDamage(Damage, Pos) then self.ContactCooldown = 1.0 end
	end

	if self.Flash > 0 then
		self.Flash = self.Flash - Dt
		-- 피격: 하얗게 덮었다가 빠르게 풀림 (SpriteComponent.FlashColor — 조명·안개 뒤에 덮는다)
		self.Sprite.FlashColor = Vector4(1, 1, 1, self.Flash > 0 and math.min(0.85, self.Flash * 9) or 0)
		if self.State ~= "ChargeWindup" then self.Sprite.Color = self:BaseColor() end
	elseif self.State ~= "ChargeWindup" then
		self.Sprite.Color = self:BaseColor()
	end
end

function HD2DBoss:BaseColor()
	return self.Phase == 2 and Vector4(1.12, 0.9, 0.86, 1) or Vector4(1, 1, 1, 1)
end

-- 패턴 끝: 숨 고르기 Recover초(2단계 × 0.8) + 다음 패턴까지 쿨다운
function HD2DBoss:EndPattern(Recover)
	local Scale = self.Phase == 2 and 0.8 or 1.0
	self:SetState("Recover", Recover * Scale)
	self.Cooldown = self.Row.AttackCooldown * (self.Phase == 2 and 0.8 or 1.0) + Recover * Scale
end

function HD2DBoss:StartPattern(Dist, DirP, Ground)
	local R = self.Row
	self.Cooldown = 99 -- 패턴이 끝날 때 EndPattern이 다시 정한다
	local bClose = Dist < R.AttackRange
	if self.Phase == 2 then
		-- 2단계: 돌진 → (가까우면 내리치기, 멀면 던지기) → 던지기 차례 순환
		self.Turn = (self.Turn or 0) % 3 + 1
		if self.Turn == 1 then
			self.NextPattern = "Charge"
		elseif self.Turn == 2 then
			self.NextPattern = bClose and "Slam" or "Throw"
		else
			self.NextPattern = "Throw"
		end
	else
		-- 1단계: 가까우면 내리치기, 단 두 번 내리친 뒤에는 던지기
		self.SlamRun = self.SlamRun or 0
		if bClose and self.SlamRun < 2 then
			self.NextPattern = "Slam"
			self.SlamRun = self.SlamRun + 1
		else
			self.NextPattern = "Throw"
			self.SlamRun = 0
		end
	end
	local Pattern = self.NextPattern
	if Pattern == "Slam" then
		self:SetState("SlamWindup", self:Windup())
		self:Play("Raise")
		self.Warn = self.GM:SpawnSprite({ Sprite = "Sprites/HD2D/Fx.esprite", Slice = "Warn", Position = Ground + Vector3(0, 0, 3), Flat = true, Blend = 0,
		                                  Life = self:Windup() + 0.05, Scale = (R.AttackRange + 40) / 120.0, Color = { 1, 1, 1, 0.9 } })
		self:Count("Slam")
	elseif Pattern == "Charge" then
		self:SetState("ChargeWindup", self:Windup())
		self:Play("Raise")
		self.GM:SpawnFx("Alert", Ground + Vector3(0, 30, 470), { Blend = 2, Scale = 2.4 })
		self:Count("Charge")
	else
		self:SetState("ThrowWindup", 0.55)
		self:Play("Throw")
		self:Count("Throw")
	end
end

function HD2DBoss:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
end

function HD2DBoss:TakeHit(Damage, Dir, Knockback)
	if self.bDead or self.bDormant then return false end
	self.Health = self.Health - Damage
	self.Flash = 0.08
	self.entity:AddKnockback(Dir * (Knockback or 0) * 0.08, 0.05)
	self.GM:Hud():ShowBoss(self.Row.DisplayName, self.Health / self.Row.MaxHealth)
	if self.Phase == 1 and self.Health <= self.Row.MaxHealth * 0.5 then
		self.Phase = 2
		local P = self.entity:GetWorldPosition()
		self.GM:Hud():Announce("골렘이 분노했다!", "몸이 붉게 달아올랐다", 2.0)
		self.GM:AddShake(12, 0.4)
		for _, Side in ipairs({ -1, 1 }) do
			self.GM:SpawnEnemy("Slime", Vector3(P.X + Side * 260, P.Y + 120, P.Z + self.Foot + 46))
		end
		self:Count("Summon")
		Log.Info("[HD2D] 보스 2단계 (분노)")
	end
	if self.Health <= 0 then
		self.bDead = true
		local P = self.entity:GetWorldPosition()
		local Ground = Vector3(P.X, P.Y, P.Z + self.Foot)
		self.GM:KillFx(self.Warn)
		for I = 0, 4 do
			self.GM:SpawnFx("Poof", Ground + Vector3((I - 2) * 80, 40, 60 + (I % 2) * 120), { Scale = 2.4 })
		end
		self.GM:SpawnFx("Burst", Ground + Vector3(0, 50, 200), { Blend = 2, Scale = 4.0, Color = { 1, 0.85, 0.6, 1 } })
		self.GM:AddShake(20, 0.7)
		Game.HitStop(0.25)
		Audio.PlayOneShot("Audio/RPG/HitHeavy.wav")
		self.GM:OnEnemyKilled(self)
		self.GM:OnBossKilled(self)
		Log.Info("[HD2D] 보스 처치: " .. self.Row.DisplayName)
		self.entity:Destroy()
	end
	return true
end

return HD2DBoss
