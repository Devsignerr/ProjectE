-- HD-2D 데모 플레이어 확장: 스킬 (HD2DPlayer.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 플레이어 self에).
--   칸 5개: K / L = 장비한 무기의 기술(무기 표 SkillK·SkillL), 3 / 4 / 5 = 마법(Balance.MagicSkills). 스킬 표 = Data/Demo/HD2D/Skills.etable.
--   조건: 레벨(UnlockLevel)·MP(ManaCost)·쿨다운(Cooldown), 다른 동작(공격·대시·시전) 중이 아닐 때. 시전 동안(CastTime) 움직이지 않는다.
--   부스트(Q로 올린 BP 단계 L)를 실으면 BP L을 쓰고 피해 × (1 + BoostDamagePerLevel × L), 다단 기술(회전·화살비·기둥·연쇄)은 L대 더.
--   판정은 시전 시작 뒤 HitDelay(다단은 간격을 두고) — 늦은 판정은 SkillEvents(게임 시간)로 미뤄 두고 UpdateSkills가 처리한다.
--   명중은 관리자 HitEnemy(…, { Element, Status, Chance }) → HD2DCombat.lua가 약점·실드·브레이크·상태 이상을 처리한다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local Skills = {}

local CombatSprite = "Sprites/HD2D/Combat.esprite"
local Book = "Sprites/HD2D/Combat_"
local FxSprite = "Sprites/HD2D/Fx.esprite"
local SpinOrder = { "Down", "Right", "Up", "Left" }
local SpinDir = { Down = Vector3(0, 1, 0), Right = Vector3(1, 0, 0), Up = Vector3(0, -1, 0), Left = Vector3(-1, 0, 0) }
local ElemSound = {  -- 속성마다 시전 소리 (경로, 음량, 피치)
	Fire = { "Audio/RPG/Spin.wav", 0.9, 0.62 }, Ice = { "Audio/RPG/Block.wav", 0.75, 1.6 }, Thunder = { "Audio/RPG/HitHeavy.wav", 0.8, 1.45 },
	Light = { "Asset/Kenney_InterfaceSounds/confirm.wav", 0.8, 1.5 }, Slash = { "Audio/RPG/Swing3.wav", 1.0, 1.0 }, Pierce = { "Audio/RPG/Dash.wav", 1.0, 0.9 },
	Bow = { "Audio/RPG/Swing2.wav", 1.0, 1.15 },
}

local function Flat(V) return Vector3(V.X, V.Y, 0) end
local function Rotate(V, Deg)
	local A = math.rad(Deg)
	return Vector3(V.X * math.cos(A) - V.Y * math.sin(A), V.X * math.sin(A) + V.Y * math.cos(A), 0)
end

function Skills:InitSkills()
	self.IsPlayerScript = true
	self.Status, self.StatusTick = {}, {}
	self.SkillCooldown = {}
	self.SkillEvents = {}
	self.SkillClock = 0.0
	self.Cast = nil
end

-- 칸 1~5의 스킬 id (무기 기술은 장비한 무기에 따라)
function Skills:SkillSlots()
	local W = self.Weapon or {}
	local M = D.Balance().MagicSkills or {}
	return { W.SkillK or "", W.SkillL or "", M[1] or "", M[2] or "", M[3] or "" }
end

-- 입력 (사람 — 자동 조종은 같은 이름의 필드를 채운다)
function Skills:GatherSkillInput(In)
	In.SkillK = In.SkillK or Input.IsKeyPressed("K")
	In.SkillL = In.SkillL or Input.IsKeyPressed("L")
	In.Magic1 = In.Magic1 or Input.IsKeyPressed("3")
	In.Magic2 = In.Magic2 or Input.IsKeyPressed("4")
	In.Magic3 = In.Magic3 or Input.IsKeyPressed("5")
end

function Skills:IsCasting() return self.Cast ~= nil end

