-- HD-2D 데모 동료 "견습 마법사 엘라" (Prefabs/Demo/HD2D/Companion.eprefab — 캡슐 이동기 + Visual > Body·Shadow·Marker, 관리자 HD2DCombat.lua가 만든다).
--   영입 전(Npc 모드): 마을 광장에 서 있다 — 관리자 마을 사람 목록에 Role "Companion"으로 들어가 E로 말을 건다(촌장 퀘스트를 받은 뒤 영입, 머리 위 "!").
--   영입 뒤(Follow 모드): 플레이어 뒤를 따라다니며 (멀어지면 내비메시 길, 화면 밖으로 크게 떨어지면 곁으로 순간이동)
--     · 마법: CompanionCastInterval초마다 플레이어 둘레 가장 가까운 적에게 그 적의 약점 속성(불·얼음·번개·빛 중 — 모르면 빛) 마법 → 관리자 HitEnemy
--       (약점 공개·실드·브레이크는 플레이어 공격과 같은 규칙 — HD2DCombat.lua)
--     · 치유: 플레이어 체력이 45% 아래면 CompanionHealInterval초마다 최대 HP × CompanionHealRatio + 독·화상·빙결 해제
--     · 피격: 적 몸·적 투사체에 맞는다 (무적 0.6초). 체력 0 → 쓰러짐(CompanionReviveTime초) → 절반 체력으로 일어남
--   수치 = Balance.edata Companion* (HD2DCombatGen.COMPANION), 체력·마법 위력은 플레이어 레벨을 따른다. 영입 여부·체력은 일행 저장(Combat.Companion).
--   이동기가 루트를 이동 방향으로 돌리므로 Visual 회전을 매 프레임 상쇄한다 (플레이어·적과 같은 규칙). 원본 그림은 오른쪽을 본다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DCompanion = {
	Properties = {},
}

local Book = "Sprites/HD2D/Mage_"
local CombatSprite = "Sprites/HD2D/Combat.esprite"
local Spells = { Fire = true, Ice = true, Thunder = true, Light = true }
local SpellStatus = { Fire = { "Burn", 0.25 }, Ice = { "Freeze", 0.3 }, Thunder = { "Stun", 0.15 } }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function HD2DCompanion:OnStart()
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.Marker = self.Visual:FindChild("Marker")
	self.Mover = self.entity:GetComponent("CharacterMovementComponent")
	self.Facing, self.Anim = "Down", ""
	self.CastCd, self.HealCd, self.Cast = 1.5, 3.0, nil
	self.Invuln, self.Hurt, self.ContactCd = 0, 0, 0
	self.DownTimer = 0
	self.Seed = 4242
	self.PathTimer = 0
	self.Mode = self.GM.CompanionRecruited and "Follow" or "Npc"
	self:RecalcHealth(true)
	if self.GM.CompanionHealth then self.Health = math.max(1, math.min(self.MaxHealth, self.GM.CompanionHealth)) end
	self.GM:RegisterCompanion(self)
	self:Play("Idle")
end

function HD2DCompanion:OnDestroy()
	if self.GM and self.GM.Companion == self then self.GM.Companion = nil end
end

function HD2DCompanion:Random()
	self.Seed = (self.Seed * 1103515245 + 12345) % 2147483648
	return (self.Seed % 100000) / 100000.0
end

function HD2DCompanion:Level()
	local P = self.GM:GetPlayer()
	return P and P.Level or 1
end

-- 최대 체력 (플레이어 레벨). bFull = 가득
function HD2DCompanion:RecalcHealth(bFull)
	local B = D.Balance()
	local Old = self.MaxHealth
	self.MaxHealth = math.floor(B.CompanionHealth + B.CompanionHealthPerLevel * (self:Level() - 1))
	if bFull or not self.Health then
		self.Health = self.MaxHealth
	elseif Old and self.MaxHealth > Old then
		self.Health = self.Health + (self.MaxHealth - Old)
	end
end

