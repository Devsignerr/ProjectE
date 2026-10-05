-- HD-2D 데모 자동 조종 확장: 전투 깊이 (HD2DAutoPilot.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 자동 조종 객체에).
--   교전 중 스킬 쓰기(UseSkills — 적 약점 속성 우선, MP·쿨다운·해금 레벨을 지키고 사람처럼 키를 누른다), 확인 항목(약점 공개·브레이크·스킬·
--   상태 이상 걸림/해독제·정예 처치·보스 브레이크), 스크린샷 시나리오(BreakShot / SkillShot / StatusShot).
--   자동 조종은 플레이어 능력치를 바꾸지 않는다 — 스킬도 사람과 같은 입력 필드(SkillK/SkillL/Magic1~3)로만 쓴다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local AutoCombat = {}

local SlotFields = { "SkillK", "SkillL", "Magic1", "Magic2", "Magic3" }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

local function IsWeak(Target, Element)
	for _, E in ipairs(Target.Weak or {}) do
		if E == Element then return true end
	end
	return false
end

-- 교전 한 프레임에서 스킬을 쓸지 (썼으면 true — 이번 프레임 입력을 정했다)
function AutoCombat:UseSkills(Target, Dist)
	local P = self.Player
	if P.Cast or P.AttackTimer > 0 or P.DashTimer > 0 or self.GM:IsMenuOpen() then return false end
	self.SkillGap = (self.SkillGap or 0) - (self.GameDt or 0)
	-- 치유: 체력이 반 아래이거나 독·화상이면 (회복약보다 먼저)
	local Slots = P:SkillSlots()
	local function Ready(K)
		local Row, bLocked, bNoMana, Cd = P:SkillState(Slots[K])
		return Row ~= nil and not bLocked and not bNoMana and Cd <= 0, Row
	end
	local bHeal, HealRow = Ready(5)
	if bHeal and HealRow.Kind == "Heal" and (P.Health < P.MaxHealth * 0.5 or self.GM:HasStatus(P, "Poison") or self.GM:HasStatus(P, "Burn")) then
		self.In[SlotFields[5]] = true
		self.SkillGap = 1.0
		return true
	end
	if self.SkillGap > 0 or Target.bDormant then return false end
	-- 약점 속성 기술 우선, 브레이크 중이면 가장 센 기술, 아니면 쓰지 않는다(평타로 BP·MP 모음) — 단 무리(둘 이상)면 범위 기술
	local Best, BestScore = nil, 0
	local Crowd = #self.GM:FindEnemies(P.entity:GetWorldPosition(), 260)
	for K = 1, 4 do
		local bOk, Row = Ready(K)
		if bOk and Row.Kind ~= "Heal" then
			local Reach = (Row.Kind == "Whirl") and Row.Radius or ((Row.Kind == "Cone" or Row.Kind == "Line" or Row.Kind == "Rush") and Row.Range or Row.Range * 0.85)
			if Dist <= Reach + (Target.Radius or 40) then
				local Score = 0
				if IsWeak(Target, Row.Element) and not Target.bBroken and (Target.Shield or 0) > 0 then Score = Score + 3 end
				if Target.bBroken then Score = Score + 2 + Row.Damage * Row.Hits / 30 end
				if Crowd >= 2 and (Row.Kind == "Whirl" or Row.Kind == "Chain" or Row.Kind == "Volley") then Score = Score + 2 end
				if Target.bBoss then Score = Score + 1 end
				-- MP를 다 쓰지 않는다 (지팡이 공격·치유 몫 — 사람도 바닥나기 전에 아낀다)
				if P.Mana - Row.ManaCost < 14 and not Target.bBroken then Score = 0 end
				if Score > BestScore then Best, BestScore = K, Score end
			end
		end
	end
	if not Best then return false end
	self.In[SlotFields[Best]] = true
	local To = Flat(Target.entity:GetWorldPosition() - P.entity:GetWorldPosition())
	if To:Length() > 1 then self.In.Move = To:Normalized() * 0.2 end -- 조준 방향
	self.SkillGap = 0.9
	return true
end

-- 가진 무기 중 대상의 약점 속성 무기로 R 한 번 (이미 약점 무기면 그대로). 바꿨으면 true
function AutoCombat:SwitchToWeakWeapon(Target)
	local GM = self.GM
	local Cur = D.Weapon(GM.Equipped)
	if Cur and IsWeak(Target, Cur.Element) then return false end
	for _, Id in ipairs(D.WeaponOrder) do
		local W = D.Weapon(Id)
		if GM:Count(Id) > 0 and W and IsWeak(Target, W.Element) then
			self.In.Switch = true
			return true
		end
	end
	return false