-- 시간·쿨다운·미뤄 둔 판정 (게임 시간 — 메뉴로 멈추면 같이 멈춘다)
function Skills:UpdateSkills(Dt)
	self.SkillClock = self.SkillClock + Dt
	for Id, Left in pairs(self.SkillCooldown) do
		Left = Left - Dt
		self.SkillCooldown[Id] = Left > 0 and Left or nil
	end
	if #self.SkillEvents > 0 then
		local Keep, Due = {}, {}
		for _, E in ipairs(self.SkillEvents) do
			if self.SkillClock >= E.At then Due[#Due + 1] = E else Keep[#Keep + 1] = E end
		end
		self.SkillEvents = Keep
		for _, E in ipairs(Due) do E.Fn() end
	end
end

function Skills:Later(Delay, Fn)
	self.SkillEvents[#self.SkillEvents + 1] = { At = self.SkillClock + Delay, Fn = Fn }
end

-- 칸 상태: 행, 잠김(레벨), MP 부족, 남은 쿨다운
function Skills:SkillState(Id)
	local Row = Id ~= "" and D.Skill(Id) or nil
	if not Row then return nil end
	return Row, self.Level < Row.UnlockLevel, self.Mana < Row.ManaCost, self.SkillCooldown[Id] or 0
end

function Skills:UpdateSkillHud()
	local H = self.GM:Hud()
	if not H or not H.SetSkillSlot then return end
	for K, Id in ipairs(self:SkillSlots()) do
		local Row, bLocked, bNoMana, Cd = self:SkillState(Id)
		H:SetSkillSlot(K - 1, Row, bLocked, bNoMana, Cd)
	end
end

-- 대기 상태에서 스킬 키를 눌렀으면 시작 (시작했으면 true)
function Skills:TryStartSkill(In, Move)
	local Slot = (In.SkillK and 1) or (In.SkillL and 2) or (In.Magic1 and 3) or (In.Magic2 and 4) or (In.Magic3 and 5)
	if not Slot then return false end
	local Id = self:SkillSlots()[Slot]
	local Row, bLocked, bNoMana, Cd = self:SkillState(Id)
	if not Row then return false end
	local H = self.GM:Hud()
	if bLocked then
		H:Toast(Row.Icon, string.format("%s: Lv %d에 배운다", Row.DisplayName, Row.UnlockLevel))
		Audio.PlayOneShot(self.GM.Sounds.Error)
		return false
	end
	if Cd > 0 then
		Audio.PlayOneShot(self.GM.Sounds.Error, 0.6, 1.2)
		return false
	end
	if bNoMana then
		if self.NoManaTimer <= 0 then
			self.NoManaTimer = 1.0
			H:Toast(D.Item("Ether").Icon, "마나가 부족하다")
			Audio.PlayOneShot(self.GM.Sounds.Error)
		end
		return false
	end
	self:StartSkill(Id, Row, Move)
	return true
end

function Skills:StartSkill(Id, Row, Move)
	local GM = self.GM
	self.Mana = self.Mana - Row.ManaCost
	self.SkillCooldown[Id] = Row.Cooldown
	self.bQueued = false
	-- 부스트를 실었으면 BP를 쓴다
	local L = self.BoostLevel
	self.BoostLevel = 0
	if L > 0 then
		self.BP = self.BP - L
		GM.Report.Boosts = GM.Report.Boosts + 1
		GM.Report.BoostMax = math.max(GM.Report.BoostMax, L)
		GM:Hud():BoostBurst(string.format("BOOST ×%d!", L + 1), 0.9)
		GM:AddShake(3 + L * 2, 0.18)
	end
	local bRanged = Row.Kind == "Volley" or Row.Kind == "Pierce" or Row.Kind == "Bolt" or Row.Kind == "Pillar" or Row.Kind == "Chain"
	local Aim = self:ChooseAim(Move, { Kind = bRanged and "Arrow" or "Slash", Range = math.max(Row.Range, 200), Arc = bRanged and 70 or 120 })
	self.AimDir = Aim
	self.Facing = (math.abs(Aim.X) >= math.abs(Aim.Y) * 0.8) and (Aim.X > 0 and "Right" or "Left") or (Aim.Y > 0 and "Down" or "Up")
	self.Cast = { Id = Id, Row = Row, Elapsed = 0.0, Aim = Aim, Boost = L, Mult = 1.0 + D.Balance().BoostDamagePerLevel * L, SpinTimer = 0, Hit = {} }
	self.Anim = ""
	if Row.Slot == "Weapon" and Row.Kind ~= "Rush" then self.AttackTimer = Row.CastTime end
	local R = GM.Report.Skills
	R[Id] = (R[Id] or 0) + 1
	GM:Hud():ShowSkillName(Row.Icon, Row.DisplayName, 1.2)
	local Pos = self.entity:GetWorldPosition()
	local Col = GM.ElementColor[Row.Element] or { 1, 1, 1, 1 }
	-- 마법: 발밑 마법진 + 몸이 속성 빛으로
	if Row.Slot == "Magic" then
		GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "MagicCircle.eflipbook", Position = Pos + Vector3(0, 0, -82), Flat = true, Blend = 2,
		                 Life = Row.CastTime + 0.25, Fade = true, Scale = 0.55, Grow = 0.6, Color = Col })
		GM:SpawnFx("Sparkle", Pos + Vector3(0, 30, 30), { Blend = 2, Scale = 1.1, Color = Col })
		Audio.PlayOneShot("Audio/RPG/Spin.wav", 0.55, 1.6)
	end
	self["Start" .. Row.Kind](self, self.Cast, Pos)
	Log.Info(string.format("[HD2D] 스킬: %s (MP %d, 남은 MP %d%s)", Row.DisplayName, Row.ManaCost, math.floor(self.Mana), L > 0 and string.format(", 부스트 %d", L) or ""))
