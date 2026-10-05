-- FarmBie HUD (씬의 "Hud" 엔티티 — UIComponent UI/FarmBie/HUD.eui). 위젯 이름은 Tools/FarmBieUI.py와 약속이다.
--   관리자가 Scene.Find("Hud"):GetScript()로 부른다: Announce(제목, 부제, 초) / ShowPrompt(글) / SetFade(0~1)
--   시계는 매 프레임 관리자 상태를 읽어 바뀔 때만 쓴다 (Set 캐시).
local Hud = {
	Properties = {},
}

function Hud:Init()
	if self.bInit then return end
	self.bInit = true
	self.Cache = {}
	self.AnnounceTime = 0
end

function Hud:OnStart()
	self:Init()
	self.GM = Scene.Find("FarmGame"):GetScript()
end

function Hud:W(Name)
	return self.entity:GetWidget(Name)
end

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

function Hud:Announce(Title, Sub, Seconds)
	self:Set("BannerTitle", "Text", Title or "")
	self:Set("BannerSub", "Text", Sub or "")
	self:Show("BannerSub", Sub ~= nil and Sub ~= "")
	self:Show("Banner", true)
	self.AnnounceTime = Seconds or 3.0
	self.AnnounceTotal = self.AnnounceTime
	self.LastAnnounce = Title
end

function Hud:ShowPrompt(Text)
	if Text and Text ~= "" then
		self:Set("PromptText", "Text", Text)
		self:Show("Prompt", true)
	else
		self:Show("Prompt", false)
	end
end

-- 획득 알림: 최근 3줄 (4초)
function Hud:Toast(Icon, Text, Color)
	self:Init()
	self.Toasts = self.Toasts or {}
	table.insert(self.Toasts, 1, { Icon = Icon or "", Text = Text, Color = Color, Time = 4.0 })
	while #self.Toasts > 3 do table.remove(self.Toasts) end
	self.LastToast = Text
end

function Hud:ShowItemName(Name)
	self:Set("ItemName", "Text", Name or "")
	self.ItemNameTime = 1.6
end

function Hud:UpdateToasts(UDt)
	local List = self.Toasts or {}
	for I = #List, 1, -1 do
		List[I].Time = List[I].Time - UDt
		if List[I].Time <= 0 then table.remove(List, I) end
	end
	for I = 0, 2 do
		local T = List[I + 1]
		self:Show("Toast" .. I, T ~= nil)
		if T then
			self:Set("ToastText" .. I, "Text", T.Text)
			self:Show("ToastIcon" .. I, T.Icon ~= "")
			if T.Icon ~= "" then self:Set("ToastIcon" .. I, "Texture", T.Icon) end
			local C = T.Color or { 0.98, 0.95, 0.88, 1.0 }
			local Key = "ToastText" .. I .. ".Color"
			local Value = string.format("%.2f,%.2f,%.2f", C[1], C[2], C[3])
			if self.Cache[Key] ~= Value then
				self.Cache[Key] = Value
				self:W("ToastText" .. I).Color = Vector4(C[1], C[2], C[3], 1)
			end
			self:Set("Toast" .. I, "Opacity", math.floor(math.min(1, T.Time / 0.6) * 20 + 0.5) / 20)
		end
	end
end

