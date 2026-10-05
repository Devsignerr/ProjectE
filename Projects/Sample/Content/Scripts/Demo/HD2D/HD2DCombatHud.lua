-- HD-2D 데모 HUD 확장: 전투 표시 (HD2DHud.lua가 Script.Require로 받아 메서드로 붙인다 — 위젯은 HD2DCombatGen.HudWidgets가 HUD.eui에 넣는다).
--   적 머리 위 태그(TagTemplate 복제 풀): 정예 이름 · 상태 이상 아이콘 · 실드(숫자, 브레이크면 깨진 방패) · 약점 칸(공개 전 "?" — 공개되면 속성 그림으로 번쩍)
--     · 체력 막대(보스는 화면 아래 보스 체력이 있어 빼고). 아래 끝 = 적 표 TagHeight(머리 바로 위), 태그가 있는 적은 몸의 체력바 스프라이트를 숨긴다.
--   배치 규칙: 화면 좌표로 모은 뒤 ① 가까운(화면 아래) 태그부터 놓고 겹치면 덜 움직이는 쪽(옆/위)으로 비켜 놓는다 ② HUD 창(상태·퀘스트·미니맵·스킬 칸·
--     무기 창·알림·보스 체력·안내·스킬 이름 띠)과 겹치거나 화면 밖으로 걸치면 숨긴다 ③ 위치가 바뀐 픽셀만 쓴다(쓸 때마다 다시 레이아웃되므로).
--   플레이어 머리 위: 상태 이상 아이콘만 (같은 템플릿). 브레이크 글자(BreakBanner — 적 몸 오른쪽에서 튀어 올라 사라짐),
--   스킬 칸 5개(아이콘·키·MP·쿨다운 덮개·잠김 레벨), 스킬 이름 띠(스킬 칸 바로 위). 자리는 Camera.WorldToScreen(늦은 갱신 — 카메라가 놓인 뒤).
local Ui = {}

local Dir = "UI/Demo/HD2D/Combat/"
local MaxTags = 8
local StatusOrder = { "Poison", "Burn", "Freeze", "Stun" }
-- HUD 창 (위젯 이름, 앵커 X/Y, 정렬 X/Y) — HD2DGameplay.WriteUi·HD2DCombatGen.HudWidgets의 슬롯과 같게
local Panels = {
	{ "StatusPanel", 0, 0, 0, 0 }, { "GoldRow", 0, 0, 0, 0 }, { "BoostRow", 0, 0, 0, 0 }, { "CompanionPanel", 0, 0, 0, 0 },
	{ "QuestPanel", 1, 0, 1, 0 }, { "WeaponPanel", 0, 1, 0, 1 }, { "SkillBar", 0, 1, 0, 1 }, { "SkillNamePanel", 0, 1, 0, 1 },
	{ "ToastBox", 1, 1, 1, 1 }, { "BossPanel", 0.5, 1, 0.5, 1 }, { "Prompt", 0.5, 1, 0.5, 1 },
}

function Ui:InitCombatHud()
	if self.Tags then return end
	self.Tags = {}         -- 키(엔티티 Id 또는 "P") → { Name, Seen, X, Y }
	self.TagFree = {}
	self.TagCount = 0
	self.RectsAge = 99
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
	T.X, T.Y = nil, nil
	return T
end

