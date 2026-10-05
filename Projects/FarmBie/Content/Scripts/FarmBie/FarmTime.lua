-- FarmBie 시간·달력·낮밤 (FarmGame에 섞이는 메서드 모음 — FarmGame.lua가 Script.Require로 합친다).
--   시각 self.Hour: DayStartHour(6) → NightStartHour(20) → NightEndHour + 24(26 = 다음 날 2시). 낮과 밤은 실제 길이가 따로 (Calendar.edata).
--   단계 self.Phase: "Day" 농사·탐색 / "Night" 디펜스(밤 길이 = 제한 시간) / "Sleep" 잠드는 전환(화면 어둡게 → 날짜 넘김·저장 → 밝게)
--   날짜: Day(1~SeasonDays), Season(0~3), Year(1~). 요일 = 누적 일수 기준 (1년차 봄 1일 = 첫 요일)
--   알림: 밤 시작·새 날·계절 바뀜·계절 끝 경고(SeasonWarnDays) → HUD 띠. 다른 모듈은 훅으로 받는다:
--     GM:OnNightStart() / GM:OnDayStart(bNewSeason) / GM:OnSeasonChanged(OldSeason) — 있으면 부른다 (농사·디펜스 단계가 채운다)
--   시간은 게임 Dt(Time.GetDelta — 일시정지 배율 0이면 멈춤). 화면(하늘·색 보정·등불)은 0.02시 단위로만 갱신 (DayNightKeys.etable 보간)
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Time_ = {}

local function Lerp(A, B, T) return A + (B - A) * T end