end

-- 시전 중 매 프레임 (공격 대신 — 움직이지 않는다)
function Skills:UpdateSkillCast(Dt)
	local C = self.Cast
	C.Elapsed = C.Elapsed + Dt
	local Row = C.Row
	if self.AttackTimer > 0 then self.AttackTimer = math.max(0, self.AttackTimer - Dt) end
	local Tick = self["Tick" .. Row.Kind]
	if Tick then Tick(self, C, Dt) end
	-- 몸이 속성 빛으로 (시전 동안)
	local Col = self.GM.ElementColor[Row.Element] or { 1, 1, 1, 1 }
	local Glow = math.max(0, 1.0 - C.Elapsed / math.max(Row.CastTime, 0.05))
	self.Sprite.FlashColor = Vector4(Col[1], Col[2], Col[3], 0.45 * Glow)
	if C.Elapsed >= Row.CastTime and (Row.Kind ~= "Rush" or self.DashTimer <= 0) then
		self.Cast = nil
		self.AttackTimer = 0
		self.DashTimer = 0
		self.Sprite.FlashColor = Vector4(1, 1, 1, 0)
		self.Anim = ""
		self.ComboWindow = 0.2
	end
end

-- 한 대 (적 S에게). Away = 넉백 방향
function Skills:SkillHit(C, S, Away, Knockback, Scale)
	local Row = C.Row
	local Damage, bCrit = self:RollDamage(Row.Damage * C.Mult * (Scale or 1.0))
	if C.Hit then C.Hit[S] = true end
	return self.GM:HitEnemy(S, Damage, Away, Knockback or 500, bCrit, self.GM.Equipped,
		{ Element = Row.Element, Status = Row.Status ~= "" and Row.Status or nil, Chance = Row.StatusChance })
end