-- 스킬 투사체가 부른다 (관리자 SpawnSkillShot의 Owner) — 레벨 배율 ±10%, 치명타 없음
function HD2DCompanion:RollDamage(Base)
	local Scale = 1.0 + D.Balance().DamagePerLevel * (self:Level() - 1)
	return math.max(1, math.floor(Base * Scale * (0.9 + 0.2 * self:Random()) + 0.5)), false
end

function HD2DCompanion:Play(Anim)
	local Dir = (self.Facing == "Left" or self.Facing == "Right") and "Side" or self.Facing
	local Want = Anim == "Down" and "Down" or (Anim .. Dir)
	if Want ~= self.Anim then
		self.Anim = Want
		self.Body:PlayFlipbook(Book .. Want .. ".eflipbook")
	end
	self.Body:SetSpriteFlip(self.Facing == "Left", false)
end

function HD2DCompanion:Face(V)
	if math.abs(V.X) < 0.05 and math.abs(V.Y) < 0.05 then return end
	if math.abs(V.X) >= math.abs(V.Y) * 0.8 then
		self.Facing = V.X > 0 and "Right" or "Left"
	else
		self.Facing = V.Y > 0 and "Down" or "Up"
	end
end

-- 목표(지면 점)로 가는 방향 (멀면 내비메시 경로, 없으면 곧장)
function HD2DCompanion:PathDir(Pos, Goal, Dt)
	local Direct = Flat(Goal - Pos)
	local L = Direct:Length()
	if L < 1 then return Vector3(0, 0, 0) end
	if L < 350 then return Direct * (1.0 / L) end
	self.PathTimer = self.PathTimer - Dt
	if self.PathTimer <= 0 or not self.PathGoal or Flat(self.PathGoal - Goal):Length() > 200 then
		self.PathTimer = 0.7
		self.PathGoal = Goal
		self.Path = AI.FindPath(Vector3(Pos.X, Pos.Y, Pos.Z - 85), Vector3(Goal.X, Goal.Y, Pos.Z - 85))
		self.PathIndex = 2
	end
	if self.Path then
		while self.Path[self.PathIndex] and Flat(self.Path[self.PathIndex] - Pos):Length() < 70 do self.PathIndex = self.PathIndex + 1 end
		local Next = self.Path[self.PathIndex]
		if Next then
			local Dd = Flat(Next - Pos)
			if Dd:Length() > 1 then return Dd:Normalized() end
		end
	end
	return Direct * (1.0 / L)
end

function HD2DCompanion:OnUpdate(Dt)
	if Dt <= 0 then return end
	local GM = self.GM
	local Player = GM:GetPlayer()
	if not Player or not Player.bStarted then return end
	local Pos = self.entity:GetWorldPosition()
	if self.Mode == "Npc" then
		-- 광장에서 기다린다 (플레이어 쪽을 본다)
		self:Face(Flat(Player.entity:GetWorldPosition() - Pos))
		self:Play("Idle")
		self.Marker:GetComponent("SpriteComponent").Visible = GM:CanRecruitCompanion()
		return
	end
	self.Marker:GetComponent("SpriteComponent").Visible = false
	self.Invuln = math.max(0, self.Invuln - Dt)
	self.ContactCd = math.max(0, self.ContactCd - Dt)
	self.CastCd = self.CastCd - Dt
	self.HealCd = self.HealCd - Dt
	local B = D.Balance()
	local PP = Player.entity:GetWorldPosition()
	-- 쓰러짐
	if self.DownTimer > 0 then
		self.DownTimer = self.DownTimer - Dt
		self:Play("Down")
		self.Sprite.Color = Vector4(0.75, 0.72, 0.8, 1)
		if self.DownTimer <= 0 then self:Revive() end
		return
	end
	-- 크게 떨어지면 (맵 이동·순간이동·벼랑) 곁으로
	if Flat(PP - Pos):Length() > 1600 or math.abs(PP.Z - Pos.Z) > 320 then
		self:Teleport(PP)
		return
	end
	self:TakeEnemyHits(Pos)
	if self.DownTimer > 0 then return end
	-- 시전 중
	if self.Cast then
		self:UpdateCast(Dt)
	else
		local Target = GM:NearestEnemy(PP, 950)
		if Player.Health < Player.MaxHealth * 0.45 and self.HealCd <= 0 and not Player.bDead then
			self:StartCast("Heal", nil)
		elseif Target and self.CastCd <= 0 and Flat(Target.entity:GetWorldPosition() - Pos):Length() < 800 then
			self:StartCast(self:ChooseElement(Target), Target)
		else
			self:FollowStep(Dt, Pos, PP, Player, Target)
		end
	end
	-- 피격 붉은빛
	self.Hurt = math.max(0, self.Hurt - Dt)
	self.Sprite.Color = self.Hurt > 0 and Vector4(1, 0.45, 0.45, 1) or Vector4(1, 1, 1, 1)
	self.Sprite.Visible = not (self.Invuln > 0 and math.floor(self.Invuln * 16) % 2 == 1)
