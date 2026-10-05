-- FarmBie 타이틀 (Scenes/Title.escene의 "Title" 엔티티). 저장 슬롯 3개(이어하기/새로 시작)·설정·끝내기. 창은 FarmOptions(OptWindow).
--   슬롯을 고르면 Persistent "FarmBie_Slot"에 슬롯 이름을 남기고 Scenes/Farm.escene을 연다 → FarmGame이 그 슬롯으로 불러오거나 새로 시작한다.
--   자동 검증(Properties.AutoPlay = "Title", 이어지는 단계는 Persistent FarmBie_AutoPhase): 시험 슬롯 "TitleTest<n>"만 쓰고 끝에 지운다.
--     Title: 빈 슬롯 표시 → 설정(효과음 음량 바꿈·되돌림) → 슬롯 1 새 게임 → (농장: FarmAutoPilot RunTitleFarm — 저장·일시정지·타이틀로) → Back: 슬롯 요약
local O = Script.Require("Scripts/FarmBie/FarmOptions.lua")

local Title = {
	Properties = {
		AutoPlay = "",
	},
}

local SlotCount = 3

function Title:OnStart()
	self.HudScript = Scene.Find("Hud"):GetScript()
	self.Settings = O.LoadSettings()
	self.Camera = Scene.Find("Camera")
	self.CamBase = self.Camera and self.Camera:GetWorldPosition()
	self.Time = 0
	self.FadeIn = 1.0
	local Auto = Game.GetPersistent("FarmBie_AutoPlay", "")
	if self.Properties.AutoPlay == "" and Auto ~= "" then self.Properties.AutoPlay = Auto end
	self.SlotPrefix = self.Properties.AutoPlay ~= "" and "TitleTest" or ""
	Game.SetTimeScale(1.0)
	O.Open(self, self.HudScript, self:MainPage())
	if self.Properties.AutoPlay ~= "" then self:StartAuto() end
end

function Title:Sfx(Name)
	if self.Settings.Sfx > 0.001 then Audio.PlayOneShot("Audio/FarmBie/" .. Name .. ".wav", self.Settings.Sfx, 1.0) end
end

function Title:SlotId(I)
	return self.SlotPrefix .. I
end

