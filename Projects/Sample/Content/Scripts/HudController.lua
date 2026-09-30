-- 인게임 UI 예제: SampleHUD.eui의 위젯 값을 바꾸고 버튼 이벤트를 받는다
--   위젯 찾기: self.entity:GetWidget("이름") (같은 엔티티의 UIComponent)
--   값: .Text / .Percent / .Visible / .Enabled / .Opacity / .Color(sRGB Vector4) / .Texture / .FontSize
--   이벤트: OnUIClicked_<이름>, OnUIPressed_, OnUIReleased_, OnUIHoverBegin_, OnUIHoverEnd_
--          텍스트 상자: OnUITextChanged_<이름>, OnUITextCommitted_<이름> (입력 중에는 게임 키 입력이 막힌다)
local HudController = {
	Properties = {
		ScorePerSecond = 120.0,
		MenuKey        = "M", -- 메뉴 다시 열기
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
end

function HudController:OnUpdate(dt)
	self.Time = self.Time + dt
	if self.bPlaying then
		self.Score = self.Score + self.Properties.ScorePerSecond * dt
	end
	self.ScoreText.Text = string.format("점수 %d", math.floor(self.Score))

	-- 체력: 천천히 오르내리고, 낮으면 빨갛게
	local Health = 55 + 45 * math.sin(self.Time * 0.8)
	self.HealthBar.Percent = Health / 100
	self.HealthText.Text   = string.format("체력 %d / 100", math.floor(Health + 0.5))
	if Health < 30 then
		self.HealthBar.Color = Vector4(0.95, 0.3, 0.25, 1)
	else
		self.HealthBar.Color = Vector4(0.3, 0.85, 0.4, 1)
	end

	if Input.IsKeyPressed(self.Properties.MenuKey) then
		self.Menu.Visible = not self.Menu.Visible
	end
end

function HudController:OnUIClicked_PlayButton()
	self.bPlaying     = true
	self.Menu.Visible = false
	self.Log.Text     = "게임을 시작했습니다. (M: 메뉴)"
	Log.Info("UI: 게임 시작")
end

function HudController:OnUITextCommitted_ChatInput()
	if self.Chat.Text ~= "" then
		self.Log.Text  = "나: " .. self.Chat.Text
		self.Chat.Text = ""
	end
end

function HudController:OnUIClicked_OptionsButton()
	self.Title.Text = "설정은 아직 없습니다"
end

function HudController:OnUIClicked_QuitButton()
	Log.Info("UI: 종료 버튼 (예제에서는 메뉴만 닫음)")
	self.Menu.Visible = false
end

function HudController:OnUIHoverBegin_PlayButton()
	self.Title.Text = "클릭하면 메뉴가 닫힙니다"
end

function HudController:OnUIHoverEnd_PlayButton()
	self.Title.Text = "인게임 UI 샘플"
end

return HudController
