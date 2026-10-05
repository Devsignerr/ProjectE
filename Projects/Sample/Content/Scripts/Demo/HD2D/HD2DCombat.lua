-- HD-2D 데모 관리자 확장 ④ 전투 깊이 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self·적·플레이어 스크립트에).
--   공격 속성: 무기 표 Element(베기 Slash / 찌르기 Pierce / 활 Bow / 빛 Light) + 스킬 표 Element(불 Fire / 얼음 Ice / 번개 Thunder / 빛 Light).
--   약점·실드·브레이크(옥토패스식): 적 표 Weakness·Shield. 약점으로 맞히면 실드 -1(처음 맞힌 약점은 공개 — 일행 저장 Combat.Revealed),
--     0이 되면 브레이크(BreakTime초 기절 · 받는 피해 × Balance.BreakDamage · 금빛 번쩍 + 실드 깨짐 + "BREAK!"), 끝나면 실드가 다시 찬다.
--     보스는 2단계(Phase 2)가 되면 약점·실드가 Weakness2·Shield2로 바뀐다 (공개 키 "<행>#2"). 약점 공격은 피해 × Balance.WeakDamage.
--   상태 이상: 독(지속 피해 — 쓰러뜨리지는 않음)·화상(지속 피해)·빙결(이동·동작 느려짐)·기절(행동 불가). 적 → 플레이어는 적 표 Inflict 또는
--     투사체·장판 Opt.Status, 플레이어 → 적은 스킬 표 Status. 머리 위 아이콘은 HUD 태그(HD2DCombatHud.lua), 해독제(Kind Cure)·치유 마법·엘릭서가 푼다.
--   적 스크립트 연결: OnStart 끝에 GM:InitEnemyCombat(self), OnUpdate 첫머리에 `local bSkip; bSkip, Dt = GM:UpdateEnemyCombat(self, Dt)` (기절·브레이크면 건너뜀).
--   스킬 투사체(화염구·얼음 창·화살비)는 관리자 투사체와 따로 여기서 움직인다(UpdateCombat — 관통·폭발·속성).
--   동료(엘라 — HD2DCompanion.lua): 마을에서는 관리자 속성 Companion 자리에 서 있다가(마을 사람 목록 Role "Companion") 촌장 퀘스트 뒤 영입,
--     영입 여부·체력은 일행 저장(Combat.Companion)이라 맵 이동·이어하기에서 플레이어 곁에 다시 만든다(UpdateCombat 첫 갱신 — 도착 순간이동 뒤).
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local Combat = {}

local FxSprite = "Sprites/HD2D/Fx.esprite"
local CombatSprite = "Sprites/HD2D/Combat.esprite"
local Book = "Sprites/HD2D/Combat_"

Combat.ElementNames = { Slash = "베기", Pierce = "찌르기", Bow = "활", Fire = "불", Ice = "얼음", Thunder = "번개", Light = "빛" }
Combat.StatusNames = { Poison = "독", Burn = "화상", Freeze = "빙결", Stun = "기절" }
-- 상태 이상 수치: 지속(초)·피해 간격(초)·느려짐 배율. 보스는 기절·빙결 시간이 반 (Resist)
local StatusPlayer = {
	Poison = { Time = 6.0, Tick = 1.0 }, Burn = { Time = 4.0, Tick = 0.5 }, Freeze = { Time = 2.5, Slow = 0.55 }, Stun = { Time = 0.9 },
}
local StatusEnemy = {
	Poison = { Time = 6.0, Tick = 1.0 }, Burn = { Time = 4.0, Tick = 0.5 }, Freeze = { Time = 3.0, Slow = 0.45 }, Stun = { Time = 1.4 },
}
local StatusColor = {
	Poison = { 0.78, 0.5, 1.0, 1 }, Burn = { 1.0, 0.6, 0.25, 1 }, Freeze = { 0.6, 0.86, 1.0, 1 }, Stun = { 1.0, 0.92, 0.4, 1 },
}
local ElementColor = {
	Slash = { 1, 0.95, 0.85, 1 }, Pierce = { 1, 0.85, 0.6, 1 }, Bow = { 0.8, 1, 0.7, 1 }, Fire = { 1, 0.55, 0.2, 1 }, Ice = { 0.55, 0.85, 1, 1 },
	Thunder = { 1, 0.92, 0.35, 1 }, Light = { 1, 0.95, 0.7, 1 },
}
Combat.ElementColor = ElementColor
Combat.StatusColor = StatusColor

local function Flat(V) return Vector3(V.X, V.Y, 0) end

-- ================================================================ 시작 · 저장
function Combat:InitCombat()
	self.Revealed = {}      -- 적 행(보스 2단계 "<행>#2") → { 속성 = true }
	self.SkillShots = {}
	self.CompanionRecruited, self.CompanionHealth = false, nil
	local R = self.Report
	R.Breaks, R.BossBreaks, R.Reveals, R.WeakHits, R.EliteKills, R.Cures = 0, 0, 0, 0, 0, 0
	R.Skills, R.StatusOnEnemy, R.StatusOnPlayer, R.ElementHits = {}, {}, {}, {}
	R.CompanionCasts, R.CompanionHeals, R.CompanionDowns, R.CompanionHits = 0, 0, 0, 0