end

-- 따라가기: 플레이어 뒤쪽(보는 방향 반대) 140cm, 적이 가까우면 물러선다
function HD2DCompanion:FollowStep(Dt, Pos, PP, Player, Target)
	local Back = Player.AimDir or Vector3(0, 1, 0)
	local Goal = PP - Back * 140 + Vector3(-Back.Y, Back.X, 0) * 50
	local To = Flat(Goal - Pos)
	local Move = nil
	if Target then
		local Away = Flat(Pos - Target.entity:GetWorldPosition())
		if Away:Length() < 260 then Move = Away:Normalized() end
	end
	if not Move and To:Length() > 90 then
		Move = self:PathDir(Pos, Goal, Dt) * math.min(1.0, To:Length() / 220.0 + 0.35)
	end
	if Move and not self.entity:IsStunned() then
		self.entity:AddMovementInput(Move)
		self:Face(Move)
		self:Play("Walk")
	else
		if Target then self:Face(Flat(Target.entity:GetWorldPosition() - Pos)) else self:Face(Flat(PP - Pos)) end
		self:Play("Idle")
	end
end

-- 대상 약점 중 엘라가 쓰는 속성 (불·얼음·번개·빛) — 없으면 빛
function HD2DCompanion:ChooseElement(Target)
	for _, E in ipairs(Target.Weak or {}) do
		if Spells[E] then return E end
	end
	return "Light"
end

function HD2DCompanion:StartCast(Element, Target)
	local Pos = self.entity:GetWorldPosition()
	self.Cast = { Element = Element, Target = Target, Time = 0, bFired = false }
	if Target then self:Face(Flat(Target.entity:GetWorldPosition() - Pos)) end
	self.Anim = ""
	self:Play("Cast")
	local Col = self.GM.ElementColor[Element == "Heal" and "Bow" or Element] or { 1, 1, 1, 1 }
	self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = "Sprites/HD2D/Combat_MagicCircle.eflipbook", Position = Pos + Vector3(0, 0, -82), Flat = true,
	                      Blend = 2, Life = 0.7, Fade = true, Scale = 0.45, Grow = 0.4, Color = Col })
	Audio.PlayOneShot("Audio/RPG/Spin.wav", Pos, 0.45, 1.7)
end

function HD2DCompanion:UpdateCast(Dt)
	local C = self.Cast
	C.Time = C.Time + Dt
	if not C.bFired and C.Time >= 0.3 then
		C.bFired = true
		if C.Element == "Heal" then self:CastHeal() else self:CastSpell(C.Element, C.Target) end
	end
	if C.Time >= 0.68 then
		self.Cast = nil
		self.Anim = ""
	end
end

