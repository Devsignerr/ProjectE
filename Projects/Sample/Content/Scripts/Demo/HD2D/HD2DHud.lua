-- HD-2D 데모 HUD (씬의 HUD 엔티티 — UI/Demo/HD2D/HUD.eui, Tools/DemoMap/HD2DGameplay.py가 만든다).
--   관리자(HD2DGame.lua)·플레이어(HD2DPlayer.lua)가 메서드로 값을 넘긴다: 상태 창(이름·레벨·HP/MP/EXP 바), 골드, 장비 무기, 퀘스트,
--   획득 알림(최근 4개), 큰 알림, 보스 체력, 상호작용 안내, 데미지 숫자(DmgTemplate 복제 풀 — Camera.WorldToScreen으로 자리),
--   대화 창(초상화 + 이름표 + 타자기 글 + ▼), 인벤토리(탭: 도구/장비/퀘스트, 장비 비교)·상점(주인 말) 창(목록 10줄 + 설명),
--   타이틀 화면, BP 구슬·부스트 글자·화면 번쩍임, 장면 전환 페이드. 메뉴 단추 클릭/올리기는 관리자에게 돌려준다.
--   보이기/숨기기는 Show(이름, bool, 모드) — 표시만 하는 위젯은 HitTestInvisible로 보여 게임 마우스 입력(공격)을 가로채지 않는다.
--   값은 바뀔 때만 위젯에 쓴다 (쓸 때마다 다시 레이아웃되므로). 알림·숫자 시간은 실제 시간 (메뉴로 게임 시간이 멈춰도 사라진다).
local Hud = {
	Properties = {},
}

local PoolSize = 24
local Rows = 10

function Hud:Init()
	if self.bInit then return end
	self.bInit = true
	self.Cache = {}
	self.Numbers, self.Free, self.Created = {}, {}, 0
	self.Toasts = {}       -- { Icon, Text, Time } 최근 것이 끝
	self.AnnounceTime = 0
end

function Hud:OnStart()
	self:Init()
	self.GM = Scene.Find("HD2DGame"):GetScript()
end

function Hud:W(Name)
	return self.entity:GetWidget(Name)
end

-- 같은 값이면 쓰지 않는다
function Hud:Set(Name, Field, Value)
	self:Init()
	local Key = Name .. "." .. Field
	if self.Cache[Key] == Value then return end
	self.Cache[Key] = Value
	self:W(Name)[Field] = Value
end

function Hud:Show(Name, bShow, Mode)
	self:Set(Name, "Visibility", bShow and (Mode or "HitTestInvisible") or "Collapsed")
end

function Hud:SetColor(Name, R, G, B, A)
	self:Init()
	local Key = Name .. ".Color"
	local Value = string.format("%.3f,%.3f,%.3f,%.3f", R, G, B, A)
	if self.Cache[Key] == Value then return end
	self.Cache[Key] = Value
	self:W(Name).Color = Vector4(R, G, B, A)
end

-- ---- 상태
function Hud:SetStatus(Name, Level, Hp, MaxHp, Mp, MaxMp, ExpFrac)
	self:Set("PlayerName", "Text", Name)
	self:Set("LevelText", "Text", "Lv " .. Level)
	self:Set("HpBar", "Percent", MaxHp > 0 and math.floor(Hp / MaxHp * 200 + 0.5) / 200 or 0)
	self:Set("HpText", "Text", string.format("%d / %d", math.ceil(Hp), MaxHp))
	self:Set("MpBar", "Percent", MaxMp > 0 and math.floor(Mp / MaxMp * 200 + 0.5) / 200 or 0)
	self:Set("MpText", "Text", string.format("%d / %d", math.floor(Mp), MaxMp))
	self:Set("ExpBar", "Percent", math.floor(ExpFrac * 200 + 0.5) / 200)
	-- 체력이 적으면 표시를 바꾼다
	self:Set("HpLabel", "Text", Hp < MaxHp * 0.3 and "HP!" or "HP")
end

function Hud:SetGold(Gold)
	self:Set("GoldText", "Text", tostring(Gold))
end

function Hud:SetWeapon(Def)
	self:Set("WeaponName", "Text", Def.DisplayName)
	self:Set("WeaponIcon", "Texture", Def.Icon)