end

function Combat:BuildCombatSave()
	local Rev = {}
	for Key, Set in pairs(self.Revealed or {}) do
		local List = {}
		for Elem in pairs(Set) do List[#List + 1] = Elem end
		table.sort(List)
		Rev[Key] = List
	end
	local C = self.Companion
	return { Revealed = Rev, Companion = { Recruited = self.CompanionRecruited == true,
	                                       Health = (C and C.Mode == "Follow") and math.max(1, math.floor(C.Health)) or self.CompanionHealth } }
end

function Combat:ApplyCombatSave(Data)
	self.Revealed = {}
	for Key, List in pairs((Data and Data.Revealed) or {}) do
		self.Revealed[Key] = {}
		for _, Elem in ipairs(List) do self.Revealed[Key][Elem] = true end
	end
	local Comp = Data and Data.Companion
	self.CompanionRecruited = Comp ~= nil and Comp.Recruited == true
	self.CompanionHealth = Comp and Comp.Health or nil
	self:RefreshCompanionMode()
end

-- 저장 비교용 요약 (Party:StateSignature가 붙인다)
function Combat:CombatSignature()
	local Keys = {}
	for Key, Set in pairs(self.Revealed or {}) do
		local List = {}
		for Elem in pairs(Set) do List[#List + 1] = Elem end
		table.sort(List)
		Keys[#Keys + 1] = Key .. ":" .. table.concat(List, "/")
	end
	table.sort(Keys)
	return table.concat(Keys, ",") .. (self.CompanionRecruited and "+Ella" or "")
end

-- 도감 약점 칸 (HD2DMeta.lua 훅을 덮어쓴다): 공개한 약점은 이름, 아직 모르는 칸은 "？" — 하나도 모르면 nil(도감이 "？？？")
--   보스는 1단계 / 2단계("<행>#2")를 나눠 보인다
function Combat:BestiaryWeakness(Kind)
	local Row = D.Enemy(Kind)
	if not Row then return nil end
	local bAny = false
	local function Line(Key, List)
		local Parts = {}
		for _, Elem in ipairs(List or {}) do
			if self:IsRevealed(Key, Elem) then
				Parts[#Parts + 1] = Combat.ElementNames[Elem] or Elem
				bAny = true
			else
				Parts[#Parts + 1] = "？"
			end
		end
		return table.concat(Parts, " · ")
	end
	local Text = Line(Kind, Row.Weakness)
	if (Row.Shield2 or 0) > 0 and #(Row.Weakness2 or {}) > 0 then
		Text = "1단계 " .. Text .. "  /  2단계 " .. Line(Kind .. "#2", Row.Weakness2)
	end
	return bAny and string.format("%s  (실드 %d)", Text, Row.Shield or 0) or nil
end

function Combat:IsRevealed(Key, Elem)
	local Set = self.Revealed[Key]
	return Set ~= nil and Set[Elem] == true
end

-- ================================================================ 적 연결
function Combat:InitEnemyCombat(S)
	local Row = S.Row
	S.Phase = S.Phase or 1
	S.CombatPhase = S.Phase
	S.WeakKey = S.Kind
	S.Weak = Row.Weakness or {}
	S.MaxShield = Row.Shield or 0
	S.Shield = S.MaxShield
	S.BreakTimer, S.bBroken = 0, false
	S.Status = {}
	S.StatusTick = {}
	S.Mover = S.entity:GetComponent("CharacterMovementComponent")
	S.BaseSpeed = S.Mover and S.Mover.MaxWalkSpeed or 0
	local Hp = S.Visual and S.Visual:FindChild("HpBack")
	S.TagZ = (Hp and Hp:GetPosition().Z or ((S.Foot or -60) + 150)) + 28
	if S.Visual then
		S.Aura = S.Visual:FindChild("Aura")
		S.AuraSprite = S.Aura and S.Aura:GetComponent("SpriteComponent") or nil
	end
	S.HitOpt = (Row.Inflict ~= nil and Row.Inflict ~= "") and { Status = Row.Inflict, Chance = Row.InflictChance } or nil
	S.BodyBase = S.BodyBase or (S.Body and S.Body:GetPosition())
end

-- 적 OnUpdate 첫머리: (건너뛸지, 이번 프레임 dt) — 기절·브레이크면 AI를 건너뛰고, 빙결이면 dt를 줄여 동작이 느려진다
function Combat:UpdateEnemyCombat(S, Dt)
	if S.bDormant or S.bDead then return false, Dt end
	-- 보스 2단계: 약점·실드가 바뀐다 (브레이크 중이면 브레이크가 끝난 뒤 — 깨 놓은 기회를 빼앗지 않는다)
	if (S.Phase or 1) ~= S.CombatPhase and not S.bBroken then
		S.CombatPhase = S.Phase
		if (S.Row.Shield2 or 0) > 0 then
			S.Weak = (#(S.Row.Weakness2 or {}) > 0) and S.Row.Weakness2 or S.Weak
			S.WeakKey = S.Kind .. "#2"
			S.MaxShield = S.Row.Shield2
			S.Shield = S.MaxShield
			self:Hud():Toast("UI/Demo/HD2D/Combat/ElemUnknown.png", "약점이 바뀌었다!")
			Log.Info(string.format("[HD2D] %s 2단계: 약점 %s, 실드 %d", S.Row.DisplayName, table.concat(S.Weak, "/"), S.MaxShield))
		end
	end
	self:TickStatus(S, Dt, false)
	if S.bDead then return true, Dt end
	local Pos = S.entity:GetWorldPosition()
	if S.bBroken or S.Status.Stun then self:TickSkippedEnemy(S, Dt) end
	if S.bBroken then
		S.BreakTimer = S.BreakTimer - Dt
		if S.BreakStars and S.BreakStars.Entity then S.BreakStars.Entity:SetPosition(Pos + Vector3(0, 8, S.TagZ - 40)) end
		-- 나는 적은 바닥으로 떨어져 있다
		if S.Shape and S.Shape.Lift and S.Shape.Lift > 0 and S.BodyBase then
			local Drop = math.min(1.0, (S.BreakAge or 0) / 0.25)
			S.Body:SetPosition(S.BodyBase - Vector3(0, 0, (S.Shape.Lift - 25) * Drop))
		end
		S.BreakAge = (S.BreakAge or 0) + Dt
		if S.BreakTimer <= 0 then self:EndBreak(S, false) end
		return true, Dt
	end
	if S.Status.Stun then
		if S.StunStars and S.StunStars.Entity then S.StunStars.Entity:SetPosition(Pos + Vector3(0, 8, S.TagZ - 40)) end
		return true, Dt
	end
	if S.Status.Freeze then
		return false, Dt * (S.bBoss and 0.7 or StatusEnemy.Freeze.Slow)
	end
	return false, Dt
end

-- AI를 건너뛰는 동안에도 적 스크립트가 하던 피격 번쩍임·체력바 시간은 흘린다
function Combat:TickSkippedEnemy(S, Dt)
	if (S.Flash or 0) > 0 then
		S.Flash = S.Flash - Dt
		S.Sprite.FlashColor = Vector4(1, 1, 1, S.Flash > 0 and math.min(0.9, S.Flash * 10) or 0)
	end
	if (S.BarTime or 0) > 0 and S.ShowBar then
		S.BarTime = S.BarTime - Dt
		if S.BarTime <= 0 then S:ShowBar(false) end
	end
	S.ContactCooldown = math.max(0, (S.ContactCooldown or 0) - Dt)
end

-- ================================================================ 명중 (HD2DGame:HitEnemy가 부른다)
-- 반환: { Damage, Element, bWeak, bBroken } — 피해에 약점·브레이크 배율을 곱한다
function Combat:ResolveCombatHit(S, Damage, WeaponId, Hit)
	local Element = Hit and Hit.Element
	if not Element and WeaponId then
		local W = D.Weapon(WeaponId)
		Element = W and W.Element or nil
	end
	local Info = { Damage = Damage, Element = Element, bWeak = false, bBroken = S.bBroken == true, Hit = Hit }
	if S.bDead or S.bDormant or not Element then return Info end
	for _, E in ipairs(S.Weak or {}) do
		if E == Element then Info.bWeak = true end
	end
	local B = D.Balance()
	local Scale = 1.0
	if Info.bWeak then Scale = Scale * (B.WeakDamage or 1.25) end
	if Info.bBroken then Scale = Scale * (B.BreakDamage or 1.5) else Scale = Scale * (S.Row.Guard or 1.0) end -- 보스·정예는 실드가 있을 때 단단하다
	Info.Damage = math.max(1, math.floor(Damage * Scale + 0.5))
	return Info
end

-- 맞은 뒤 (TakeHit 성공): 약점 공개·실드·브레이크·상태 이상
function Combat:AfterCombatHit(S, Info)
	local R = self.Report
	if Info.Element then R.ElementHits[Info.Element] = (R.ElementHits[Info.Element] or 0) + 1 end
	if S.bDead then return end -- 정예 처치 수는 관리자 OnEnemyKilled
	if Info.bWeak then
		R.WeakHits = R.WeakHits + 1
		local Pos = S.entity:GetWorldPosition()
		if not self:IsRevealed(S.WeakKey, Info.Element) then
			self.Revealed[S.WeakKey] = self.Revealed[S.WeakKey] or {}
			self.Revealed[S.WeakKey][Info.Element] = true
			R.Reveals = R.Reveals + 1
			S.RevealFlash = { Element = Info.Element, Time = 0.5 }
			Audio.PlayOneShot("Asset/Kenney_InterfaceSounds/confirm.wav", Pos, 0.6, 1.6)
			Log.Info(string.format("[HD2D] 약점 발견: %s ← %s", S.Row.DisplayName, Combat.ElementNames[Info.Element] or Info.Element))
		end
		if not S.bBroken and S.Shield > 0 then
			S.Shield = S.Shield - 1
			S.ShieldPulse = 0.25
			Audio.PlayOneShot("Audio/RPG/Block.wav", Pos, 0.55, 1.35 + 0.08 * (S.MaxShield - S.Shield))
			if S.Shield <= 0 then self:StartBreak(S) end
		end
	end
	local H = Info.Hit
	if H and H.Status and H.Status ~= "" and not S.bDead and self:Random() < (H.Chance or 1.0) then
		self:ApplyStatus(S, H.Status)
	end
end

function Combat:StartBreak(S)
	local Row = S.Row
	S.bBroken = true
	S.BreakTimer = Row.BreakTime or 3.5
	S.BreakAge = 0
	S.BreakFlash = 0.3
	local R = self.Report
	R.Breaks = R.Breaks + 1
	if S.bBoss then R.BossBreaks = R.BossBreaks + 1 end
	-- 예비 동작 끊기 (경고 원·돌진 예고)
	self:KillFx(S.Warn)
	if S.Body and S.BodyBase and not (S.Shape and S.Shape.Lift and S.Shape.Lift > 0) then S.Body:SetPosition(S.BodyBase) end
	if S.SetState then S:SetState(S.bBoss and "Recover" or "Idle", 0.2) end
	if S.Play then S:Play("Idle") end
	if S.Status.Stun then self:ClearStatus(S, "Stun") end
	local Pos = S.entity:GetWorldPosition()
	local Head = Pos + Vector3(0, 16, S.TagZ - 30)
	local Ground = Pos + Vector3(0, 0, (S.Foot or -60) + 3)
	-- 연출: 실드가 깨지며 금빛 폭발 + 바닥 고리 + 기절 별 + 화면 번쩍 + 멈춤 + "BREAK!"
	self:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Shatter.eflipbook", Position = Head, Blend = 0, Life = 0.42, Scale = S.bBoss and 1.6 or 1.15 })
	self:SpawnFx("Burst", Pos + Vector3(0, 20, (S.HitHeight or 60) - 10), { Blend = 2, Scale = S.bBoss and 3.6 or 2.4, Color = { 1, 0.85, 0.4, 1 } })
	self:SpawnFx("Ring", Ground, { Flat = true, Blend = 2, Scale = 0.5, Grow = S.bBoss and 7.0 or 5.0, Life = 0.4, Fade = true, Color = { 1, 0.85, 0.45, 1 } })
	for I = 0, 3 do
		local A = I / 4 * math.pi * 2 + 0.4
		self:SpawnFx("Sparkle", Head + Vector3(math.cos(A) * 70, 10, math.sin(A) * 50), { Blend = 2, Scale = 1.2, Life = 0.33 + I * 0.04, Color = { 1, 0.9, 0.5, 1 } })
	end
	S.BreakStars = self:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Stars.eflipbook", Position = Pos + Vector3(0, 8, S.TagZ - 40), Blend = 0,
	                                  Life = S.BreakTimer, Scale = S.bBoss and 2.4 or 1.6 })
	self:Hud():ShowBreakBanner(Pos + Vector3(0, 30, (S.HitHeight or 60) - 20), S.bBoss) -- 몸 가운데 (머리 위 태그를 가리지 않게)
	self:Hud():ScreenFlash(S.bBoss and 0.42 or 0.28, 0.22)
	Game.HitStop(S.bBoss and 0.2 or 0.11)
	self:AddShake(S.bBoss and 16 or 10, 0.3)
	Audio.PlayOneShot("Audio/RPG/Block.wav", Pos, 1.0, 0.62)
	Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", Pos, 1.0, 0.8)
	Timer.After(0.09, function() Audio.PlayOneShot("Asset/Kenney_InterfaceSounds/confirm.wav", 0.8, 1.9) end, { Unscaled = true })
	Log.Info(string.format("[HD2D] 브레이크: %s (%.1f초)", Row.DisplayName, S.BreakTimer))
end

-- 브레이크 끝: 실드가 다시 찬다 (bQuiet = 2단계 전환처럼 연출 없이)
function Combat:EndBreak(S, bQuiet)
	S.bBroken = false
	S.BreakTimer = 0
	S.Shield = S.MaxShield
	self:KillFx(S.BreakStars)
	S.BreakStars = nil
	if S.Body and S.BodyBase then S.Body:SetPosition(S.BodyBase) end
	if S.SetState and not bQuiet then S:SetState(S.bBoss and "Recover" or "Idle", 0.35) end
	if not bQuiet and S.entity:IsValid() then
		local Pos = S.entity:GetWorldPosition()
		self:SpawnFx("Sparkle", Pos + Vector3(0, 20, S.TagZ - 40), { Blend = 2, Scale = 1.4, Color = { 0.7, 0.85, 1, 1 } })
		Audio.PlayOneShot("Asset/Kenney_RPGAudio/equip.wav", Pos, 0.7, 1.25)
	end
end

-- ================================================================ 상태 이상
-- T = 적 스크립트 또는 플레이어. Kind = Poison/Burn/Freeze/Stun
function Combat:ApplyStatus(T, Kind, Time)
	local bPlayer = T.IsPlayerScript == true
	local Def = (bPlayer and StatusPlayer or StatusEnemy)[Kind]
	if not Def then return end
	T.Status = T.Status or {}
	T.StatusTick = T.StatusTick or {}
	local Dur = Time or Def.Time
	if not bPlayer and T.bBoss and (Kind == "Stun" or Kind == "Freeze") then Dur = Dur * 0.5 end
	if not bPlayer and Kind == "Stun" and T.bBroken then return end -- 브레이크 중엔 이미 쓰러져 있다
	local bNew = T.Status[Kind] == nil
	T.Status[Kind] = math.max(T.Status[Kind] or 0, Dur)
	if bNew then T.StatusTick[Kind] = Def.Tick or 0 end
	local R = self.Report
	local Bucket = bPlayer and R.StatusOnPlayer or R.StatusOnEnemy
	Bucket[Kind] = (Bucket[Kind] or 0) + 1
	local Pos = T.entity:GetWorldPosition()
	local Head = Pos + Vector3(0, 0, bPlayer and 95 or (T.TagZ - 20))
	if bNew then
		self:DamageNumber(Head, Combat.StatusNames[Kind] .. "!", StatusColor[Kind], 0.85)
		if Kind == "Freeze" then
			self:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Frost.eflipbook", Position = Pos + Vector3(0, 20, 0), Blend = 0, Life = 0.37,
			                   Scale = bPlayer and 1.0 or 1.3 })
			Audio.PlayOneShot("Audio/RPG/Block.wav", Pos, 0.7, 1.7)
		elseif Kind == "Stun" then
			local Stars = self:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Stars.eflipbook", Position = Head + Vector3(0, 8, 10), Blend = 0,
			                                 Life = Dur, Scale = bPlayer and 1.2 or 1.4 })
			if bPlayer then T.StunStars = Stars else self:KillFx(T.StunStars); T.StunStars = Stars end
			Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", Pos, 0.7, 1.35)
		elseif Kind == "Burn" then
			Audio.PlayOneShot("Audio/RPG/Spin.wav", Pos, 0.6, 0.65)
		elseif Kind == "Poison" then
			Audio.PlayOneShot("Audio/RPG/Dodge.wav", Pos, 0.7, 0.7)
		end
		Log.Info(string.format("[HD2D] 상태 이상: %s ← %s", bPlayer and "플레이어" or T.Row.DisplayName, Combat.StatusNames[Kind]))
	end
	if bPlayer and Kind == "Freeze" then T:RecalcStats() end
	if not bPlayer and Kind == "Freeze" and T.Mover then T.Mover.MaxWalkSpeed = T.BaseSpeed * (T.bBoss and 0.7 or Def.Slow) end
	if Kind == "Stun" and not bPlayer then
		self:KillFx(T.Warn)
		if T.SetState then T:SetState(T.bBoss and "Recover" or "Idle", 0.2) end
		if T.Body and T.BodyBase and not (T.Shape and T.Shape.Lift and T.Shape.Lift > 0) then T.Body:SetPosition(T.BodyBase) end
	end