end

-- 상태 이상: 독버섯 곁에 서서 독에 걸린 뒤 인벤토리에서 해독제로 푼다 (상점에서 산 것)
function AutoCombat:PoisonAndCure()
	local GM, P = self.GM, self.Player
	local Before = GM.Report.StatusOnPlayer.Poison or 0
	if not GM:HasStatus(P, "Poison") then
		local Shroom = GM:NearestEnemy(self:Pos(), 4000, function(S) return S.Kind == "Mushroom" end)
		local Until = self.Time + 30
		while not GM:HasStatus(P, "Poison") and self.Time < Until do
			Shroom = (Shroom and Shroom.entity:IsValid() and not Shroom.bDead) and Shroom or GM:NearestEnemy(self:Pos(), 4000, function(S) return S.Kind == "Mushroom" end)
			if Shroom and not GM:IsMenuOpen() then
				local To = Flat(Shroom.entity:GetWorldPosition() - self:Pos())
				if To:Length() > 110 then self:MoveToward(Shroom.entity:GetWorldPosition()) end
			end
			self:Survive()
			self:Yield()
		end
	end
	self:Expect(GM:HasStatus(P, "Poison") and (GM.Report.StatusOnPlayer.Poison or 0) > Before, "독 웅덩이 → 중독")
	self:Yield()
	self:Yield()
	local Tag = GM:Hud().Tags and GM:Hud().Tags.P
	self:Expect(Tag ~= nil and GM:Hud():W(Tag.Name).Visible, "플레이어 머리 위 상태 이상 아이콘")
	-- 물러나 해독제
	local Away = self:Pos() + Vector3(-500, 250, 0)
	local Until = self.Time + 4
	while self.Time < Until do
		if not GM:IsMenuOpen() then self:MoveToward(Away) end
		self:Yield()
	end
	local Cures = GM.Report.Cures
	local Count = GM:Count("Antidote")
	self:Press("Inventory")
	self:Wait(0.3)
	self:SelectTab(1)
	self:SelectRow("Antidote")
	self:Press("Confirm")
	self:Wait(0.3)
	-- 인벤토리가 열린 동안은 시간이 멈춰 있다 (닫으면 따라온 독버섯이 다시 독을 뿌릴 수 있다)
	self:Expect(not GM:HasStatus(P, "Poison") and GM.Report.Cures == Cures + 1 and GM:Count("Antidote") == Count - 1, "해독제로 중독 회복")
	self:Press("Inventory")
	self:Wait(0.2)
end

-- 동료: 촌장 퀘스트를 받은 뒤 광장의 엘라에게 말을 걸어 영입
function AutoCombat:RecruitCompanionCheck()
	local GM = self.GM
	self:TalkToNpc("Companion")
	self:TalkThrough()
	self:Wait(0.3)
	self:Expect(GM.CompanionRecruited and GM.Companion ~= nil and GM.Companion.Mode == "Follow", "동료 영입 (견습 마법사 엘라)")
	self:Expect(GM:Hud():W("CompanionPanel").Visible, "동료 HUD (체력)")
end

-- 동료 전투 참여: 마법을 쓰고 맞혔는가 (약점 속성이면 실드·공개도 같이)
function AutoCombat:CheckCompanion(Label)
	local R = self.GM.Report
	self:Expect(R.CompanionCasts >= 3 and R.CompanionHits >= 1,
		string.format("%s: 동료 전투 참여 (마법 %d, 명중 %d, 치유 %d, 쓰러짐 %d)", Label, R.CompanionCasts, R.CompanionHits, R.CompanionHeals, R.CompanionDowns))
end

-- 약점 공개·브레이크 확인 (지금까지의 전투에서)
function AutoCombat:CheckBreaks(Label)
	local R = self.GM.Report
	self:Expect(R.Reveals >= 1 and R.Breaks >= 1, string.format("%s: 약점 공개 %d, 브레이크 %d", Label, R.Reveals, R.Breaks))
end

