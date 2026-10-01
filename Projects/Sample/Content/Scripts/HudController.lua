-- 인게임 UI 예제: SampleHUD.eui의 위젯 값을 바꾸고 버튼 이벤트를 받는다
--   위젯 찾기: self.entity:GetWidget("이름") (같은 엔티티의 UIComponent)
--   값: .Text / .Percent / .Visible / .Enabled / .Opacity / .Color(sRGB Vector4) / .Texture / .FontSize
--   이벤트: OnUIClicked_<이름>, OnUIPressed_, OnUIReleased_, OnUIHoverBegin_, OnUIHoverEnd_
--          텍스트 상자: OnUITextChanged_<이름>, OnUITextCommitted_<이름> (입력 중에는 게임 키 입력이 막힌다)
--   애니메이션(.eui 타임라인): self.entity:PlayUIAnimation("이름"[, 반복, 속도]), 끝나면 OnUIAnimationFinished_<이름>
--   다국어: 디자이너에서 텍스트에 문자열 키(Localization/Strings.estrings)를 지정하면 언어를 바꿀 때 바로 바뀐다.
--          스크립트 글자는 Loc.Get("키", 인자...)로 만들고, .TextKey = "키"로 다시 키에 묶을 수 있다. L 키 = 한/영 전환
local HudController = {
	Properties = {
		ScorePerSecond = 120.0,
		MenuKey        = "M", -- 메뉴 다시 열기
		LanguageKey    = "L", -- 한국어 ↔ 영어
	},
}

function HudController:OnStart()
	self.Score      = 0
	self.Time       = 0
	self.Menu       = self.entity:GetWidget("MenuPanel")
	self.Title      = self.entity:GetWidget("Subtitle")
	self.HealthBar  = self.entity:GetWidget("HealthBar")
	self.HealthText = self.entity:GetWidget("HealthLabel")
	self.ScoreText  = self.entity:GetWidget("ScoreText")
	self.Log        = self.entity:GetWidget("LogLine0")
	self.Chat       = self.entity:GetWidget("ChatInput")
	self.bPlaying   = false
	self.NextPulse  = 500
	self.entity:PlayUIAnimation("MenuIntro")
end

function HudController:OnUpdate(dt)
	self.Time = self.Time + dt
	if self.bPlaying then
		self.Score = self.Score + self.Properties.ScorePerSecond * dt
	end
	-- 매 프레임 Loc.Get으로 만들므로 언어를 바꾸면 다음 프레임부터 바뀐다
	self.ScoreText.Text = Loc.Get("HUD.Score", math.floor(self.Score))
	if self.Score >= self.NextPulse then
		self.NextPulse = self.NextPulse + 500
		self.entity:PlayUIAnimation("ScorePulse")
	end

	-- 체력: 천천히 오르내리고, 낮으면 빨갛게
	local Health = 55 + 45 * math.sin(self.Time * 0.8)
	self.HealthBar.Percent = Health / 100
	self.HealthText.Text   = Loc.Get("HUD.Health", math.floor(Health + 0.5))
	if Health < 30 then
		self.HealthBar.Color = Vector4(0.95, 0.3, 0.25, 1)
	else
		self.HealthBar.Color = Vector4(0.3, 0.85, 0.4, 1)
	end

	if Input.IsKeyPressed(self.Properties.MenuKey) then
		self.Menu.Visible = not self.Menu.Visible
		if self.Menu.Visible then
			self.entity:PlayUIAnimation("MenuIntro")
		end
	end
	if Input.IsKeyPressed(self.Properties.LanguageKey) then
		local Next = Loc.GetLanguage() == "ko" and "en" or "ko"
		Loc.SetLanguage(Next) -- 사용자 설정에 저장 (다음 실행에도 유지)
		self.Log.Text = Loc.Get("HUD.Language", { Name = Loc.GetLanguageName(Next) })
	end
end

function HudController:OnUIClicked_PlayButton()
	self.bPlaying     = true
	self.Menu.Visible = false
	self.Log.TextKey  = "Log.Started" -- 키로 묶어 두면 언어를 바꿔도 따라 바뀐다
	Log.Info("UI: 게임 시작")
end

function HudController:OnUITextCommitted_ChatInput()
	if self.Chat.Text ~= "" then
		self.Log.Text  = Loc.Get("Chat.Me", self.Chat.Text)
		self.Chat.Text = ""
	end
end

function HudController:OnUIClicked_OptionsButton()
	self.Title.TextKey = "Menu.NoOptions"
end

function HudController:OnUIClicked_QuitButton()
	Log.Info("UI: 종료 버튼 (예제에서는 메뉴만 닫음)")
	self.Menu.Visible = false
end

function HudController:OnUIHoverBegin_PlayButton()
	self.Title.TextKey = "Menu.PlayHover"
end

function HudController:OnUIHoverEnd_PlayButton()
	self.Title.TextKey = "Menu.Subtitle"
end

return HudController