end

function Combat:ClearStatus(T, Kind)
	if not T.Status or not T.Status[Kind] then return end
	T.Status[Kind] = nil
	if Kind == "Stun" then
		self:KillFx(T.StunStars)
		T.StunStars = nil
	elseif Kind == "Freeze" then
		if T.IsPlayerScript then T:RecalcStats() elseif T.Mover then T.Mover.MaxWalkSpeed = T.BaseSpeed end
	end
end

-- 독·화상·빙결을 푼다 (해독제·치유·엘릭서). 푼 것이 있으면 true
function Combat:CureStatus(T)
	local bAny = false
	for _, Kind in ipairs({ "Poison", "Burn", "Freeze" }) do
		if T.Status and T.Status[Kind] then
			self:ClearStatus(T, Kind)
			bAny = true
		end
	end
	if bAny then
		self.Report.Cures = self.Report.Cures + 1
		local Pos = T.entity:GetWorldPosition()
		self:SpawnFx("Sparkle", Pos + Vector3(0, 30, 50), { Blend = 2, Scale = 1.3, Color = { 0.7, 1, 0.75, 1 } })
		self:DamageNumber(Pos + Vector3(0, 0, 100), "회복", { 0.7, 1, 0.75, 1 }, 0.85)
		Log.Info("[HD2D] 상태 이상 회복")
	end
	return bAny