function Skills:HitFeel(N, Stop, Shake, Sound, Pitch)
	if N <= 0 then return end
	Game.HitStop(Stop)
	self.GM:AddShake(Shake, 0.15)
	Audio.PlayOneShot(Sound or "Audio/RPG/HitHeavy.wav", 1.0, Pitch or 1.0)
end

function Skills:ElementSound(Element)
	local S = ElemSound[Element]
	if S then Audio.PlayOneShot(S[1], self.entity:GetWorldPosition(), S[2], S[3]) end
end

-- ---- 회오리 베기: 몸을 돌리며 둘레를 여러 번
function Skills:StartWhirl(C, Pos)
	local Row = C.Row
	local N = Row.Hits + C.Boost
	for I = 0, N - 1 do
		self:Later(Row.HitDelay + I * 0.11, function()
			local P = self.entity:GetWorldPosition()
			self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Whirl.eflipbook", Position = P + Vector3(0, 6, -42), Blend = 2, Life = 0.24,
			                      Scale = Row.Radius / 205.0, FlipX = I % 2 == 1, Color = I == N - 1 and { 1, 0.9, 0.6, 1 } or nil })
			Audio.PlayOneShot(I % 2 == 0 and "Audio/RPG/Swing1.wav" or "Audio/RPG/Swing2.wav", 1.0, 1.0 + I * 0.06)
			local Hit = 0
			for _, S in ipairs(self.GM:FindEnemies(P, Row.Radius)) do
				local Away = Flat(S.entity:GetWorldPosition() - P)
				Away = Away:Length() > 1 and Away:Normalized() or C.Aim
				if self:SkillHit(C, S, Away, I == N - 1 and 820 or 260) then Hit = Hit + 1 end
			end
			self:HitFeel(Hit, I == N - 1 and 0.07 or 0.03, I == N - 1 and 9 or 5, I == N - 1 and "Audio/RPG/HitHeavy.wav" or "Audio/RPG/Hit.wav")
		end)
	end
	Audio.PlayOneShot("Audio/RPG/Spin.wav", 1.0, 1.1)
end

function Skills:TickWhirl(C, Dt)
	-- 몸이 도는 것처럼 방향을 바꿔 가며 공격 자세
	C.SpinTimer = C.SpinTimer - Dt
	if C.SpinTimer <= 0 then
		C.SpinTimer = 0.075
		C.SpinIndex = (C.SpinIndex or 0) % 4 + 1
		self.Facing = SpinOrder[C.SpinIndex]
		self.Anim = ""
	end
end

-- ---- 부채꼴 (불꽃 베기·빙결 베기)
function Skills:StartCone(C, Pos)
	local Row = C.Row
	self:ElementSound(Row.Element)
	self:Later(Row.HitDelay, function()
		local P = self.entity:GetWorldPosition()
		local Angle = self.GM.ScreenAngle(C.Aim)
		local Col = self.GM.ElementColor[Row.Element]
		local Center = P + C.Aim * (Row.Range * 0.45)
		self.GM:SpawnFx("Slash", Center + Vector3(0, 12, -5), { Rotation = Angle, Blend = 2, Scale = 2.0, Color = Col })
		if Row.Element == "Fire" then
			for K = -1, 1 do
				local Dir = Rotate(C.Aim, K * 38)
				self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "FireBurst.eflipbook", Position = P + Dir * (Row.Range * 0.7) + Vector3(0, 14, -20),
				                      Blend = 0, Life = 0.42, Scale = 0.9 + (K == 0 and 0.3 or 0) })
			end
		else
			self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Frost.eflipbook", Position = P + C.Aim * (Row.Range * 0.65) + Vector3(0, 14, -20),
			                      Blend = 0, Life = 0.37, Scale = 1.5 })
		end
		self.entity:AddKnockback(C.Aim * 300, 0.08)
		Audio.PlayOneShot("Audio/RPG/Swing3.wav", 1.0, 0.9)
		local Hit = 0
		for _, S in ipairs(self.GM:FindEnemiesInCone(P, C.Aim, Row.Range, Row.Radius)) do
			local Away = Flat(S.entity:GetWorldPosition() - P)
			Away = Away:Length() > 1 and Away:Normalized() or C.Aim
			if self:SkillHit(C, S, Away, 900) then Hit = Hit + 1 end
		end
		self:HitFeel(Hit, 0.08, 10)
	end)
