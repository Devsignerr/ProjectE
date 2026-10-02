-- 쇼케이스 감독 (Demo_Showcase의 "Showcase" 엔티티: SequencePlayerComponent + UIComponent(UI/ShowcaseHUD.eui) + 이 스크립트)
--   시작하면 시퀀스 Sequences/ShowcaseTour.esequence가 자동 재생 (구역별 카메라 컷). 이벤트 트랙 Zone_<이름>마다 HUD 구역 이름/설명을 바꾼다.
--   투어가 끝나거나 Space/Enter를 누르면 시퀀스를 멈추고(컷 카메라 해제) Player 여우의 ShowcasePlayer:SetControlEnabled(true)
--   HUD 글자는 모두 문자열 키(Localization/Strings.estrings의 Showcase.*) — L 키로 한/영 전환
local ShowcaseDirector = {
	Properties = {
		PlayerName = "Player",
	},
}

local ZoneKeys = {
	Intro     = "Showcase.Zone.Intro",
	Character = "Showcase.Zone.Character",
	Physics   = "Showcase.Zone.Physics",
	Materials = "Showcase.Zone.Materials",
	Lights    = "Showcase.Zone.Lights",
	Campfire  = "Showcase.Zone.Campfire",
	AI        = "Showcase.Zone.AI",
	Plaza     = "Showcase.Zone.Plaza",
}

function ShowcaseDirector:OnStart()
	self.ZoneText = self.entity:GetWidget("ZoneText")
	self.DescText = self.entity:GetWidget("DescText")
	self.HintText = self.entity:GetWidget("HintText")
	self.Touring  = true
	self:ShowZone("Intro")
	self.HintText.TextKey = "Showcase.Hint.Tour"
	Log.Info("쇼케이스 투어 시작 — 길이", self.entity:GetSequenceDuration(), "초 (Space/Enter: 건너뛰기)")
end

function ShowcaseDirector:ShowZone(Name)
	local Key = ZoneKeys[Name]
	if Key == nil then return end
	self.ZoneText.TextKey = Key
	self.DescText.TextKey = Key .. ".Desc"
	self.entity:PlayUIAnimation("ZoneIn")
	Log.Info("쇼케이스 구역:", Name, string.format("(%.1f초)", self.entity:GetSequenceTime()))
end

-- 시퀀스 이벤트 트랙 (구역마다 하나)
function ShowcaseDirector:OnSequenceEvent_Zone_Intro()     self:ShowZone("Intro") end
function ShowcaseDirector:OnSequenceEvent_Zone_Character() self:ShowZone("Character") end
function ShowcaseDirector:OnSequenceEvent_Zone_Physics()   self:ShowZone("Physics") end
function ShowcaseDirector:OnSequenceEvent_Zone_Materials() self:ShowZone("Materials") end
function ShowcaseDirector:OnSequenceEvent_Zone_Lights()    self:ShowZone("Lights") end
function ShowcaseDirector:OnSequenceEvent_Zone_Campfire()  self:ShowZone("Campfire") end
function ShowcaseDirector:OnSequenceEvent_Zone_AI()        self:ShowZone("AI") end
function ShowcaseDirector:OnSequenceEvent_Zone_Plaza()     self:ShowZone("Plaza") end

function ShowcaseDirector:OnSequenceFinished()
	self:BeginControl()
end

function ShowcaseDirector:BeginControl()
	if not self.Touring then return end
	self.Touring = false
	self.entity:StopSequence() -- 컷 카메라 해제 (끝까지 재생된 경우에도 안전)
	self:ShowZone("Plaza")
	self.HintText.TextKey = "Showcase.Hint.Play"
	local Player = Scene.Find(self.Properties.PlayerName)
	local Script = Player and Player:GetScript()
	if Script and Script.SetControlEnabled then
		Script:SetControlEnabled(true)
	else
		Log.Warn("쇼케이스: 플레이어 스크립트를 찾지 못했습니다:", self.Properties.PlayerName)
	end
end

function ShowcaseDirector:OnUpdate(dt)
	if self.Touring and (Input.IsKeyPressed("Space") or Input.IsKeyPressed("Enter")) then
		Log.Info("쇼케이스: 투어 건너뛰기 (", string.format("%.1f", self.entity:GetSequenceTime()), "초)")
		self:BeginControl()
	end
	if Input.IsKeyPressed("L") then
		local Next = Loc.GetLanguage() == "ko" and "en" or "ko"
		Loc.SetLanguage(Next)
	end
end

return ShowcaseDirector