function Title:MainPage(Index)
	local Items = {}
	for I = 1, SlotCount do
		Items[#Items + 1] = { Label = "슬롯 " .. I, Value = function() return O.SlotSummary("FarmBie_" .. self:SlotId(I)) or "비어 있음 — 새 게임" end,
		                      Act = function() self:ChooseSlot(I) end }
	end
	Items[#Items + 1] = { Label = "설정", Act = function()
		O.Open(self, self.HudScript, O.SettingsPage(self.Settings, function(S) self.Settings = S end, function()
			O.Open(self, self.HudScript, self:MainPage(SlotCount + 1), SlotCount + 1)
		end))
	end }
	Items[#Items + 1] = { Label = "끝내기", Act = function() Game.Quit() end }
	return { Title = "시작하기", Sub = "E 고르기   W/S 움직이기   Esc 뒤로", Items = Items }
end

function Title:ChooseSlot(I)
	local Name = "FarmBie_" .. self:SlotId(I)
	local Summary = O.SlotSummary(Name)
	if not Summary then
		self:Start(I, true)
		return
	end
	local function Back() O.Open(self, self.HudScript, self:MainPage(), I) end
	O.Open(self, self.HudScript, { Title = "슬롯 " .. I, Sub = Summary, Back = Back, Items = {
		{ Label = "이어하기", Act = function() self:Start(I, false) end },
		{ Label = "새로 시작", Value = function() return "저장을 지운다" end, Act = function()
			O.Open(self, self.HudScript, O.ConfirmPage("새로 시작할까?", "슬롯 " .. I .. "의 저장이 사라진다", function() self:Start(I, true) end, Back))
		end },
		{ Label = "뒤로", Act = Back },
	} })
end

function Title:Start(I, bNew)
	if self.StartTimer then return end
	if bNew then SaveGame.Delete("FarmBie_" .. self:SlotId(I)) end
	Game.SetPersistent("FarmBie_Slot", self:SlotId(I))
	Game.SetPersistent("FarmBie_Session", nil)
	self.StartTimer = 0
	O.Close(self, self.HudScript)
	Log.Info(string.format("[FarmBie] 타이틀: 슬롯 %s %s", self:SlotId(I), bNew and "새 게임" or "이어하기"))
end

function Title:OnUpdate(Dt)
	local UDt = Time.GetUnscaledDelta()
	self.Time = self.Time + UDt
	local Hud = self.HudScript
	-- 카메라: 농장 위를 천천히 오간다
	if self.Camera and self.CamBase then
		self.Camera:SetPosition(self.CamBase + Vector3(math.sin(self.Time * 0.05) * 500, math.sin(self.Time * 0.031) * 150, 0))
	end
	if self.StartTimer then
		self.StartTimer = self.StartTimer + UDt
		Hud:SetFade(self.StartTimer / 0.5)
		if self.StartTimer >= 0.55 and not self.bSent then
			self.bSent = true
			Game.OpenScene("Scenes/Farm.escene")
		end
		return
	end
	if self.FadeIn > 0 then
		self.FadeIn = math.max(0, self.FadeIn - UDt * 1.5)
		Hud:SetFade(self.FadeIn)
	end
	local In
	if self.Auto then
		In = self:StepAuto(UDt)
	else
		In = O.ReadInput(self)
	end
	O.Input(self, Hud, In, function(Name) self:Sfx(Name) end)
end

-- ================================================================ 자동 검증
function Title:StartAuto()
	local Phase = Game.GetPersistent("FarmBie_AutoPhase", "")
	self.Auto = { Failures = {}, Checks = 0, Time = 0 }
	if Phase ~= "" then
		for Item in string.gmatch(Game.GetPersistent("FarmBie_AutoFailures", ""), "[^|]+") do self.Auto.Failures[#self.Auto.Failures + 1] = Item end
		self.Auto.Checks = Game.GetPersistent("FarmBie_AutoChecks", 0)
		self.Auto.Time = Game.GetPersistent("FarmBie_AutoTime", 0)
	end
	local Runner = self["RunTitle" .. Phase]
	self.Auto.Co = coroutine.create(function() Runner(self) end)
end

function Title:StepAuto(UDt)
	local A = self.Auto
	A.Time = A.Time + UDt
	A.In = {}
	if A.Co and coroutine.status(A.Co) ~= "dead" then
		local bOk, Err = coroutine.resume(A.Co)
		if not bOk then
			Log.Error("[FarmBie] 자동 조종 오류: " .. tostring(Err))
			A.Failures[#A.Failures + 1] = "자동 조종 오류"
			A.Co = nil
			self:FinishAuto()
		end
	end
	local In = A.In
	In.Confirm = In.Confirm or In.Interact
	In.Cancel = In.Cancel or In.Pause
	return In
end

function Title:Expect(bOk, What)
	local A = self.Auto
	A.Checks = A.Checks + 1
	if not bOk then A.Failures[#A.Failures + 1] = What end
	Log.Info(string.format("[FarmBie] 자동 Title %.1fs: 확인%s %s", A.Time, bOk and "" or " 실패", What))
end

function Title:Wait(Seconds)
	local Until = self.Auto.Time + Seconds
	while self.Auto.Time < Until do coroutine.yield() end
end

function Title:Press(Field)
	self.Auto.In[Field] = true
	coroutine.yield()
	coroutine.yield()
end

function Title:Cell(Name)
	return self.HudScript.Cache[Name] or ""
end

function Title:FinishAuto()
	local A = self.Auto
	if A.bFinished then return end
	A.bFinished = true
	for I = 1, SlotCount do SaveGame.Delete("FarmBie_TitleTest" .. I) end
	for _, K in ipairs({ "FarmBie_AutoPhase", "FarmBie_AutoPlay", "FarmBie_Slot" }) do Game.SetPersistent(K, nil) end
	Log.Info(string.format("[FarmBie] 자동 플레이 요약: %.0f초, 확인 %d건", A.Time, A.Checks))
	if #A.Failures == 0 then
		Log.Info("[FarmBie] 결과: 실패 0건")
	else
		Log.Error("[FarmBie] 결과: 실패 " .. #A.Failures .. "건 — " .. table.concat(A.Failures, ", "))
	end
end

function Title:RunTitle()
	for I = 1, SlotCount do SaveGame.Delete("FarmBie_TitleTest" .. I) end
	self:Wait(1.0)
	self:Expect(self:Cell("OptWindow.Visibility") == "HitTestInvisible" and string.find(self:Cell("OptPrice0.Text"), "비어 있음") ~= nil, "빈 슬롯 표시")
	self:Expect(self:Cell("OptName3.Text") == "설정" and self:Cell("OptName4.Text") == "끝내기", "설정·끝내기 줄")
	-- 설정: 효과음 음량 한 칸 내리고 되돌림 (저장까지)
	local Old = self.Settings.Sfx
	for _ = 1, 3 do self:Press("MenuDown") end
	self:Press("Confirm")
	self:Expect(self:Cell("OptTitle.Text") == "설정", "설정 쪽")
	self:Press("MenuLeft")
	self:Expect(math.abs(self.Settings.Sfx - (Old - 0.1)) < 0.01 or Old < 0.05, string.format("효과음 %.1f → %.1f", Old, self.Settings.Sfx))
	self:Press("MenuRight")
	self:Press("Cancel")
	self:Expect(math.abs(O.LoadSettings().Sfx - Old) < 0.01 and self:Cell("OptName0.Text") == "슬롯 1", "설정 저장·되돌림")
	-- 슬롯 1 새 게임 → 농장
	for _ = 1, 3 do self:Press("MenuUp") end
	Game.SetPersistent("FarmBie_AutoPlay", "Title")
	Game.SetPersistent("FarmBie_AutoPhase", "Farm")
	Game.SetPersistent("FarmBie_AutoFailures", table.concat(self.Auto.Failures, "|"))
	Game.SetPersistent("FarmBie_AutoChecks", self.Auto.Checks)
	Game.SetPersistent("FarmBie_AutoTime", self.Auto.Time)
	self:Press("Confirm")
	self:Wait(5)
	self:Expect(false, "농장으로 가지 못함")
	self:FinishAuto()
end

function Title:RunTitleBack()
	self:Wait(1.0)
	self:Expect(string.find(self:Cell("OptPrice0.Text"), "1년차 봄") ~= nil, "슬롯 1 요약 " .. self:Cell("OptPrice0.Text"))
	self:Press("Confirm")
	self:Expect(self:Cell("OptName0.Text") == "이어하기" and self:Cell("OptName1.Text") == "새로 시작", "이어하기/새로 시작")
	self:Press("Cancel")
	self:Expect(self:Cell("OptName0.Text") == "슬롯 1", "뒤로")
	self:FinishAuto()
end

return Title
