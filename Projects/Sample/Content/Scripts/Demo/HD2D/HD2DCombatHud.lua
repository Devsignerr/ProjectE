-- HD-2D 데모 HUD 확장: 전투 표시 (HD2DHud.lua가 Script.Require로 받아 메서드로 붙인다 — 위젯은 HD2DCombatGen.HudWidgets가 HUD.eui에 넣는다).
--   적 머리 위 태그(TagTemplate 복제 풀): 정예 이름 · 상태 이상 아이콘 · 실드(숫자, 브레이크면 깨진 방패) · 약점 칸(공개 전 "?" — 공개되면 속성 그림으로 번쩍)
--   플레이어 머리 위: 상태 이상 아이콘만 (같은 템플릿). 브레이크 글자(BreakBanner — 적 자리에서 커졌다 줄며 사라짐),
--   스킬 칸 5개(아이콘·키·MP·쿨다운 덮개·잠김 레벨), 스킬 이름 띠. 자리는 Camera.WorldToScreen(늦은 갱신 — 카메라가 놓인 뒤).
local Ui = {}

local Dir = "UI/Demo/HD2D/Combat/"
local MaxTags = 8
local StatusOrder = { "Poison", "Burn", "Freeze", "Stun" }

function Ui:InitCombatHud()
	if self.Tags then return end
	self.Tags = {}         -- 키(엔티티 Id 또는 "P") → { Name, Seen }
	self.TagFree = {}
	self.TagCount = 0
end

function Ui:AcquireTag(Key)
	local T = self.Tags[Key]
	if T then return T end
	T = table.remove(self.TagFree)
	if not T then
		self.TagCount = self.TagCount + 1
		T = { Name = "Tag_" .. self.TagCount }
		self.entity:CloneWidget("TagTemplate", T.Name)
	end
	self.Tags[Key] = T
	T.Key = Key
	T.Weak = {}
	return T
end