end

function Combat:HasStatus(T, Kind)
	return T.Status ~= nil and T.Status[Kind] ~= nil
end

-- 지속 피해·시간 (적·플레이어 공용)
function Combat:TickStatus(T, Dt, bPlayer)
	if not T.Status then return end
	local Pos = nil
	for Kind, Left in pairs(T.Status) do
		Left = Left - Dt
		if Left <= 0 then
			self:ClearStatus(T, Kind)
		else
			T.Status[Kind] = Left
			local Def = (bPlayer and StatusPlayer or StatusEnemy)[Kind]
			if Def.Tick then
				T.StatusTick[Kind] = (T.StatusTick[Kind] or Def.Tick) - Dt
				if T.StatusTick[Kind] <= 0 then
					T.StatusTick[Kind] = Def.Tick
					Pos = Pos or T.entity:GetWorldPosition()
					self:StatusDamage(T, Kind, Pos, bPlayer)
					if T.bDead or not T.entity:IsValid() then return end
				end
			end
			-- 몸에 붙는 작은 효과 (불꽃·거품·서리)
			T.StatusFx = (T.StatusFx or 0) - Dt
			if T.StatusFx <= 0 and (Kind == "Burn" or Kind == "Poison" or Kind == "Freeze") then
				T.StatusFx = Kind == "Freeze" and 0.45 or 0.3
				Pos = Pos or T.entity:GetWorldPosition()
				local Seed = (self:Random() - 0.5)
				local Height = bPlayer and 40 or math.max(20, (T.HitHeight or 60) - 20)
				local At = Pos + Vector3(Seed * (bPlayer and 50 or (T.Radius or 50) * 1.2), 22, (bPlayer and -40 or 0) + Height * (0.5 + self:Random() * 0.6))
				local Name = (Kind == "Burn" and "Flame") or (Kind == "Poison" and "Bubble") or "FrostBit"
				self:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. Name .. ".eflipbook", Position = At, Blend = 0, Life = Kind == "Freeze" and 0.5 or 0.36,
				                   Fade = true, Vel = Vector3(0, 0, Kind == "Freeze" and 10 or 60), Scale = bPlayer and 1.0 or 1.2 })
			end
		end
	end
