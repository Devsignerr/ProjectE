-- HD-2D 데모 HUD (씬의 HUD 엔티티 — UI/Demo/HD2D/HUD.eui, Tools/DemoMap/HD2DGameplay.py가 만든다).
--   관리자(HD2DGame.lua)·플레이어(HD2DPlayer.lua)가 메서드로 값을 넘긴다: 상태 창(이름·레벨·HP/MP/EXP 바), 골드, 장비 무기, 퀘스트,
--   획득 알림(최근 4개), 큰 알림, 보스 체력, 상호작용 안내, 데미지 숫자(DmgTemplate 복제 풀 — Camera.WorldToScreen으로 자리),
--   대화 창(이름표 + 타자기 글 + ▼), 인벤토리·상점 창(목록 8줄 + 설명). 메뉴 단추 클릭/올리기는 관리자에게 돌려준다.
--   값은 바뀔 때만 위젯에 쓴다 (쓸 때마다 다시 레이아웃되므로). 알림·숫자 시간은 실제 시간 (메뉴로 게임 시간이 멈춰도 사라진다).
local Hud = {
	Properties = {},
}

local PoolSize = 24
local Rows = 8

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
		self:Set("Toast" .. I, "Visible", T ~= nil)
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
	self:Set("Announce", "Visible", true)
	self:Set("AnnounceSub", "Text", Sub or "")
	self:Set("AnnounceSub", "Visible", Sub ~= nil and Sub ~= "")
	self.AnnounceTime = Time or 2.0
	self.AnnounceMax = self.AnnounceTime
end

function Hud:ShowBoss(Name, Fraction)
	if Name == nil then
		self:Set("BossPanel", "Visible", false)
		return
	end
	self:Set("BossPanel", "Visible", true)
	self:Set("BossName", "Text", Name)
	self:Set("BossBar", "Percent", math.floor(math.max(0, Fraction or 0) * 300 + 0.5) / 300)
end

function Hud:ShowPrompt(Text)
	self:Set("Prompt", "Visible", Text ~= nil)
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
function Hud:ShowDialog(Name, Line, Chars, bDone)
	self:Set("DialogWindow", "Visible", true)
	self:Set("WeaponPanel", "Visible", false)
	self:Set("DialogName", "Text", Name)
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
	self:Set("DialogWindow", "Visible", false)
	self:Set("WeaponPanel", "Visible", true)
end

-- ---- 메뉴 (인벤토리 "Inv" / 상점 "Shop" / nil = 닫기)
function Hud:ShowMenu(Prefix)
	self:Set("MenuShade", "Visible", Prefix ~= nil)
	self:Set("InvWindow", "Visible", Prefix == "Inv")
	self:Set("ShopWindow", "Visible", Prefix == "Shop")
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
			self:Set(Prefix .. "Sel" .. I, "Visible", bSel)
			self:Set(Prefix .. "Cursor" .. I, "Visibility", bSel and "HitTestInvisible" or "Hidden")
			local C = L.bDim and 0.5 or 1.0
			self:SetColor(Prefix .. "Name" .. I, (bSel and 1.0 or 0.92) * C, (bSel and 0.95 or 0.9) * C, (bSel and 0.82 or 0.86) * C, 1)
		end
	end
end

function Hud:SetMenuDetail(Prefix, Icon, Name, Type, Desc, Stats)
	self:Set(Prefix .. "DetailIcon", "Visible", Icon ~= nil)
	if Icon then self:Set(Prefix .. "DetailIcon", "Texture", Icon) end
	self:Set(Prefix .. "DetailName", "Text", Name)
	self:Set(Prefix .. "DetailType", "Text", Type)
	self:Set(Prefix .. "DetailDesc", "Text", Desc)
	self:Set(Prefix .. "DetailStats", "Text", Stats)
end

-- 단추: 줄 클릭/올리기 → 관리자
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
			self:Set("Announce", "Visible", false)
			self:Set("AnnounceSub", "Visible", false)
		end
	end
	-- 대화 ▼ 깜빡임
	self:Set("DialogNext", "Visible", self.bDialogDone == true and math.floor(self.Clock * 2.5) % 2 == 0)
end

return Hud