function Time_:InitTime()
	local C = D.Calendar()
	self.Calendar = C
	self.Day, self.Season, self.Year = 1, 0, 1
	self.Hour = C.DayStartHour
	self.Phase = "Day"
	self.SleepTimer = 0
	-- 화면 대상 (씬 이름 약속: Sun, Sky, Camera, Lamp_<n>)
	local Sun, Sky, Cam = Scene.Find("Sun"), Scene.Find("Sky"), Scene.Find("Camera")
	self.SunLight = Sun and Sun:GetComponent("DirectionalLightComponent")
	self.SkyLight = Sky and Sky:GetComponent("SkyLightComponent")
	self.Atmosphere = Sky and Sky:GetComponent("SkyAtmosphereComponent")
	self.Fog = Sky and Sky:GetComponent("HeightFogComponent")
	self.Grading = Cam and Cam:GetComponent("ColorGradingComponent")
	self.Vignette = Cam and Cam:GetComponent("VignetteComponent")
	self.SunBase = self.SunLight and self.SunLight.Intensity or 1
	self.SkyBase = self.SkyLight and self.SkyLight.Intensity or 1
	self.Lamps = {}
	for I = 0, 63 do
		local L = Scene.Find("Lamp_" .. I)
		if not L then break end
		local P = L:GetComponent("PointLightComponent")
		self.Lamps[#self.Lamps + 1] = { Light = P, Base = 4.0, Radius = P.Radius }
	end
end

-- ---- 달력 문자열
function Time_:TotalDays()
	return (self.Year - 1) * 4 * self.Calendar.SeasonDays + self.Season * self.Calendar.SeasonDays + self.Day
end

function Time_:Weekday() -- 0 = 첫 요일
	return (self:TotalDays() - 1) % #self.Calendar.WeekdayNames
end

function Time_:SeasonName(Season)
	return self.Calendar.SeasonNames[(Season or self.Season) + 1]
end

function Time_:DateText()
	return string.format("%s %d일 (%s)", self:SeasonName(), self.Day, self.Calendar.WeekdayNames[self:Weekday() + 1])
end

function Time_:ClockText()
	-- 10분 단위 (농장 게임 관례)
	local H = self.Hour % 24
	local Minutes = math.floor(H * 6) * 10
	return string.format("%02d:%02d", math.floor(Minutes / 60), Minutes % 60)
end

function Time_:IsNight()
	return self.Phase == "Night"
end

-- 오늘 밤이 보스 밤인가 (10·20·30일차 = 중간 보스, 마지막 날 = 계절 보스도)
function Time_:IsBossNight()
	return self.Day % self.Calendar.BossEveryDays == 0
end

function Time_:IsSeasonBossNight()
	return self.Day == self.Calendar.SeasonDays
end

function Time_:DaysLeftInSeason()
	return self.Calendar.SeasonDays - self.Day
end

-- ---- 진행
function Time_:HoursPerSecond()
	local C = self.Calendar
	if self.Phase == "Night" then
		return (C.NightEndHour + 24 - C.NightStartHour) / (C.NightRealMinutes * 60)
	end
	return (C.NightStartHour - C.DayStartHour) / (C.DayRealMinutes * 60)
end

-- 밤 남은 실제 초 (디펜스 제한 시간 표시)
function Time_:NightSecondsLeft()
	if self.Phase ~= "Night" then return 0 end
	return math.max(0, (self.Calendar.NightEndHour + 24 - self.Hour) / self:HoursPerSecond())
end

function Time_:UpdateTime(Dt)
	local C = self.Calendar
	if self.Phase == "Sleep" then
		self:UpdateSleep(Dt)
		return
	end
	self.Hour = self.Hour + Dt * self:HoursPerSecond()
	if self.Phase == "Day" and self.Hour >= C.NightStartHour then
		self.Phase = "Night"
		self:Hud():Announce("밤이 찾아온다", self:IsSeasonBossNight() and "계절의 마지막 밤 — 무언가 다가온다" or
			(self:IsBossNight() and "불길한 기운이 느껴진다" or "날이 밝을 때까지 버텨라"), 3.5)
		Log.Info(string.format("[FarmBie] 밤 시작: %s", self:DateText()))
		if self.OnNightStart then self:OnNightStart() end
	elseif self.Phase == "Night" and self.Hour >= C.NightEndHour + 24 then
		self:BeginSleep("Dawn")
	end
	self:ApplyDayNight(false)
end

-- 잠들기 시작 (Reason: "Bed" 스스로 잠 / "Dawn" 새벽이 되어 쓰러지듯 잠)
function Time_:BeginSleep(Reason)
	if self.Phase == "Sleep" then return end
	self.Phase = "Sleep"
	self.SleepReason = Reason
	self.SleepTimer = 0
	self.bSlept = false
	Log.Info(string.format("[FarmBie] 잠듦 (%s): %s %s", Reason, self:DateText(), self:ClockText()))
end

local SleepFadeIn, SleepHold, SleepFadeOut = 1.0, 0.6, 1.0

function Time_:UpdateSleep(Dt)
	-- 화면은 게임 Dt가 아닌 실제 시간으로 (일시정지와 무관하게 전환이 끝나야 한다)
	self.SleepTimer = self.SleepTimer + Time.GetUnscaledDelta()
	local T = self.SleepTimer
	if T < SleepFadeIn then
		self:Hud():SetFade(T / SleepFadeIn)
	elseif not self.bSlept then
		self:Hud():SetFade(1)
		self.bSlept = true
		self:AdvanceDay()
	elseif T < SleepFadeIn + SleepHold then
		self:Hud():SetFade(1)
	elseif T < SleepFadeIn + SleepHold + SleepFadeOut then
		self:Hud():SetFade(1 - (T - SleepFadeIn - SleepHold) / SleepFadeOut)
	else
		self:Hud():SetFade(0)
		self.Phase = "Day"
		local Sub = nil
		local Left = self:DaysLeftInSeason()
		if self.bNewSeason then
			Sub = string.format("%s이(가) 왔다 — 새 계절의 씨앗을 심자", self:SeasonName())
		elseif Left < self.Calendar.SeasonWarnDays then
			Sub = Left == 0 and "오늘이 계절의 마지막 날 — 작물이 곧 시든다!" or string.format("계절이 %d일 뒤 바뀐다 — 작물 수확을 서두르자", Left)
		end
		local Notes = self.MorningNotes or {}
		if Sub then table.insert(Notes, 1, Sub) end
		local Text = #Notes > 0 and table.concat(Notes, "   ·   ") or string.format("%d년차 아침", self.Year)
		self.MorningNotes = nil
		if self.OnWake and self:OnWake(self:DateText(), Text) then return end
		self:Hud():Announce(self:DateText(), Text, 3.5)
	end
end

-- 날짜 넘김 (+ 계절·연도) → 아침 → 저장
function Time_:AdvanceDay()
	local C = self.Calendar
	local OldSeason = self.Season
	self.Day = self.Day + 1
	self.bNewSeason = false
	if self.Day > C.SeasonDays then
		self.Day = 1
		self.Season = self.Season + 1
		if self.Season > 3 then
			self.Season = 0
			self.Year = self.Year + 1
		end
		self.bNewSeason = true
	end
	self.Hour = C.DayStartHour
	self.Report.Days = (self.Report.Days or 0) + 1
	if self.bNewSeason and self.OnSeasonChanged then self:OnSeasonChanged(OldSeason) end
	if self.OnDayStart then self:OnDayStart(self.bNewSeason) end
	self:ApplyDayNight(true)
	self:WakePlayer()
	self:SaveGame()
	Log.Info(string.format("[FarmBie] 새 날: %d년차 %s", self.Year, self:DateText()))
end

-- 시각을 바로 바꿈 (자동 검증·디버그). 단계도 시각에 맞춘다 (밤 시작 훅은 부르지 않음)
function Time_:SetHour(Hour)
	local C = self.Calendar
	if Hour < C.DayStartHour then Hour = Hour + 24 end
	self.Hour = Hour
	self.Phase = (Hour >= C.NightStartHour) and "Night" or "Day"
	self:ApplyDayNight(true)
end

-- ---- 화면 (하늘·색 보정·등불)
function Time_:SampleKeys(Hour)
	local Keys = D.DayNightKeys()
	if not Keys or #Keys == 0 then return nil end
	local H = Hour % 24
	local Prev, Next, PH, NH = Keys[#Keys], Keys[1], Keys[#Keys].Hour - 24, Keys[1].Hour
	for I = 1, #Keys do
		if Keys[I].Hour > H then
			Next, NH = Keys[I], Keys[I].Hour
			if I > 1 then Prev, PH = Keys[I - 1], Keys[I - 1].Hour end
			break
		end
		Prev, PH = Keys[I], Keys[I].Hour
		Next, NH = Keys[I + 1] or Keys[1], Keys[I + 1] and Keys[I + 1].Hour or (Keys[1].Hour + 24)
	end
	local T = (NH - PH) > 1e-4 and (H - PH) / (NH - PH) or 0
	local Out = {}
	for K, V in pairs(Prev) do
		local N = Next[K]
		if type(V) == "number" and type(N) == "number" then
			Out[K] = Lerp(V, N, T)
		elseif type(V) == "table" and type(N) == "table" then
			Out[K] = { Lerp(V[1], N[1], T), Lerp(V[2], N[2], T), Lerp(V[3], N[3], T) }
		end
	end
	return Out
end

-- 게임 시각 → 하늘 시각 (Calendar.SkyDayStart/SkyNightStart/SkyNightEnd — 하늘 모델의 해 지는 시각에 맞춤)
function Time_:GetSkyHour()
	local C = self.Calendar
	local H = self.Hour
	if H < C.NightStartHour then
		local T = (H - C.DayStartHour) / (C.NightStartHour - C.DayStartHour)
		return Lerp(C.SkyDayStart, C.SkyNightStart, T)
	end
	local T = (H - C.NightStartHour) / (C.NightEndHour + 24 - C.NightStartHour)
	return Lerp(C.SkyNightStart, C.SkyNightEnd + 24, T) % 24
end

function Time_:ApplyDayNight(bForce)
	local Q = math.floor(self:GetSkyHour() / 0.02 + 0.5) * 0.02
	if not bForce and Q == self.AppliedSkyHour then return end
	self.AppliedSkyHour = Q
	Sky.SetTimeOfDay(Q)
	local K = self:SampleKeys(Q)
	if not K then return end
	if self.SkyLight then self.SkyLight.Intensity = self.SkyBase * K.SkyScale end
	if self.SunLight then self.SunLight.Intensity = self.SunBase * K.SunScale end
	local G = self.Grading
	if G then
		G.Temperature, G.Tint, G.Saturation, G.Contrast = K.Temperature, K.Tint, K.Saturation, K.Contrast
		G.Lift = Vector3(K.Lift[1], K.Lift[2], K.Lift[3])
		G.Gamma = Vector3(K.Gamma[1], K.Gamma[2], K.Gamma[3])
		G.Gain = Vector3(K.Gain[1] * K.Exposure, K.Gain[2] * K.Exposure, K.Gain[3] * K.Exposure)
	end
	-- 정신력이 낮으면 화면이 바래고 가장자리가 어두워진다 (FarmVitals:FearLevel)
	local Fear = self.FearLevel and self:FearLevel() or 0
	if G and Fear > 0 then
		G.Saturation = K.Saturation * (1 - 0.45 * Fear)
		G.Temperature = K.Temperature - 0.15 * Fear
	end
	if self.Vignette then self.Vignette.Intensity = K.Vignette + 0.35 * Fear end
	if self.Atmosphere then self.Atmosphere.MoonIntensity = K.Moon end
	if self.Fog then
		self.Fog.Color = Vector3(K.Fog[1], K.Fog[2], K.Fog[3])
		self.Fog.DirectionalInscatteringColor = Vector3(K.Inscatter[1], K.Inscatter[2], K.Inscatter[3])
	end
	local Level = K.LampScale
	if bForce or math.abs(Level - (self.AppliedLamp or -1)) > 0.004 then
		self.AppliedLamp = Level
		for _, L in ipairs(self.Lamps) do
			L.Light.Intensity = L.Base * Level
			L.Light.Radius = L.Radius * (0.8 + 0.2 * Level)
		end
	end
end

-- ---- 저장 조각 (FarmGame:BuildSave/ApplySave가 모은다)
function Time_:SaveTime(T)
	T.Day, T.Season, T.Year = self.Day, self.Season, self.Year
end

function Time_:LoadTime(T)
	self.Day = math.max(1, math.floor(T.Day or 1))
	self.Season = math.max(0, math.min(3, math.floor(T.Season or 0)))
	self.Year = math.max(1, math.floor(T.Year or 1))
	self.Hour = self.Calendar.DayStartHour
	self.Phase = "Day"
end

return Time_