end

function Combat:StatusDamage(T, Kind, Pos, bPlayer)
	if bPlayer then
		local N = math.max(2, math.floor(T.MaxHealth * (Kind == "Poison" and 0.02 or 0.015) + 0.5))
		if Kind == "Poison" then N = math.min(N, T.Health - 1) end -- 독으로는 쓰러지지 않는다 (HP 1 남김)
		if N <= 0 then return end
		T:TakeDamage(N, Pos, { bNoKnockback = true, bNoInvuln = true, bNoDefense = true, Color = StatusColor[Kind], bQuiet = true })
		return
	end
	local Max = T.Row.MaxHealth
	local N = math.floor(math.max(2, math.min(T.bBoss and 12 or 10, Max * 0.03)) + 0.5)
	T.Health = T.Health - N
	self:DamageNumber(Pos + Vector3(0, 0, T.HitHeight or 60), tostring(N), StatusColor[Kind], 0.8)
	if T.bBoss then self:Hud():ShowBoss(T.Row.DisplayName, math.max(0, T.Health / Max)) end
	if T.Health <= 0 then
		T.Health = 1
		T:TakeHit(1, Vector3(0, 0, 0), 0) -- 쓰러짐 처리는 적 스크립트의 같은 경로로
	end
end

-- ================================================================ 플레이어 연결 (HD2DPlayer.lua)
function Combat:UpdatePlayerStatus(P, Dt)
	self:TickStatus(P, Dt, true)
	if P.StunStars and P.StunStars.Entity then P.StunStars.Entity:SetPosition(P.entity:GetWorldPosition() + Vector3(0, 8, 60)) end