function HD2DCompanion:CastHeal()
	local B = D.Balance()
	local Player = self.GM:GetPlayer()
	self.HealCd = B.CompanionHealInterval
	Player:Heal(math.floor(Player.MaxHealth * B.CompanionHealRatio + 0.5), 0)
	self.GM:CureStatus(Player)
	local PP = Player.entity:GetWorldPosition()
	self.GM:SpawnSprite({ Sprite = "Sprites/HD2D/Fx.esprite", Slice = "Pillar", Position = PP + Vector3(0, 20, -85), Blend = 2, Life = 0.7, Fade = true,
	                      Color = { 0.6, 1, 0.7, 0.9 }, Scale = { 1.2, 1.5 } })
	Audio.PlayOneShot(self.GM.Sounds.Potion, 0.8, 1.25)
	self.GM:Hud():Toast("UI/Demo/HD2D/Combat/SkillHeal.png", D.Balance().CompanionName .. "의 치유 마법")
	self.GM.Report.CompanionHeals = (self.GM.Report.CompanionHeals or 0) + 1
	Log.Info("[HD2D] 동료 치유")
end

function HD2DCompanion:CastSpell(Element, Target)
	local GM = self.GM
	local B = D.Balance()
	self.CastCd = B.CompanionCastInterval
	GM.Report.CompanionCasts = (GM.Report.CompanionCasts or 0) + 1
	if not Target or not Target.entity:IsValid() or Target.bDead then return end
	local Pos = self.entity:GetWorldPosition()
	local TP = Target.entity:GetWorldPosition()
	local Dir = Flat(TP - Pos)
	Dir = Dir:Length() > 1 and Dir:Normalized() or Vector3(0, 1, 0)
	local St = SpellStatus[Element]
	local Hit = { Element = Element, Status = St and St[1] or nil, Chance = St and St[2] or 0 }
	if Element == "Thunder" then
		-- 번개: 하늘에서 바로 떨어진다
		GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = "Sprites/HD2D/Combat_Lightning.eflipbook", Position = TP + Vector3(0, 18, (Target.Foot or -60)),
		                 Blend = 2, Life = 0.28, Scale = 0.9 })
		local Damage = self:RollDamage(B.CompanionDamage)
		GM:HitEnemy(Target, Damage, Dir, 250, false, nil, Hit)
		GM:AddShake(4, 0.1)
		Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", TP, 0.8, 1.45)
		return
	end
	local Fx
	if Element == "Fire" then
		Fx = { Sprite = CombatSprite, Flipbook = "Sprites/HD2D/Combat_Fireball.eflipbook", Rotation = GM.ScreenAngle(Dir), Blend = 0, Life = 3.0, Scale = 1.3 }
	elseif Element == "Ice" then
		Fx = { Sprite = CombatSprite, Flipbook = "Sprites/HD2D/Combat_IceLance.eflipbook", Rotation = GM.ScreenAngle(Dir), Blend = 0, Life = 3.0, Scale = 1.2 }
	else
		Fx = { Sprite = "Sprites/HD2D/Fx.esprite", Flipbook = "Sprites/HD2D/Fx_Bolt.eflipbook", Blend = 2, Life = 3.0, Scale = 1.2, Color = { 1, 0.92, 0.6, 1 } }
	end
	GM:SpawnSkillShot({ Pos = Pos + Dir * 45 + Vector3(0, 10, 20), Dir = Dir, Speed = 1500, Range = 950, Damage = B.CompanionDamage, Owner = self,
	                    Combat = Hit, HitRadius = 38, Knockback = 300, Fx = Fx,
	                    OnHit = function(Shot, S)
		                    GM:SpawnFx("Burst", Shot.Pos + Vector3(0, 12, 0), { Blend = 2, Scale = 0.9, Color = GM.ElementColor[Element] })
	                    end })
	Audio.PlayOneShot("Audio/RPG/Swing2.wav", Pos, 0.7, 1.35)
end

