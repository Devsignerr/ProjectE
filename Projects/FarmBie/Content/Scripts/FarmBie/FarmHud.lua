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
	-- 알림 띠: 실제 시간으로 줄어든다 (일시정지 중에도 사라짐), 끝 0.5초 페이드
	if self.AnnounceTime > 0 then
		self.AnnounceTime = self.AnnounceTime - Time.GetUnscaledDelta()
		local A = math.min(1, self.AnnounceTime / 0.5, (self.AnnounceTotal - self.AnnounceTime) / 0.25)
		self:Set("Banner", "Opacity", math.floor(math.max(0, A) * 40 + 0.5) / 40)
		if self.AnnounceTime <= 0 then self:Show("Banner", false) end
	end
end

return Hud