end

-- ---- 관통 돌진: 앞으로 미끄러지며 길 위의 적을 한 번씩 (돌진하는 동안 맞지 않음 — 대시 판정)
function Skills:StartRush(C, Pos)
	local Row = C.Row
	local Time = Row.Range / math.max(Row.Speed, 1)
	self.DashTimer = Time
	self.entity:AddKnockback(C.Aim * Row.Speed, Time)
	C.AfterTimer = 0
	self.GM:SpawnFx("Dust", Pos + Vector3(-C.Aim.X * 40, -C.Aim.Y * 40 + 4, -82), { FlipX = C.Aim.X < 0, Scale = 1.3 })
	Audio.PlayOneShot("Audio/RPG/Dash.wav", 1.0, 0.85)
	Audio.PlayOneShot("Audio/RPG/Swing3.wav", 1.0, 1.1)
end

function Skills:TickRush(C, Dt)
	local Row = C.Row
	self.DashTimer = math.max(0, self.DashTimer - Dt)
	local P = self.entity:GetWorldPosition()
	C.AfterTimer = C.AfterTimer - Dt
	if C.AfterTimer <= 0 then
		C.AfterTimer = 0.03
		local Slice, Atlas = self.Body:GetSpriteSlice()
		if Slice then
			self.GM:SpawnAfterimage(Atlas, Slice, self.Body:GetWorldPosition() + Vector3(0, -2, 0), self.Sprite.FlipX, { 1.0, 0.85, 0.55, 0.55 })
		end
	end
	if self.DashTimer <= 0 then return end
	local Hit = 0
	for _, S in ipairs(self.GM:FindEnemiesInLine(P - C.Aim * 40, C.Aim, 150, Row.Radius)) do
		if not C.Hit[S] then
			local Away = Vector3(-C.Aim.Y, C.Aim.X, 0)
			if Away:Dot(Flat(S.entity:GetWorldPosition() - P)) < 0 then Away = Away * -1 end
			if self:SkillHit(C, S, (Away + C.Aim):Normalized(), 700) then
				Hit = Hit + 1
				self.GM:SpawnFx("Thrust", S.entity:GetWorldPosition() + Vector3(0, 14, 0), { Rotation = self.GM.ScreenAngle(C.Aim), Blend = 2, Scale = { 1.0, 1.6 } })
			end
		end
	end
	self:HitFeel(Hit, 0.05, 7)
end

-- ---- 직선 (번개 찌르기): 길게 두 번
function Skills:StartLine(C, Pos)
	local Row = C.Row
	self:ElementSound(Row.Element)
	for I = 0, Row.Hits - 1 do
		self:Later(Row.HitDelay + I * 0.16, function()
			local P = self.entity:GetWorldPosition()
			local Angle = self.GM.ScreenAngle(C.Aim)
			self.GM:SpawnFx("Thrust", P + C.Aim * 30 + Vector3(0, 12, -8), { Rotation = Angle, Blend = 2, Scale = { Row.Range / 230.0, 1.8 }, Color = { 1, 0.95, 0.45, 1 } })
			self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Zap.eflipbook", Position = P + C.Aim * 40 + Vector3(0, 16, -6), Rotation = Angle,
			                      Blend = 2, Life = 0.2, Fade = true, Scale = { Row.Range / 384.0, 1.0 } })
			self.entity:AddKnockback(C.Aim * 320, 0.07)
			Audio.PlayOneShot("Audio/RPG/Swing3.wav", 1.0, 1.15 + I * 0.1)
			local Hit = 0
			for _, S in ipairs(self.GM:FindEnemiesInLine(P, C.Aim, Row.Range, Row.Radius)) do
				if self:SkillHit(C, S, C.Aim, 600) then
					Hit = Hit + 1
					self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Lightning.eflipbook", Position = S.entity:GetWorldPosition() + Vector3(0, 18, (S.Foot or -60)),
					                      Blend = 2, Life = 0.28, Scale = 0.7 })
				end
			end
			self:HitFeel(Hit, 0.06, 8, "Audio/RPG/HitHeavy.wav", 1.3)
		end)
	end