end

function Hud:SetQuest(Title, Text)
	self:Set("QuestTitle", "Text", "◆ " .. Title)
	self:Set("QuestText", "Text", Text)
end

-- ---- 알림
function Hud:Toast(Icon, Text)
	self:Init()
	self.Toasts[#self.Toasts + 1] = { Icon = Icon, Text = Text, Time = 3.0 }
	while #self.Toasts > 4 do table.remove(self.Toasts, 1) end
	self:RefreshToasts()
end

function Hud:RefreshToasts()
	for I = 0, 3 do
		local T = self.Toasts[I + 1]
		self:Show("Toast" .. I, T ~= nil)
		if T then
			self:Set("ToastIcon" .. I, "Texture", T.Icon)
			self:Set("ToastText" .. I, "Text", T.Text)
			self:Set("Toast" .. I, "Opacity", math.floor(math.min(1.0, T.Time / 0.5) * 20 + 0.5) / 20)
		end
	end
end

function Hud:Announce(Text, Sub, Time)
	self:Init()
	self:Set("Announce", "Text", Text)
	self:Show("Announce", true)
	self:Set("AnnounceSub", "Text", Sub or "")
	self:Show("AnnounceSub", Sub ~= nil and Sub ~= "")
	self.AnnounceTime = Time or 2.0
	self.AnnounceMax = self.AnnounceTime
end

function Hud:ShowBoss(Name, Fraction)
	if Name == nil then
		self:Show("BossPanel", false)
		return
	end
	self:Show("BossPanel", true)
	self:Set("BossName", "Text", Name)
	self:Set("BossBar", "Percent", math.floor(math.max(0, Fraction or 0) * 300 + 0.5) / 300)
end

function Hud:ShowPrompt(Text)
	self:Show("Prompt", Text ~= nil)
	if Text then self:Set("PromptText", "Text", Text) end
end

-- ---- 데미지 숫자 (월드 위치에 떠오르며 사라짐)
function Hud:ShowDamage(Pos, Text, Color, Scale)
	self:Init()
	local Item = table.remove(self.Free)
	if Item == nil then
		if #self.Numbers >= PoolSize then
			Item = table.remove(self.Numbers, 1)
		else
			self.Created = self.Created + 1
			Item = { Name = "Dmg_" .. self.Created }
			self.entity:CloneWidget("DmgTemplate", Item.Name)
		end
	end
	local W = self:W(Item.Name)
	W.Text = Text
	W.Color = Vector4(Color[1], Color[2], Color[3], Color[4] or 1)
	W.FontSize = math.floor(28 * (Scale or 1))
	W.Visible = true
	Item.Pos = Pos
	Item.Age = 0
	self.Numbers[#self.Numbers + 1] = Item
	self:PlaceNumber(Item, W)
end

function Hud:PlaceNumber(Item, W)
	local X, Y = Camera.WorldToScreen(Item.Pos + Vector3(0, 0, 30 + Item.Age * 110), self.entity)
	-- 처음 0.1초는 살짝 튀어 오름
	local Pop = Item.Age < 0.1 and (1.0 - Item.Age / 0.1) * 10 or 0
	W.Position = Vector2(X, Y - Pop)
	W.Opacity = math.max(0.0, math.min(1.0, (0.8 - Item.Age) * 4))
end

-- ---- 대화 창
function Hud:ShowDialog(Name, Portrait, Line, Chars, bDone)
	self:Show("DialogPortraitFrame", Portrait ~= nil)
	if Portrait then self:Set("DialogPortrait", "Texture", Portrait) end
	self:Show("DialogNamePlate", Name ~= nil and Name ~= "")
	self:Show("DialogWindow", true, "SelfHitTestInvisible")
	self:Show("WeaponPanel", false)
	self:Set("DialogName", "Text", Name or "")
	local TextX = Portrait and 176 or 48
	if self.DialogTextX ~= TextX then
		self.DialogTextX = TextX
		self:W("DialogText").Position = Vector2(TextX, 42)
	end
	local Shown = Line
	local Total = utf8.len(Line) or #Line
	if Chars < Total then
		local Cut = utf8.offset(Line, Chars + 1)
		Shown = Cut and string.sub(Line, 1, Cut - 1) or Line
	end
	self:Set("DialogText", "Text", Shown)
	self.bDialogDone = bDone == true
end

function Hud:HideDialog()
	self:Show("DialogWindow", false, "SelfHitTestInvisible")
	self:Show("WeaponPanel", true)
end

-- ---- 메뉴 (인벤토리 "Inv" / 상점 "Shop" / nil = 닫기)
function Hud:ShowMenu(Prefix)
	self:Show("MenuShade", Prefix ~= nil, "Visible")
	self:Show("InvWindow", Prefix == "Inv", "SelfHitTestInvisible")
	self:Show("ShopWindow", Prefix == "Shop", "SelfHitTestInvisible")
	self.MenuPrefix = Prefix
end

function Hud:SetMenuHeader(Prefix, Status, Gold)
	self:Set(Prefix .. "Status", "Text", Status)
	self:Set(Prefix .. "Gold", "Text", string.format("%d G", Gold))
end

-- Lines: { { Icon, Name, Right, bDim } ... }, Selected = 1부터
function Hud:SetMenuRows(Prefix, Lines, Selected)
	for I = 0, Rows - 1 do
		local L = Lines[I + 1]
		self:Set(Prefix .. "Row" .. I, "Visible", L ~= nil)
		if L then
			local bSel = (I + 1) == Selected
			self:Set(Prefix .. "Icon" .. I, "Texture", L.Icon)
			self:Set(Prefix .. "Name" .. I, "Text", L.Name)
			self:Set(Prefix .. "Count" .. I, "Text", L.Right)
			self:Show(Prefix .. "Sel" .. I, bSel)
			self:Set(Prefix .. "Cursor" .. I, "Visibility", bSel and "HitTestInvisible" or "Hidden")
			local C = L.bDim and 0.5 or 1.0
			self:SetColor(Prefix .. "Name" .. I, (bSel and 1.0 or 0.92) * C, (bSel and 0.95 or 0.9) * C, (bSel and 0.82 or 0.86) * C, 1)
		end
	end
end

function Hud:SetMenuDetail(Prefix, Icon, Name, Type, Desc, Stats)
	self:Show(Prefix .. "DetailIcon", Icon ~= nil)
	if Icon then self:Set(Prefix .. "DetailIcon", "Texture", Icon) end
	self:Set(Prefix .. "DetailName", "Text", Name)
	self:Set(Prefix .. "DetailType", "Text", Type)
	self:Set(Prefix .. "DetailDesc", "Text", Desc)
	self:Set(Prefix .. "DetailStats", "Text", Stats)
end

-- ---- 메뉴 탭·비교·상점 주인 말
function Hud:SetMenuTabs(Prefix, Active)
	for K = 0, 2 do
		local bOn = (K + 1) == Active
		self:SetColor(Prefix .. "TabText" .. K, bOn and 1.0 or 0.6, bOn and 0.85 or 0.58, bOn and 0.42 or 0.66, 1)
		self:SetColor(Prefix .. "Tab" .. K, bOn and 0.32 or 0.1, bOn and 0.22 or 0.09, bOn and 0.12 or 0.18, bOn and 0.95 or 0.7)
	end
end

-- Lines: { { Text, Color {r,g,b,a} } ... } 최대 4줄
function Hud:SetMenuCompare(Prefix, Lines)
	for K = 0, 3 do
		local L = Lines[K + 1]
		self:Show(Prefix .. "Cmp" .. K, L ~= nil)
		if L then
			self:Set(Prefix .. "Cmp" .. K, "Text", L.Text)
			self:SetColor(Prefix .. "Cmp" .. K, L.Color[1], L.Color[2], L.Color[3], L.Color[4])
		end
	end
end

function Hud:SetShopSay(Text)
	self:Set("ShopSay", "Text", "「" .. Text .. "」")
end

-- ---- 타이틀
function Hud:ShowTitle(bShow, Name, Sub, bCanContinue)
	self:Show("TitleScreen", bShow, "SelfHitTestInvisible")
	if bShow then
		self:Set("TitleLogo", "Text", Name)
		self:Set("TitleSub", "Text", Sub)
	end
end

function Hud:SetTitleSelection(Index, bCanContinue, Info)
	for K = 0, 1 do
		local bSel = (K + 1) == Index
		self:Set("TitleCursor" .. K, "Visibility", bSel and "HitTestInvisible" or "Hidden")
		local bDim = K == 1 and not bCanContinue
		local C = bDim and 0.45 or 1.0
		self:SetColor("TitleText" .. K, (bSel and 1.0 or 0.88) * C, (bSel and 0.86 or 0.86) * C, (bSel and 0.45 or 0.82) * C, 1)
	end
	self:Set("TitleInfo", "Text", Info or "")
end

-- ---- 엔딩·크레딧 (검은 화면 + 제목 + 본문 — 쪽 페이드는 관리자가 SetEndingAlpha로)
function Hud:ShowEnding(bShow)
	self:Show("EndingScreen", bShow, "SelfHitTestInvisible")
end

function Hud:SetEndingPage(Title, Body, Alpha)
	self:Set("EndingTitle", "Text", Title)
	self:Set("EndingBody", "Text", Body)
	self:SetEndingAlpha(Alpha or 1.0)
end

function Hud:SetEndingAlpha(Alpha)
	local A = math.floor(Alpha * 20 + 0.5) / 20
	self:Set("EndingTitle", "Opacity", A)
	self:Set("EndingBody", "Opacity", A)
end

-- 게임 HUD 패널 (타이틀 동안 숨김)
function Hud:SetHudVisible(bShow)
	for _, Name in ipairs({ "StatusPanel", "GoldRow", "BoostRow", "WeaponPanel", "QuestPanel", "ToastBox" }) do
		self:Show(Name, bShow)
	end
end

-- ---- 부스트 BP 구슬: 0..BP-Pending-1 채움, 그 위 Pending개는 푸른 빛(이번에 쓸 것), 나머지 빈 구슬
function Hud:SetBoost(BP, Pending, Max)
	for K = 0, 4 do
		local Tex
		if K >= Max then
			Tex = nil
		elseif K < BP - Pending then
			Tex = "UI/Demo/HD2D/OrbFull.png"
		elseif K < BP then
			Tex = "UI/Demo/HD2D/OrbPending.png"
		else
			Tex = "UI/Demo/HD2D/OrbEmpty.png"
		end
		self:Show("Orb" .. K, Tex ~= nil)
		if Tex then self:Set("Orb" .. K, "Texture", Tex) end
	end
	self:Set("BoostLevel", "Text", Pending > 0 and ("BOOST " .. string.rep("▶", Pending)) or ((self.OrbPulse and BP < Max + 1) and "BP +1" or ""))
end

function Hud:PulseOrb(Index)
	self:Init()
	self.OrbPulse = { Index = Index, Time = 0.35 }
end

-- 가운데 큰 글자 (부스트 발동·단계)
function Hud:BoostBurst(Text, Time)
	self:Init()
	self:Set("BoostBurst", "Text", Text)
	self:Show("BoostBurst", true)
	self.BurstTime, self.BurstMax = Time or 0.8, Time or 0.8
end

function Hud:ScreenFlash(Alpha, Time)
	self:Init()
	self.FlashTime, self.FlashMax, self.FlashAlpha = Time or 0.25, Time or 0.25, Alpha or 0.5
	self:Show("ScreenFlash", true)
end

-- 장면 전환 페이드: FadeTo(1, t) = 어두워짐, FadeFrom(1, t) = 검은 화면에서 밝아짐
function Hud:FadeTo(Target, Time)
	self:Init()
	self.FadeFromV, self.FadeToV, self.FadeTime, self.FadeMax = self.FadeValue or 0, Target, 0, Time
end

function Hud:FadeFrom(Start, Time)
	self:Init()
	self.FadeFromV, self.FadeToV, self.FadeTime, self.FadeMax = Start, 0, 0, Time
	self.FadeValue = Start
	self:ApplyFade()
end

function Hud:ApplyFade()
	local V = self.FadeValue or 0
	self:Show("Fade", V > 0.001)
	self:Set("Fade", "Opacity", math.floor(V * 50 + 0.5) / 50)
end

function Hud:UpdateEffects(UDt)
	if self.FadeMax then
		self.FadeTime = self.FadeTime + UDt
		local T = math.min(1.0, self.FadeTime / math.max(self.FadeMax, 0.001))
		self.FadeValue = self.FadeFromV + (self.FadeToV - self.FadeFromV) * T
		self:ApplyFade()
		if T >= 1.0 then self.FadeMax = nil end
	end
	if self.FlashTime then
		self.FlashTime = self.FlashTime - UDt
		if self.FlashTime <= 0 then
			self.FlashTime = nil
			self:Show("ScreenFlash", false)
		else
			self:Set("ScreenFlash", "Opacity", math.floor(self.FlashAlpha * (self.FlashTime / self.FlashMax) * 40 + 0.5) / 40)
		end
	end
	if self.BurstTime then
		self.BurstTime = self.BurstTime - UDt
		if self.BurstTime <= 0 then
			self.BurstTime = nil
			self:Show("BoostBurst", false)
		else
			local Age = self.BurstMax - self.BurstTime
			local Scale = Age < 0.12 and (1.6 - Age / 0.12 * 0.6) or 1.0
			self:W("BoostBurst").FontSize = math.floor(40 * Scale)
			self:Set("BoostBurst", "Opacity", math.floor(math.min(1.0, self.BurstTime / 0.25) * 20 + 0.5) / 20)
		end
	end
	if self.OrbPulse then
		self.OrbPulse.Time = self.OrbPulse.Time - UDt
		if self.OrbPulse.Time <= 0 then self.OrbPulse = nil end
	end
end

-- 단추: 줄 클릭/올리기 → 관리자
for I = 0, 1 do
	Hud["OnUIClicked_TitleRow" .. I] = function(self) self.GM:OnMenuRowClicked(I + 1) end
	Hud["OnUIHoverBegin_TitleRow" .. I] = function(self) self.GM:OnMenuRowHovered(I + 1) end
end
for I = 0, Rows - 1 do
	Hud["OnUIClicked_InvRow" .. I] = function(self) self.GM:OnMenuRowClicked(I + 1) end
	Hud["OnUIClicked_ShopRow" .. I] = function(self) self.GM:OnMenuRowClicked(I + 1) end
	Hud["OnUIHoverBegin_InvRow" .. I] = function(self) self.GM:OnMenuRowHovered(I + 1) end
	Hud["OnUIHoverBegin_ShopRow" .. I] = function(self) self.GM:OnMenuRowHovered(I + 1) end
end

-- 카메라는 플레이어 OnLateUpdate에서 놓이므로 숫자 자리도 늦은 갱신에서 (실제 시간)
function Hud:OnLateUpdate(Dt)
	self:Init()
	local UDt = Time.GetUnscaledDelta()
	self.Clock = (self.Clock or 0) + UDt
	local Keep = {}
	for _, Item in ipairs(self.Numbers) do
		Item.Age = Item.Age + UDt
		local W = self:W(Item.Name)
		if Item.Age > 0.8 then
			W.Visible = false
			self.Free[#self.Free + 1] = Item
		else
			self:PlaceNumber(Item, W)
			Keep[#Keep + 1] = Item
		end
	end
	self.Numbers = Keep
	-- 알림 수명
	local bChanged = false
	local KeepT = {}
	for _, T in ipairs(self.Toasts) do
		T.Time = T.Time - UDt
		if T.Time > 0 then KeepT[#KeepT + 1] = T else bChanged = true end
		if T.Time < 0.5 then bChanged = true end
	end
	self.Toasts = KeepT
	if bChanged then self:RefreshToasts() end
	if self.AnnounceTime > 0 then
		self.AnnounceTime = self.AnnounceTime - UDt
		local A = math.max(0.0, math.min(1.0, self.AnnounceTime / 0.5, (self.AnnounceMax - self.AnnounceTime) / 0.2))
		self:Set("Announce", "Opacity", math.floor(A * 20 + 0.5) / 20)
		self:Set("AnnounceSub", "Opacity", math.floor(A * 20 + 0.5) / 20)
		if self.AnnounceTime <= 0 then
			self:Show("Announce", false)
			self:Show("AnnounceSub", false)
		end
	end
	self:UpdateEffects(UDt)
	-- 대화 ▼ 깜빡임
	self:Show("DialogNext", self.bDialogDone == true and math.floor(self.Clock * 2.5) % 2 == 0)
end

return Hud