function AutoCombat:CheckSkills(Label, MinKinds)
	local Kinds = 0
	local Names = {}
	for Id, N in pairs(self.GM.Report.Skills) do
		if N > 0 then
			Kinds = Kinds + 1
			Names[#Names + 1] = Id
		end
	end
	table.sort(Names)
	self:Expect(Kinds >= MinKinds, string.format("%s: 스킬 %d종 사용 (%s)", Label, Kinds, table.concat(Names, ", ")))
	local Enemy = 0
	for _, N in pairs(self.GM.Report.StatusOnEnemy) do Enemy = Enemy + N end
	return Enemy
end

-- ================================================================ 스크린샷 시나리오
-- 효과가 가장 잘 보이는 순간에 게임 시간을 멈춘다 (브레이크 글자·스킬 이름 띠는 실제 시간이라 붙잡아 둔다)
function AutoCombat:FreezeForShot(Hold)
	self:Note("스크린샷 순간 — 시간 정지")
	Game.SetTimeScale(0.0)
	local H = self.GM:Hud()
	while true do
		if H.Banner then H.Banner.Age = math.min(H.Banner.Age, Hold or 0.2) end
		if H.SkillNameTime then H.SkillNameTime = math.max(H.SkillNameTime, 0.8) end
		self:Yield()
	end
end

-- 브레이크: 정예 고블린 도적에게 창으로 (찌르기 약점) — 첫 브레이크 직후 멈춤
function AutoCombat:RunBreakShot()
	self:Wait(0.3)
	self:DemoLoadout(4)
	self:EquipBySwitch("Spear")
	local GM = self.GM
	local Breaks = GM.Report.Breaks
	local Until = self.Time + 40
	while GM.Report.Breaks == Breaks and self.Time < Until do
		local Target = GM:NearestEnemy(self:Pos(), 1500, function(S) return S.Kind == "EliteGoblin" end) or GM:NearestEnemy(self:Pos(), 1500)
		if Target and not GM:IsMenuOpen() then self:Engage(Target, true) end
		if self.Player.Health < self.Player.MaxHealth * 0.4 then self.In.Use1 = true end
		self:Yield()
	end
	self:Wait(0.4)
	self:FreezeForShot(0.4)
end

-- 스킬: 무리 앞에서 빛의 기둥(지팡이) 뒤 화염구 — 화염구 폭발 순간 멈춤
function AutoCombat:RunSkillShot()
	self:Wait(0.3)
	self:DemoLoadout(5)
	local GM, P = self.GM, self.Player
	self:EquipBySwitch("Sword")
	local Target
	self:WaitUntil(function()
		Target = GM:NearestEnemy(self:Pos(), 1200)
		if Target and not GM:IsMenuOpen() then
			local L = Flat(Target.entity:GetWorldPosition() - self:Pos()):Length()
			if L > 230 then self:MoveToward(Target.entity:GetWorldPosition()) end
			return L <= 230
		end
		return false
	end, 15)
	self:Press("SkillK") -- 회오리 베기
	self:Wait(0.5)
	self:Press("Magic1") -- 화염구
	self:Wait(0.42)
	self:FreezeForShot()
end

-- 동료: 엘라와 함께 싸우다 엘라의 두 번째 마법이 날아가는 순간
function AutoCombat:RunPartyShot()
	self:Wait(0.3)
	self:DemoLoadout(4)
	local GM = self.GM
	GM:RecruitCompanion()
	self:EquipBySwitch("Spear")
	local Until = self.Time + 30
	while (GM.Report.CompanionCasts or 0) < 2 and self.Time < Until do
		local Target = GM:NearestEnemy(self:Pos(), 1500)
		if Target and not GM:IsMenuOpen() then self:Engage(Target) end
		self:Yield()
	end
	self:Wait(0.22)
	self:FreezeForShot()
end

-- 상태 이상: 빙결·화상에 걸린 적과 독에 걸린 플레이어
function AutoCombat:RunStatusShot()
	self:Wait(0.3)
	self:DemoLoadout(5)
	local GM, P = self.GM, self.Player
	self:EquipBySwitch("Sword")
	local Until = self.Time + 25
	while self.Time < Until do
		local Target = GM:NearestEnemy(self:Pos(), 1500)
		if Target and not GM:IsMenuOpen() then
			local L = Flat(Target.entity:GetWorldPosition() - self:Pos()):Length()
			if (GM.Report.StatusOnEnemy.Burn or 0) == 0 and L < 400 and P.SkillCooldown.Fireball == nil then
				self.In.Magic1 = true
			elseif (GM.Report.StatusOnEnemy.Freeze or 0) == 0 and L < 600 and P.SkillCooldown.IceLance == nil then
				self.In.Magic2 = true
			else
				self:Engage(Target)
			end
		end
		if (GM.Report.StatusOnEnemy.Burn or 0) > 0 and (GM.Report.StatusOnEnemy.Freeze or 0) > 0 then break end
		self:Yield()
	end
	self:Wait(0.3)
	self:FreezeForShot()
end

return AutoCombat