end

-- ---- 화살비: 부채꼴로 여러 발 (스킬 투사체)
function Skills:StartVolley(C, Pos)
	local Row = C.Row
	self:Later(Row.HitDelay, function()
		local P = self.entity:GetWorldPosition()
		local N = Row.Hits + C.Boost
		for K = 0, N - 1 do
			local Off = (K - (N - 1) * 0.5) * (40.0 / math.max(N - 1, 1))
			local Dir = Rotate(C.Aim, Off)
			self.GM:SpawnSkillShot({ Pos = P + Dir * 50 + Vector3(0, 10, 8), Dir = Dir, Speed = Row.Speed, Range = Row.Range, Damage = Row.Damage * C.Mult,
			                         Owner = self, Weapon = self.GM.Equipped, Combat = { Element = Row.Element }, HitRadius = Row.Radius, Knockback = 350,
			                         Fx = { Sprite = FxSprite, Slice = "Arrow", Rotation = self.GM.ScreenAngle(Dir), Blend = 0, Life = 3.0, Scale = 1.3,
			                                Color = { 1, 1, 0.85, 1 } } })
		end
		Audio.PlayOneShot("Audio/RPG/Swing2.wav", 1.0, 1.2)
		Audio.PlayOneShot("Audio/RPG/Swing1.wav", 0.8, 1.4)
	end)
end

-- ---- 관통 탄 (얼음 화살·얼음 창): 줄지은 적을 꿰뚫는다
function Skills:StartPierce(C, Pos)
	local Row = C.Row
	self:ElementSound(Row.Element)
	self:Later(Row.HitDelay, function()
		local P = self.entity:GetWorldPosition()
		local Dir = C.Aim
		local Fx
		if Row.Slot == "Magic" then
			Fx = { Sprite = CombatSprite, Flipbook = Book .. "IceLance.eflipbook", Rotation = self.GM.ScreenAngle(Dir), Blend = 0, Life = 3.0, Scale = 1.5 }
		else
			Fx = { Sprite = FxSprite, Slice = "Arrow", Rotation = self.GM.ScreenAngle(Dir), Blend = 0, Life = 3.0, Scale = 1.6, Color = { 0.65, 0.9, 1.0, 1 } }
		end
		local GM = self.GM
		GM:SpawnSkillShot({ Pos = P + Dir * 50 + Vector3(0, 10, 8), Dir = Dir, Speed = Row.Speed, Range = Row.Range, Damage = Row.Damage * C.Mult, Owner = self,
		                    Weapon = GM.Equipped, Combat = { Element = Row.Element, Status = Row.Status, Chance = Row.StatusChance }, bPierce = true,
		                    HitRadius = 40, Knockback = 450, Fx = Fx,
		                    TrailFx = function(Shot)
			                    GM:SpawnSprite({ Sprite = CombatSprite, Slice = "FrostBit0", Position = Shot.Pos - Shot.Dir * 30 + Vector3(0, -2, 0), Blend = 2,
			                                     Life = 0.25, Fade = true, Scale = 0.9, Grow = -2.0 })
		                    end,
		                    OnHit = function(Shot, S)
			                    GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Frost.eflipbook", Position = S.entity:GetWorldPosition() + Vector3(0, 20, 0),
			                                     Blend = 0, Life = 0.37, Scale = 1.0 })
			                    Audio.PlayOneShot("Audio/RPG/Block.wav", S.entity:GetWorldPosition(), 0.7, 1.8)
		                    end })
		Audio.PlayOneShot("Audio/RPG/Swing2.wav", 1.0, 1.3)
	end)
