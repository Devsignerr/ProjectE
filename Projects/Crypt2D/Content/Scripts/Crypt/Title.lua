-- Crypt2D 타이틀 (Scenes/Title.escene — UI/Crypt/Title.eui). 시작(Enter·단추) → Scenes/Crypt.escene, 종료.
--   최고 기록(SaveGame "Crypt2D")을 보여 주고, 배경 산·묘지 스프라이트를 층마다 다른 폭으로 천천히 흔들어 패럴랙스를 만든다.
local Title = {
	Properties = {},
}

function Title:OnStart()
	Game.SetInputMode("GameAndUI")
	Game.SetMouseLocked(false)
	local R = SaveGame.Load("Crypt2D")
	local Text = "첫 도전입니다 — 지하 묘지 끝의 제단을 찾아라"
	if R and (R.Runs or 0) > 0 then
		Text = string.format("최고 기록  %d층 · 처치 %d · 승리 %d회%s", R.BestFloor or 0, R.BestKills or 0, R.Wins or 0,
			R.BestTime and string.format(" · 최단 %d:%02d", R.BestTime // 60, R.BestTime % 60) or "")
	end
	self.entity:GetWidget("RecordText").Text = Text
	self.Layers = {}
	for _, Spec in ipairs({ { "Mountains0", 60 }, { "Mountains1", 60 }, { "Mountains2", 60 }, { "Graveyard0", 140 }, { "Graveyard1", 140 } }) do
		local E = Scene.Find(Spec[1])
		if E then
			local P = E:GetPosition()
			self.Layers[#self.Layers + 1] = { Entity = E, X = P.X, Y = P.Y, Z = P.Z, Speed = Spec[2] }
		end
	end
	self.Time = 0
	self.bStarting = false
end

function Title:OnUpdate(Dt)
	self.Time = self.Time + Dt
	for _, L in ipairs(self.Layers) do
		-- 앞 층일수록 크게 좌우로 흔들린다 (화면 가장자리가 비지 않는 폭)
		local X = L.X + math.sin(self.Time * 0.35) * L.Speed
		L.Entity:SetPosition(Vector3(X, L.Y, L.Z))
	end
	if Input.WasActionPressed("Confirm") then
		self:StartGame()
	end
end

function Title:StartGame()
	if self.bStarting then return end
	self.bStarting = true
	Audio.PlayOneShot("Audio/Crypt/UIConfirm.wav")
	Game.OpenScene("Scenes/Crypt.escene")
end

function Title:OnUIClicked_StartButton()
	self:StartGame()
end

function Title:OnUIClicked_QuitButton()
	Game.Quit()
end

return Title