function Ui:ReleaseTag(T)
	self:Show(T.Name, false)
	self.Tags[T.Key] = nil
	self.TagFree[#self.TagFree + 1] = T
end

-- 화면 크기 + HUD 창 사각형 (0.2초마다 — 창 크기는 자주 바뀌지 않는다)
function Ui:RefreshHudRects(UDt)
	self.RectsAge = self.RectsAge + UDt
	if self.RectsAge < 0.2 and self.Rects then return end
	self.RectsAge = 0
	local Probe = self:W("ScreenProbe")
	local Sz = Probe and Probe.Size
	local W, H = (Sz and Sz.X > 10) and Sz.X or 1280, (Sz and Sz.Y > 10) and Sz.Y or 720
	self.ScreenW, self.ScreenH = W, H
	local Rects = {}
	local function Add(Widget, Ax, Ay, Lx, Ly)
		if not Widget or not Widget.Visible then return end
		local P, S = Widget.Position, Widget.Size
		local X = Ax * W + P.X - Lx * S.X
		local Y = Ay * H + P.Y - Ly * S.Y
		Rects[#Rects + 1] = { X - 4, Y - 4, X + S.X + 4, Y + S.Y + 4 }
	end
	for _, Pn in ipairs(Panels) do Add(self:W(Pn[1]), Pn[2], Pn[3], Pn[4], Pn[5]) end
	-- 미니맵 (메타 UI — 다른 UI 엔티티, HUD 위에 그려진다)
	self.MetaUi = self.MetaUi or Scene.Find("MetaUI")
	if self.MetaUi then Add(self.MetaUi:GetWidget("Minimap"), 1, 0, 1, 0) end
	self.Rects = Rects
end

-- 사각형이 HUD 창과 겹치거나 화면 밖으로 걸치는가
function Ui:TagBlocked(X0, Y0, X1, Y1)
	if X0 < 4 or Y0 < 4 or X1 > self.ScreenW - 4 or Y1 > self.ScreenH - 4 then return true end
	for _, R in ipairs(self.Rects) do
		if X0 < R[3] and X1 > R[1] and Y0 < R[4] and Y1 > R[2] then return true end
	end
	return false
end

-- 태그 내용 (위치는 따로 — PlaceTags). Info: { Name(정예), Statuses {…}, Shield, bBroken, Weak { {Elem, bShown, Flash} }, Hp(0~1 또는 nil), Pulse }
function Ui:FillTag(T, Info)
	local N = T.Name
	self:Show(N .. ".TagName", Info.Name ~= nil)
	if Info.Name then self:Set(N .. ".TagName", "Text", Info.Name) end
	self:Show(N .. ".TagStatus", #Info.Statuses > 0)
	for K = 0, 3 do
		local St = Info.Statuses[K + 1]
		self:Show(N .. ".TagSt" .. K, St ~= nil)
		if St then self:Set(N .. ".TagSt" .. K, "Texture", Dir .. "Status" .. St .. ".png") end
	end
	self:Show(N .. ".TagHp", Info.Hp ~= nil)
	if Info.Hp then self:Set(N .. ".TagHp", "Percent", math.floor(math.max(0, Info.Hp) * 40 + 0.5) / 40) end
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
			local F = Wk.Flash or 0
			local C = 1.0 + F * 1.4
			self:SetColor(Name, C, C, C * (1.0 - F * 0.3), 1)
		end
	end
end

-- 크기 어림 (레이아웃 결과를 읽으면 그 프레임에 다시 배치하므로 내용으로 계산 — 슬롯 크기는 생성기와 같다)
local function TagSize(Info)
	local W = Info.Shield ~= nil and (45 + 32 * #Info.Weak) or (28 * #Info.Statuses)
	local H = (Info.Shield ~= nil and 42 or 0) + (Info.Hp and 8 or 0) + (Info.Name and 20 or 0) + (#Info.Statuses > 0 and 28 or 0)
	if Info.Name then W = math.max(W, 16 * (utf8.len(Info.Name) or 6)) end
	return W, H
end

-- 매 프레임 (OnLateUpdate): 적 태그·플레이어 상태·브레이크 글자
function Ui:UpdateCombatHud(UDt)
	self:InitCombatHud()
	local GM = self.GM
	if not GM or not GM.CombatTagTargets then return end
	self:RefreshHudRects(UDt)
	local Player = GM:GetPlayer()
	local bHide = GM.Menu ~= nil or GM.Mode == "Travel"
	for _, T in pairs(self.Tags) do T.Seen = false end
	local Items = {}
	if Player and not bHide then
		local PPos = Player.entity:GetWorldPosition()
		for _, Item in ipairs(GM:CombatTagTargets(PPos, MaxTags)) do
			local S = Item.S
			local Weak = {}
			for _, Elem in ipairs(S.Weak or {}) do
				local Flash = 0
				if S.RevealFlash and S.RevealFlash.Element == Elem then
					S.RevealFlash.Time = S.RevealFlash.Time - UDt
					Flash = math.max(0, S.RevealFlash.Time / 0.5)
					if S.RevealFlash.Time <= 0 then S.RevealFlash = nil end
				end
				Weak[#Weak + 1] = { Elem = Elem, bShown = GM:IsRevealed(S.WeakKey, Elem), Flash = Flash }
			end
			local Statuses = {}
			for _, Kind in ipairs(StatusOrder) do
				if S.Status and S.Status[Kind] then Statuses[#Statuses + 1] = Kind end
			end
			if S.ShieldPulse then
				S.ShieldPulse = S.ShieldPulse - UDt
				if S.ShieldPulse <= 0 then S.ShieldPulse = nil end
			end
			local Hp = (not S.bBoss) and math.max(0, S.Health / S.Row.MaxHealth) or nil
			Items[#Items + 1] = { Key = S.entity.Id, Pos = Item.Pos, Info = { Name = S.Row.Elite and S.Row.DisplayName or nil, Statuses = Statuses,
			                      Shield = S.Shield, bBroken = S.bBroken, Weak = Weak, Pulse = S.ShieldPulse, Hp = Hp } }
		end
		-- 플레이어: 상태 이상이 있을 때만
		local Statuses = {}
		for _, Kind in ipairs(StatusOrder) do
			if Player.Status and Player.Status[Kind] then Statuses[#Statuses + 1] = Kind end
		end
		if #Statuses > 0 then
			Items[#Items + 1] = { Key = "P", Pos = PPos + Vector3(0, 0, 105), Info = { Statuses = Statuses, Weak = {} } }
		end
	end
	self:PlaceTags(Items)
	for _, T in pairs(self.Tags) do
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

-- 화면 좌표로 놓기: 가까운(화면 아래) 것부터, 겹치면 비켜 놓고, HUD 창·화면 밖이면 숨김
function Ui:PlaceTags(Items)
	for _, It in ipairs(Items) do
		local X, Y, bVisible = Camera.WorldToScreen(It.Pos, self.entity)
		It.X, It.Y, It.bOn = X, Y, bVisible
		It.W, It.H = TagSize(It.Info)
	end
	table.sort(Items, function(A, B) return A.Y > B.Y end)
	local Placed = {}
	for _, It in ipairs(Items) do
		if It.bOn then
			for _ = 1, 3 do
				local Hit = nil
				for _, P in ipairs(Placed) do
					if math.abs(It.X - P.X) < (It.W + P.W) * 0.5 + 2 and It.Y > P.Y - P.H - 2 and It.Y - It.H < P.Y + 2 then Hit = P break end
				end
				if not Hit then break end
				-- 덜 움직이는 쪽으로: 옆으로 비키기 vs 위로 올리기
				local DX = (It.W + Hit.W) * 0.5 + 2 - math.abs(It.X - Hit.X)
				local DY = It.Y - (Hit.Y - Hit.H - 2)
				if DX < DY then
					It.X = It.X + (It.X >= Hit.X and DX or -DX)
				else
					It.Y = It.Y - DY
				end
			end
			if self:TagBlocked(It.X - It.W * 0.5, It.Y - It.H, It.X + It.W * 0.5, It.Y) then It.bOn = false end
		end
		local T = self:AcquireTag(It.Key)
		T.Seen = true
		if It.bOn then
			Placed[#Placed + 1] = It
			self:FillTag(T, It.Info)
			self:Show(T.Name, true)
			local PX, PY = math.floor(It.X + 0.5), math.floor(It.Y + 0.5)
			if PX ~= T.X or PY ~= T.Y then
				T.X, T.Y = PX, PY
				self:W(T.Name).Position = Vector2(PX, PY) -- 바뀐 픽셀만 (쓰면 다시 레이아웃)
			end
		else
			self:Show(T.Name, false)
		end
	end
end

-- ---- 브레이크 글자: 적 몸 오른쪽에서 튀어 올라 위로 떠오르며 사라짐 (머리 위 태그와 겹치지 않게 몸 높이에서)
function Ui:ShowBreakBanner(WorldPos, bBig, SideOffset)
	self:InitCombatHud()
	self.Banner = { Pos = WorldPos, Age = 0, Size = bBig and 56 or 42, Side = SideOffset or 70 }
	self:Show("BreakBanner", true)
	self:SetColor("BreakBanner", 1.0, 0.86, 0.32, 1)
end

function Ui:UpdateBreakBanner(UDt)
	local B = self.Banner
	if not B then return end
	B.Age = B.Age + UDt
	local Life = 1.0
	if B.Age >= Life then
		self.Banner = nil
		self:Show("BreakBanner", false)
		return
	end
	local X, Y = Camera.WorldToScreen(B.Pos, self.entity)
	local W = self:W("BreakBanner")
	-- 크게 튀어나왔다가(0.08초) 제자리, 처음 0.25초 동안 위로 튀어 오르고 끝 0.3초에 떠오르며 사라짐
	local Pop = B.Age < 0.08 and (1.0 + (1.0 - B.Age / 0.08) * 0.6) or 1.0
	local Jump = math.min(1.0, B.Age / 0.25)
	local Rise = 26 * (1 - (1 - Jump) * (1 - Jump)) + math.max(0, B.Age - (Life - 0.3)) * 110
	W.Position = Vector2(X + B.Side, Y - Rise)
	W.FontSize = math.floor(B.Size * Pop)
	W.Opacity = math.max(0, math.min(1, (Life - B.Age) / 0.3))
	if B.Age < 0.24 and math.floor(B.Age * 25) % 2 == 0 then
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