end

function Combat:PlayerSpeedScale(P)
	return (P.Status and P.Status.Freeze) and StatusPlayer.Freeze.Slow or 1.0
end

-- 플레이어 몸 색 (피격 붉은빛은 플레이어가 먼저 정한다)
function Combat:PlayerTint(P)
	local S = P.Status
	if S then
		if S.Freeze then return Vector4(0.7, 0.9, 1.35, 1) end
		if S.Poison then
			local Pulse = 0.5 + 0.5 * math.sin(self.Time * 7.0)
			return Vector4(1.0 - 0.18 * Pulse, 0.82 - 0.12 * Pulse, 1.05, 1)
		end
		if S.Burn then return Vector4(1.15, 0.85, 0.75, 1) end
	end
	return Vector4(1, 1, 1, 1)
end

-- ================================================================ 동료
function Combat:RegisterCompanion(C) self.Companion = C end

function Combat:CanRecruitCompanion()
	return not self.CompanionRecruited and (self.QuestStage or 0) >= 1
end

-- 첫 갱신(플레이어가 자리 잡은 뒤): 영입했으면 플레이어 곁에, 아니면 마을 자리에 (마을 사람 목록에 넣어 E로 말을 건다)
function Combat:SpawnCompanion()
	local Player = self:GetPlayer()
	if self.bCompanionSpawned or not Player or not Player.bStarted or self.ArrivePos then return end
	self.bCompanionSpawned = true
	local Spot = self.Properties.Companion ~= "" and self.Properties.Companion or nil
	if Spot then
		local X, Y, Z = string.match(Spot, "([-%d%.]+),([-%d%.]+),([-%d%.]+)")
		self.CompanionSpot = Vector3(tonumber(X), tonumber(Y), tonumber(Z))
	end
	local At
	if self.CompanionRecruited then
		At = Player.entity:GetWorldPosition() + Vector3(-90, 110, 4)
	elseif self.CompanionSpot then
		At = self.CompanionSpot
	else
		return
	end
	local B = D.Balance()
	self.CompanionNpc = { Id = "Companion", Pos = self.CompanionSpot or At,
	                      Row = { DisplayName = "견습 마법사 " .. B.CompanionName, Role = "Companion", Portrait = "UI/Demo/HD2D/Portraits/Mage.png", Lines = {} } }
	Scene.SpawnPrefab("Prefabs/Demo/HD2D/Companion.eprefab", At, function(E)
		self.CompanionNpc.Entity = E
		if not self.CompanionRecruited then self.Npcs[#self.Npcs + 1] = self.CompanionNpc end
	end)
end

-- 저장을 불러온 뒤 (이어하기): 이미 있는 엘라를 영입 상태에 맞춘다
function Combat:RefreshCompanionMode()
	local C = self.Companion
	if not C then return end
	if self.CompanionRecruited and C.Mode ~= "Follow" then
		self:RemoveCompanionNpc()
		C:StartFollowing()
		if self.CompanionHealth then C.Health = math.max(1, math.min(C.MaxHealth, self.CompanionHealth)) end
	elseif not self.CompanionRecruited and C.Mode == "Follow" and self.CompanionSpot then
		C.Mode = "Npc"
		C.entity:SetPosition(self.CompanionSpot)
		self.Npcs[#self.Npcs + 1] = self.CompanionNpc
	end
end

function Combat:RemoveCompanionNpc()
	for I, N in ipairs(self.Npcs) do
		if N == self.CompanionNpc then
			table.remove(self.Npcs, I)
			return
		end
	end
end

-- 마을 사람 대화 (HD2DGame:TalkTo — Role "Companion")
function Combat:TalkCompanion(Npc)
	local B = D.Balance()
	if self:CanRecruitCompanion() then
		self:StartDialog("", B.CompanionLines, function() self:RecruitCompanion() end)
	else
		self:StartDialog("", B.CompanionWaitLines)
	end
end

function Combat:RecruitCompanion()
	if self.CompanionRecruited then return end
	self.CompanionRecruited = true
	self:RemoveCompanionNpc()
	local C = self.Companion
	if C then C:StartFollowing() else self.bCompanionSpawned = false end -- 이 맵에 엘라가 없으면 다음 갱신에 플레이어 곁에
	local B = D.Balance()
	self:Hud():Announce("동료 가입!", "견습 마법사 " .. B.CompanionName .. "이(가) 일행에 들어왔다", 2.6)
	self:Fanfare()
	if C then self:SpawnLevelFx(C.entity:GetWorldPosition()) end
	self.Report.CompanionRecruits = (self.Report.CompanionRecruits or 0) + 1
	Log.Info("[HD2D] 동료 영입: " .. B.CompanionName)
end

-- ================================================================ 매 프레임 (관리자 OnUpdate가 부른다): 스킬 투사체·동료
function Combat:UpdateCombat(Dt)
	if not self.bCompanionSpawned then self:SpawnCompanion() end
	local Keep = {}
	for _, P in ipairs(self.SkillShots) do
		P.Time = P.Time + Dt
		local Step = P.Speed * Dt
		P.Pos = P.Pos + P.Dir * Step
		P.Travel = P.Travel + Step
		if P.Fx.Entity then P.Fx.Entity:SetPosition(P.Pos) end
		P.Trail = P.Trail - Dt
		if P.Trail <= 0 and P.TrailFx then
			P.Trail = 0.035
			P.TrailFx(P)
		end
		local bDone = false
		for _, S in ipairs(self:FindEnemies(P.Pos, P.HitRadius)) do
			if not P.Hits[S] then
				P.Hits[S] = true
				local Damage, bCrit = P.Owner:RollDamage(P.Damage)
				if self:HitEnemy(S, Damage, P.Dir, P.Knockback, bCrit, P.Weapon, P.Combat) and P.Owner == self.Companion then
					self.Report.CompanionHits = self.Report.CompanionHits + 1
				end
				if P.OnHit then P.OnHit(P, S) end
				if not P.bPierce then
					bDone = true
					break
				end
			end
		end
		if not bDone and P.Travel >= P.Range then
			if P.OnEnd then P.OnEnd(P) end
			bDone = true
		end
		if bDone then self:KillFx(P.Fx) else Keep[#Keep + 1] = P end
	end
	self.SkillShots = Keep
end

-- 스킬 투사체 하나. Desc: Pos, Dir, Speed, Range, Damage(굴림 전), Owner(플레이어), Combat{Element, Status, Chance}, Weapon, Fx{…SpawnSprite 설명},
--   bPierce, HitRadius, Knockback, TrailFx(P), OnHit(P, S), OnEnd(P)
function Combat:SpawnSkillShot(Desc)
	local P = { Pos = Desc.Pos, Dir = Desc.Dir, Speed = Desc.Speed, Range = Desc.Range, Travel = 0, Time = 0, Damage = Desc.Damage, Owner = Desc.Owner,
	            Combat = Desc.Combat, Weapon = Desc.Weapon, bPierce = Desc.bPierce, HitRadius = Desc.HitRadius or 34, Knockback = Desc.Knockback or 400,
	            TrailFx = Desc.TrailFx, OnHit = Desc.OnHit, OnEnd = Desc.OnEnd, Hits = {}, Trail = 0 }
	Desc.Fx.Position = Desc.Pos
	P.Fx = self:SpawnSprite(Desc.Fx)
	self.SkillShots[#self.SkillShots + 1] = P
	self.Report.Projectiles = self.Report.Projectiles + 1
	return P
end

-- 효과 조각 하나를 두 점 사이에 (연쇄 번개 줄기) — 스프라이트 평면(XZ)에 화면에서 보이는 방향·길이로 눕힌다
function Combat:SpawnBeam(From, To, Life)
	local DX = To.X - From.X
	local Up = (To.Z - From.Z) - (To.Y - From.Y) * 0.47
	local Len = math.max(math.sqrt(DX * DX + Up * Up), 1)
	local Angle = math.deg(math.atan(Up, DX))
	return self:SpawnSprite({ Sprite = CombatSprite, Flipbook = Book .. "Zap.eflipbook", Position = From + Vector3(0, 30, 0), Rotation = Angle, Blend = 2,
	                          Life = Life or 0.22, Fade = true, Scale = { Len / (64 * 6.0), 1.2 }, Color = { 1, 0.95, 0.6, 1 } })
end

-- ================================================================ 화면 위 태그 대상 (HD2DCombatHud.lua가 매 프레임 묻는다)
-- 가까운 적 중 싸우는 중(발견·맞음·브레이크)인 것 최대 Max개: { S, Pos(머리 위 월드), … }
function Combat:CombatTagTargets(Center, Max)
	local List = {}
	for _, S in ipairs(self:AliveEnemies()) do
		if S.MaxShield and S.MaxShield > 0 then
			local Pos = S.entity:GetWorldPosition()
			local L = Flat(Pos - Center):Length()
			local Range = S.bBoss and 2200 or 1100
			if L < Range and (S.bAggro or S.bBoss or S.bBroken or (S.BarTime or 0) > 0 or L < 650) then
				List[#List + 1] = { S = S, Dist = L, Pos = Pos + Vector3(0, 0, S.TagZ) }
			end
		end
	end
	table.sort(List, function(A, B) return A.Dist < B.Dist end)
	while #List > Max do table.remove(List) end
	return List
end

-- 정예 윤곽·적 상태 색 (적 스크립트가 몸 색을 정한 뒤 — 관리자 OnLateUpdate)
function Combat:OnLateUpdate(Dt)
	local Now = self.Time or 0
	for _, S in pairs(self.Enemies) do
		if S.entity:IsValid() and not S.bDead and S.Sprite then
			if S.AuraSprite then
				local Slice, Atlas = S.Body:GetSpriteSlice()
				if Slice and S.AuraSprite.Slice ~= Slice then
					S.AuraSprite.Sprite = Atlas
					S.AuraSprite.Slice = Slice
				end
				S.Aura:SetSpriteFlip(S.Sprite.FlipX == true, false)
				S.Aura:SetPosition(S.Body:GetPosition() + Vector3(0, -4, -3))
				local A = 0.42 + 0.22 * math.sin(Now * 4.5 + S.entity.Id)
				S.AuraSprite.Color = Vector4(1, 0.85, 0.35, A)
			end
			local bFlashing = (S.Flash or 0) > 0
			if S.bBroken then
				S.BreakFlash = math.max(0, (S.BreakFlash or 0) - Dt)
				local T = S.Tint or Vector4(1, 1, 1, 1)
				S.Sprite.Color = Vector4(T.X * 0.82, T.Y * 0.8, T.Z * 0.78, T.W)
				if not bFlashing then
					local Pulse = 0.12 + 0.1 * math.sin(Now * 9.0)
					S.Sprite.FlashColor = Vector4(1.0, 0.82, 0.3, math.max(Pulse, math.min(0.9, S.BreakFlash * 2.2)))
				end
			elseif S.Status and S.Status.Freeze then
				local T = S.Tint or Vector4(1, 1, 1, 1)
				S.Sprite.Color = Vector4(T.X * 0.62, T.Y * 0.86, T.Z * 1.35, T.W)
				if not bFlashing then S.Sprite.FlashColor = Vector4(0.75, 0.92, 1.0, 0.22) end
			elseif S.Status and S.Status.Burn and not bFlashing then
				S.Sprite.FlashColor = Vector4(1.0, 0.5, 0.15, 0.1 + 0.1 * math.sin(Now * 20.0 + S.entity.Id))
			elseif not bFlashing and S.Sprite.FlashColor.W > 0 and (S.Flash or 0) <= 0 then
				S.Sprite.FlashColor = Vector4(1, 1, 1, 0)
			end
		end
	end
end

-- 자동 검증 요약 한 줄 (관리자 ReportAutoPlay가 부른다)
function Combat:CombatReportLine()
	local R = self.Report
	local function Join(T)
		local Parts = {}
		for K, V in pairs(T) do Parts[#Parts + 1] = K .. " " .. V end
		table.sort(Parts)
		return table.concat(Parts, ", ")
	end
	return string.format("[HD2D] 전투 요약: 브레이크 %d(보스 %d), 약점 공개 %d, 약점 명중 %d, 정예 처치 %d, 회복 %d, 동료 %s(마법 %d, 명중 %d, 치유 %d, 쓰러짐 %d) | 스킬 {%s} | 적 상태 이상 {%s} | 플레이어 상태 이상 {%s} | 속성 명중 {%s}",
		R.Breaks, R.BossBreaks, R.Reveals, R.WeakHits, R.EliteKills, R.Cures, self.CompanionRecruited and "있음" or "없음", R.CompanionCasts, R.CompanionHits,
		R.CompanionHeals, R.CompanionDowns, Join(R.Skills), Join(R.StatusOnEnemy), Join(R.StatusOnPlayer), Join(R.ElementHits))
end

return Combat