-- 적 몸·적 투사체 (적 AI는 플레이어만 노리므로 엘라는 곁에서 휘말린다)
function HD2DCompanion:TakeEnemyHits(Pos)
	if self.Invuln > 0 then return end
	for _, S in ipairs(self.GM:AliveEnemies()) do
		if self.ContactCd <= 0 and not S.bBroken and Flat(S.entity:GetWorldPosition() - Pos):Length() < (S.Radius or 45) + 34 then
			local R = S.Row
			local bAttack = S.State == "Attack" or S.State == "Charge" or S.State == "Pounce"
			self:TakeDamage(bAttack and R.AttackDamage or R.ContactDamage, S.entity:GetWorldPosition())
			self.ContactCd = 0.9
			return
		end
	end
	for _, P in ipairs(self.GM.Projectiles) do
		if P.Team == "Enemy" and P.Kind ~= "Rock" and Flat(P.Pos - Pos):Length() < 48 then
			self:TakeDamage(P.Damage, P.Pos - P.Dir * 100)
			P.Travel = P.Range -- 다음 갱신에 사라진다
			return
		end
	end
end

function HD2DCompanion:TakeDamage(Amount, From)
	local Damage = math.max(1, math.floor(Amount + 0.5))
	self.Health = self.Health - Damage
	self.Invuln, self.Hurt = 0.6, 0.15
	local Pos = self.entity:GetWorldPosition()
	self.GM:DamageNumber(Pos + Vector3(0, 0, 70), tostring(Damage), { 1, 0.55, 0.65, 1 }, 0.9)
	local Away = Flat(Pos - From)
	Away = Away:Length() > 1 and Away:Normalized() or Vector3(0, 1, 0)
	self.entity:AddKnockback(Away * 500, 0.15)
	self.Cast = nil
	if self.Health <= 0 then self:KnockOut() end
end

function HD2DCompanion:KnockOut()
	local B = D.Balance()
	self.Health = 0
	self.DownTimer = B.CompanionReviveTime
	self.Cast = nil
	self.Sprite.Visible = true
	self.GM:SpawnFx("Poof", self.entity:GetWorldPosition() + Vector3(0, 10, -60), { Scale = 1.1 })
	self.GM:Hud():Toast("UI/Demo/HD2D/Portraits/Mage.png", B.CompanionName .. "이(가) 쓰러졌다…")
	Audio.PlayOneShot("Audio/RPG/Hurt.wav", self.entity:GetWorldPosition(), 0.9, 1.3)
	self.GM.Report.CompanionDowns = (self.GM.Report.CompanionDowns or 0) + 1
	Log.Info("[HD2D] 동료 쓰러짐")
end

function HD2DCompanion:Revive()
	self:RecalcHealth(false)
	self.Health = math.floor(self.MaxHealth * 0.5)
	self.Invuln = 1.0
	self.Anim = ""
	local Pos = self.entity:GetWorldPosition()
	self.GM:SpawnHealFx(Pos, { 0.8, 0.7, 1, 1 })
	self.GM:Hud():Toast("UI/Demo/HD2D/Portraits/Mage.png", D.Balance().CompanionName .. "이(가) 다시 일어섰다")
	Audio.PlayOneShot("Asset/Kenney_InterfaceSounds/confirm.wav", Pos, 0.7, 1.3)
	Log.Info("[HD2D] 동료 일어남")
end

-- 플레이어 곁으로 (뒤쪽, 지면 위)
function HD2DCompanion:Teleport(PP)
	local Back = (self.GM:GetPlayer().AimDir or Vector3(0, 1, 0))
	local At = PP - Back * 120 + Vector3(0, 0, 4)
	self.entity:SetPosition(At)
	self.Path = nil
	self.GM:SpawnFx("Sparkle", At + Vector3(0, 20, 10), { Blend = 2, Scale = 1.0, Color = { 0.85, 0.7, 1, 1 } })
end

-- 영입 (관리자 RecruitCompanion이 부른다)
function HD2DCompanion:StartFollowing()
	self.Mode = "Follow"
	self:RecalcHealth(true)
	self.Marker:GetComponent("SpriteComponent").Visible = false
end

function HD2DCompanion:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
	local H = self.GM:Hud()
	if H and H.SetCompanion then
		if self.Mode == "Follow" and self.GM.Menu ~= "Title" then
			H:SetCompanion(D.Balance().CompanionName, self.Health, self.MaxHealth, self.DownTimer)
		else
			H:SetCompanion(nil)
		end
	end
end

return HD2DCompanion