function Hud:UpdateHotbar()
	local GM = self.GM
	if not GM.Bag then return end
	if GM.BuildMode then
		-- 건설 막대: 설치물 아이콘, 재료가 모자라면 흐리게
		for I = 1, 9 do
			local W = I - 1
			local E = GM:BuildEntry(I)
			self:Set("SlotBg" .. W, "Texture", I == GM.BuildIndex and "UI/FarmBie/SlotSel.png" or "UI/FarmBie/Slot.png")
			self:Show("SlotIcon" .. W, E ~= nil)
			self:Set("SlotCount" .. W, "Text", "")
			if E then
				self:Set("SlotIcon" .. W, "Texture", E.Icon)
				local bOk = not E.bMax and GM:HasCost(E.Cost) and GM.Gold >= E.Gold
				self:Set("SlotIcon" .. W, "Opacity", bOk and 1.0 or 0.4)
			end
		end
		self:Show("WaterRow", false)
		if (self.ItemNameTime or 0) > 0 then
			self.ItemNameTime = self.ItemNameTime - Time.GetUnscaledDelta()
			if self.ItemNameTime <= 0 then self:Set("ItemName", "Text", "") end
		end
		return
	end
	for I = 1, 9 do
		local S = GM.Bag[I]
		local W = I - 1
		self:Set("SlotIcon" .. W, "Opacity", 1.0)
		self:Set("SlotBg" .. W, "Texture", I == GM.Selected and "UI/FarmBie/SlotSel.png" or "UI/FarmBie/Slot.png")
		self:Show("SlotIcon" .. W, S ~= nil)
		if S then
			local Info = GM:ItemInfo(S.Key)
			self:Set("SlotIcon" .. W, "Texture", Info.Icon)
			self:Set("SlotCount" .. W, "Text", (Info.Kind == "Tool") and "" or tostring(S.Count))
		else
			self:Set("SlotCount" .. W, "Text", "")
		end
	end
	local Sel = GM.Bag[GM.Selected]
	local bCan = Sel ~= nil and Sel.Key == "Can"
	self:Show("WaterRow", bCan)
	if bCan then self:Set("WaterBar", "Percent", math.floor(GM.Water / GM.Farming.CanCapacity * 100 + 0.5) / 100) end
	if (self.ItemNameTime or 0) > 0 then
		self.ItemNameTime = self.ItemNameTime - Time.GetUnscaledDelta()
		if self.ItemNameTime <= 0 then self:Set("ItemName", "Text", "") end
	end
end

function Hud:SetFade(Alpha)
	Alpha = math.max(0, math.min(1, Alpha))
	local Q = math.floor(Alpha * 50 + 0.5) / 50
	self:Show("Fade", Q > 0)
	self:Set("Fade", "Opacity", Q)
end

function Hud:OnUpdate(Dt)
	local GM = self.GM
	if not GM or not GM.Calendar then return end
	self:Set("ClockDate", "Text", GM:DateText())
	self:Set("ClockTime", "Text", GM:ClockText())
	self:Set("ClockYear", "Text", string.format("%d년차", GM.Year))
	local bMoon = GM.Phase == "Night" or GM.Hour >= GM.Calendar.NightStartHour
	self:Set("ClockIcon", "Texture", bMoon and "UI/FarmBie/Moon.png" or "UI/FarmBie/Sun.png")
	self:Set("GoldText", "Text", tostring(GM.Gold or 0))
	if GM.Vitals then
		local V = GM.Vitals
		self:Set("HealthBar", "Percent", math.floor(GM.Health / V.MaxHealth * 100 + 0.5) / 100)
		self:Set("HealthText", "Text", tostring(math.ceil(GM.Health)))
		self:Set("SanityBar", "Percent", math.floor(GM.Sanity / V.MaxSanity * 100 + 0.5) / 100)
		self:Set("SanityText", "Text", tostring(math.ceil(GM.Sanity)))
		local Buff = GM:BuffText()
		self:Set("BuffText", "Text", Buff)
		self:Show("BuffText", Buff ~= "")
	end
	self:UpdateHotbar()
	self:UpdateToasts(Time.GetUnscaledDelta())
	-- 알림 띠: 실제 시간으로 줄어든다 (일시정지 중에도 사라짐), 끝 0.5초 페이드
	if self.AnnounceTime > 0 then
		self.AnnounceTime = self.AnnounceTime - Time.GetUnscaledDelta()
		local A = math.min(1, self.AnnounceTime / 0.5, (self.AnnounceTotal - self.AnnounceTime) / 0.25)
		self:Set("Banner", "Opacity", math.floor(math.max(0, A) * 40 + 0.5) / 40)
		if self.AnnounceTime <= 0 then self:Show("Banner", false) end
	end
end

return Hud