end

-- ---- 빛의 기둥: 가장 가까운 적 자리에 마법진 → 빛기둥 → 여러 번
function Skills:StartPillar(C, Pos)
	local Row = C.Row
	local Target = self.GM:NearestEnemy(Pos, Row.Range)
	local Spot
	if Target then
		local T = Target.entity:GetWorldPosition()
		Spot = Vector3(T.X, T.Y, T.Z + (Target.Foot or -60))
	else
		Spot = Pos + C.Aim * 320 + Vector3(0, 0, -85)
	end
	C.Spot = Spot
	self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "MagicCircle.eflipbook", Position = Spot + Vector3(0, 0, 3), Flat = true, Blend = 2,
	                      Life = Row.HitDelay + 0.75, Fade = true, Scale = Row.Radius / 160.0, Color = { 1, 0.9, 0.55, 1 } })
	self:ElementSound(Row.Element)
	self:Later(Row.HitDelay, function()
		self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "HolyBeam.eflipbook", Position = Spot + Vector3(0, 10, 0), Blend = 2, Life = 0.55,
		                      Scale = { Row.Radius / 95.0, 1.6 } })
		self.GM:SpawnFx("Ring", Spot + Vector3(0, 0, 4), { Flat = true, Blend = 2, Scale = 0.5, Grow = 4.0, Life = 0.4, Fade = true, Color = { 1, 0.92, 0.6, 1 } })
		self.GM:AddShake(7, 0.2)
		Audio.PlayOneShot("Audio/RPG/Spin.wav", Spot, 1.0, 1.35)
	end)
	for I = 0, Row.Hits + C.Boost - 1 do
		self:Later(Row.HitDelay + 0.04 + I * 0.15, function()
			local Hit = 0
			for _, S in ipairs(self.GM:FindEnemies(Spot, Row.Radius)) do
				if self:SkillHit(C, S, Vector3(0, 1, 0), 120) then Hit = Hit + 1 end
			end
			self:HitFeel(Hit, 0.03, 4, "Audio/RPG/Hit.wav", 1.3 + I * 0.1)
		end)
	end
end

-- ---- 연쇄 번개: 가까운 적에서 적으로
function Skills:StartChain(C, Pos)
	local Row = C.Row
	self:ElementSound(Row.Element)
	self:Later(Row.HitDelay, function()
		local From = self.entity:GetWorldPosition() + Vector3(0, 0, 20)
		local Hit = {}
		local Cur = self.GM:NearestEnemy(From, Row.Range)
		local N = Row.Hits + C.Boost
		for K = 1, N do
			if not Cur then break end
			Hit[Cur] = true
			local Target = Cur
			local A = From
			local B = Target.entity:GetWorldPosition() + Vector3(0, 0, math.max(10, (Target.HitHeight or 60) - 30))
			self:Later((K - 1) * 0.08, function()
				if not Target.entity:IsValid() then return end
				self.GM:SpawnBeam(A, B, 0.24)
				self.GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Lightning.eflipbook", Position = Target.entity:GetWorldPosition() + Vector3(0, 18, (Target.Foot or -60)),
				                      Blend = 2, Life = 0.28, Scale = 0.9 })
				local Away = Flat(B - A)
				Away = Away:Length() > 1 and Away:Normalized() or C.Aim
				if not Target.bDead and self:SkillHit(C, Target, Away, 300) then self:HitFeel(1, 0.04, 6, "Audio/RPG/HitHeavy.wav", 1.4 + K * 0.06) end
			end)
			From = B
			Cur = self.GM:NearestEnemy(B, Row.Radius, function(S) return not Hit[S] end)
		end
	end)
end