function Ui:ReleaseTag(T)
	self:Show(T.Name, false)
	self.Tags[T.Key] = nil
	self.TagFree[#self.TagFree + 1] = T
end

-- 태그 한 개 그리기. Info: { Pos, Name(정예), Statuses {…}, Shield, bBroken, Weak { {Elem, bShown, Flash} } } (Shield nil = 실드 줄 숨김)
function Ui:DrawTag(T, Info)
	local N = T.Name
	local X, Y, bVisible = Camera.WorldToScreen(Info.Pos, self.entity)
	if not bVisible then
		self:Show(N, false)
		return
	end
	self:Show(N, true)
	local W = self:W(N)
	W.Position = Vector2(math.floor(X + 0.5), math.floor(Y + 0.5))
	self:Show(N .. ".TagName", Info.Name ~= nil)
	if Info.Name then self:Set(N .. ".TagName", "Text", Info.Name) end
	local NS = #Info.Statuses
	self:Show(N .. ".TagStatus", NS > 0)
	for K = 0, 3 do
		local St = Info.Statuses[K + 1]
		self:Show(N .. ".TagSt" .. K, St ~= nil)
		if St then self:Set(N .. ".TagSt" .. K, "Texture", Dir .. "Status" .. St .. ".png") end
	end
	self:Show(N .. ".TagRow", Info.Shield ~= nil)
	if Info.Shield == nil then return end
	self:Set(N .. ".TagShield", "Texture", Info.bBroken and (Dir .. "ShieldBroken.png") or (Dir .. "Shield.png"))
	self:Set(N .. ".TagShieldNum", "Text", Info.bBroken and "" or tostring(Info.Shield))
	-- 실드가 줄면 숫자가 잠깐 붉고 크게
	local Pulse = Info.Pulse or 0
	self:Set(N .. ".TagShieldNum", "FontSize", Pulse > 0 and 26 or 19)
	if Pulse > 0 then self:SetColor(N .. ".TagShieldNum", 1, 0.55, 0.45, 1) else self:SetColor(N .. ".TagShieldNum", 1, 1, 1, 1) end
	for K = 0, 4 do
		local Wk = Info.Weak[K + 1]
		local Name = N .. ".TagWeak" .. K
		self:Show(Name, Wk ~= nil)
		if Wk then
			self:Set(Name, "Texture", Dir .. "Elem" .. (Wk.bShown and Wk.Elem or "Unknown") .. ".png")
			-- 막 공개된 칸은 번쩍 (밝게 → 원래)
			local F = Wk.Flash or 0
			local C = 1.0 + F * 1.4
			self:SetColor(Name, C, C, C * (1.0 - F * 0.3), 1)
		end
	end
end

-- 매 프레임 (OnLateUpdate): 적 태그·플레이어 상태·브레이크 글자
function Ui:UpdateCombatHud(UDt)
	self:InitCombatHud()
	local GM = self.GM
	if not GM or not GM.CombatTagTargets then return end
	local Player = GM:GetPlayer()
	local bHide = GM.Menu == "Title" or GM.Menu == "Ending" or GM.Mode == "Travel"
	for _, T in pairs(self.Tags) do T.Seen = false end
	if Player and not bHide then
		local PPos = Player.entity:GetWorldPosition()
		for _, Item in ipairs(GM:CombatTagTargets(PPos, MaxTags)) do
			local S = Item.S
			local T = self:AcquireTag(S.entity.Id)
			T.Seen = true
			local Weak = {}
			for _, Elem in ipairs(S.Weak or {}) do
				local bShown = GM:IsRevealed(S.WeakKey, Elem)
				local Flash = 0
				if S.RevealFlash and S.RevealFlash.Element == Elem then
					S.RevealFlash.Time = S.RevealFlash.Time - UDt
					Flash = math.max(0, S.RevealFlash.Time / 0.5)
					if S.RevealFlash.Time <= 0 then S.RevealFlash = nil end
				end
				Weak[#Weak + 1] = { Elem = Elem, bShown = bShown, Flash = Flash }
			end
			local Statuses = {}
			for _, Kind in ipairs(StatusOrder) do
				if S.Status and S.Status[Kind] then Statuses[#Statuses + 1] = Kind end
			end
			if S.ShieldPulse then
				S.ShieldPulse = S.ShieldPulse - UDt
				if S.ShieldPulse <= 0 then S.ShieldPulse = nil end
			end
			self:DrawTag(T, { Pos = Item.Pos, Name = S.Row.Elite and S.Row.DisplayName or nil, Statuses = Statuses, Shield = S.Shield, bBroken = S.bBroken,
			                  Weak = Weak, Pulse = S.ShieldPulse })
		end
		-- 플레이어: 상태 이상이 있을 때만
		local Statuses = {}
		for _, Kind in ipairs(StatusOrder) do
			if Player.Status and Player.Status[Kind] then Statuses[#Statuses + 1] = Kind end
		end
		if #Statuses > 0 then
			local T = self:AcquireTag("P")
			T.Seen = true
			self:DrawTag(T, { Pos = PPos + Vector3(0, 0, 105), Statuses = Statuses, Weak = {} })
		end
	end
	for Key, T in pairs(self.Tags) do
		if not T.Seen then self:ReleaseTag(T) end
	end
	self:UpdateBreakBanner(UDt)
	if self.SkillNameTime then
		self.SkillNameTime = self.SkillNameTime - UDt
		local A = math.max(0, math.min(1, self.SkillNameTime / 0.25, (self.SkillNameMax - self.SkillNameTime) / 0.08))
		self:Set("SkillNamePanel", "Opacity", math.floor(A * 20 + 0.5) / 20)
		if self.SkillNameTime <= 0 then
			self.SkillNameTime = nil
			self:Show("SkillNamePanel", false)
		end
	end
end

-- ---- 브레이크 글자
function Ui:ShowBreakBanner(WorldPos, bBig)
	self:InitCombatHud()
	self.Banner = { Pos = WorldPos, Age = 0, Size = bBig and 62 or 46 }
	self:Show("BreakBanner", true)
	self:SetColor("BreakBanner", 1.0, 0.86, 0.32, 1)
end

function Ui:UpdateBreakBanner(UDt)
	local B = self.Banner
	if not B then return end
	B.Age = B.Age + UDt
	local Life = 1.1
	if B.Age >= Life then
		self.Banner = nil
		self:Show("BreakBanner", false)
		return
	end
	local X, Y = Camera.WorldToScreen(B.Pos, self.entity)
	local W = self:W("BreakBanner")
	-- 크게 튀어나왔다가(0.1초) 제자리, 끝 0.3초에 위로 떠오르며 사라짐
	local Pop = B.Age < 0.1 and (1.0 + (1.0 - B.Age / 0.1) * 0.8) or 1.0
	local Rise = math.max(0, B.Age - (Life - 0.3)) * 120
	W.Position = Vector2(X, Y - 20 - Rise)
	W.FontSize = math.floor(B.Size * Pop)
	W.Opacity = math.max(0, math.min(1, (Life - B.Age) / 0.3))
	-- 금빛 ↔ 흰빛 깜빡 (처음 0.3초)
	if B.Age < 0.3 and math.floor(B.Age * 20) % 2 == 0 then
		self:SetColor("BreakBanner", 1, 1, 0.92, 1)
	else
		self:SetColor("BreakBanner", 1.0, 0.86, 0.32, 1)
	end
end

-- ---- 스킬 칸 (K = 0..4). Row nil = 빈 칸
function Ui:SetSkillSlot(K, Row, bLocked, bNoMana, Cd)
	local Show = Row ~= nil
	self:Show("Skill" .. K, Show)
	if not Show then return end
	self:Set("SkillIcon" .. K, "Texture", Row.Icon)
	self:Set("SkillBg" .. K, "Texture", bLocked and (Dir .. "SkillFrameLocked.png") or (Dir .. "SkillFrame.png"))
	self:Set("SkillMp" .. K, "Text", bLocked and "" or tostring(math.floor(Row.ManaCost)))
	if bNoMana and not bLocked then self:SetColor("SkillMp" .. K, 1, 0.45, 0.45, 1) else self:SetColor("SkillMp" .. K, 0.6, 0.86, 1, 1) end
	local Dim = bLocked and 0.3 or ((bNoMana or Cd > 0) and 0.55 or 1.0)
	self:SetColor("SkillIcon" .. K, Dim, Dim, bLocked and 0.36 or Dim * (bNoMana and 1.25 or 1.0), 1)
	self:Show("SkillLock" .. K, bLocked)
	if bLocked then self:Set("SkillLock" .. K, "Text", "Lv" .. Row.UnlockLevel) end
	local bCd = Cd > 0 and not bLocked
	self:Show("SkillCd" .. K, bCd)
	self:Show("SkillTime" .. K, bCd)
	if bCd then
		self:Set("SkillCd" .. K, "Percent", math.floor(math.min(1, Cd / math.max(Row.Cooldown, 0.01)) * 40 + 0.5) / 40)
		self:Set("SkillTime" .. K, "Text", Cd >= 1 and tostring(math.ceil(Cd)) or string.format("%.1f", Cd))
	end
end

-- ---- 동료 창 (Name nil = 숨김)
function Ui:SetCompanion(Name, Hp, MaxHp, DownLeft)
	self:Show("CompanionPanel", Name ~= nil)
	if not Name then return end
	self:Set("CompanionName", "Text", Name)
	self:Set("CompanionHp", "Percent", MaxHp > 0 and math.floor(math.max(0, Hp) / MaxHp * 100 + 0.5) / 100 or 0)
	self:Set("CompanionHpText", "Text", string.format("%d / %d", math.max(0, math.ceil(Hp)), MaxHp))
	self:Set("CompanionState", "Text", (DownLeft or 0) > 0 and string.format("쓰러짐 %d", math.ceil(DownLeft)) or "")
end

function Ui:ShowSkillName(Icon, Name, Time)
	self:InitCombatHud()
	self:Show("SkillNamePanel", true)
	self:Set("SkillNameIcon", "Texture", Icon)
	self:Set("SkillNameText", "Text", Name)
	self.SkillNameTime, self.SkillNameMax = Time or 1.2, Time or 1.2
end

return Ui