-- ---- 폭발 탄 (화염구): 맞거나 사거리 끝에서 터져 둘레까지
function Skills:StartBolt(C, Pos)
	local Row = C.Row
	local GM = self.GM
	local Player = self
	self:ElementSound(Row.Element)
	self:Later(Row.HitDelay, function()
		local P = self.entity:GetWorldPosition()
		local Dir = C.Aim
		local function Explode(Shot, Center)
			GM:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "FireBurst.eflipbook", Position = Center + Vector3(0, 20, 0), Blend = 0, Life = 0.44,
			                 Scale = Row.Radius / 90.0 })
			GM:SpawnFx("Ring", Center + Vector3(0, 0, -60), { Flat = true, Blend = 2, Scale = 0.4, Grow = 4.5, Life = 0.3, Fade = true, Color = { 1, 0.6, 0.3, 1 } })
			GM:AddShake(7, 0.16)
			Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", Center, 1.0, 0.72)
			Audio.PlayOneShot("Audio/RPG/Spin.wav", Center, 0.7, 0.55)
			for _, S in ipairs(GM:FindEnemies(Center, Row.Radius)) do
				if not Shot.Hits[S] then
					Shot.Hits[S] = true
					local Away = Flat(S.entity:GetWorldPosition() - Center)
					Away = Away:Length() > 1 and Away:Normalized() or Dir
					local Damage, bCrit = Player:RollDamage(Row.Damage * C.Mult * 0.6)
					GM:HitEnemy(S, Damage, Away, 500, bCrit, GM.Equipped, { Element = Row.Element, Status = Row.Status, Chance = Row.StatusChance * 0.5 })
				end
			end
		end
		GM:SpawnSkillShot({ Pos = P + Dir * 50 + Vector3(0, 10, 10), Dir = Dir, Speed = Row.Speed, Range = Row.Range, Damage = Row.Damage * C.Mult, Owner = self,
		                    Weapon = GM.Equipped, Combat = { Element = Row.Element, Status = Row.Status, Chance = Row.StatusChance }, HitRadius = 40, Knockback = 650,
		                    Fx = { Sprite = CombatSprite, Flipbook = Book .. "Fireball.eflipbook", Rotation = GM.ScreenAngle(Dir), Blend = 0, Life = 3.0, Scale = 1.7 },
		                    TrailFx = function(Shot)
			                    GM:SpawnSprite({ Sprite = CombatSprite, Slice = "Flame1", Position = Shot.Pos - Shot.Dir * 40 + Vector3(0, -2, -10), Blend = 2,
			                                     Life = 0.2, Fade = true, Scale = 1.1, Grow = -3.0, Color = { 1, 0.7, 0.4, 0.9 } })
		                    end,
		                    OnHit = function(Shot, S) Explode(Shot, Shot.Pos) end,
		                    OnEnd = function(Shot) Explode(Shot, Shot.Pos) end })
		Audio.PlayOneShot("Audio/RPG/Swing2.wav", 1.0, 0.8)
	end)
end

-- ---- 치유: 최대 HP 일부 + 독·화상·빙결 회복
function Skills:StartHeal(C, Pos)
	local Row = C.Row
	self:Later(Row.HitDelay, function()
		local P = self.entity:GetWorldPosition()
		self.GM:SpawnSprite({ Sprite = FxSprite, Slice = "Pillar", Position = P + Vector3(0, 20, -85), Blend = 2, Life = 0.8, Fade = true,
		                      Color = { 0.55, 1, 0.65, 0.95 }, Scale = { 1.4, 1.7 } })
		self:Heal(math.floor(self.MaxHealth * 0.35 + 0.5), 0)
		self.GM:CureStatus(self)
		Audio.PlayOneShot(self.GM.Sounds.Potion, 1.0, 1.15)
		Audio.PlayOneShot("Asset/Kenney_InterfaceSounds/confirm.wav", 0.7, 1.3)
	end)
end

return Skills
