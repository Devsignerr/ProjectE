-- 컷신 데모 (Demo_Cinematic): SequencePlayerComponent가 있는 엔티티에 붙인다.
--   시퀀스 이벤트 트랙의 이름마다 OnSequenceEvent_<이름>()이 불리고, 끝나면 OnSequenceFinished().
--   끝나고 ReplayDelay초 뒤 처음부터 다시 재생한다. R 키 = 즉시 다시, P 키 = 일시정지/계속
local CinematicDirector = {
	Properties = {
		ReplayDelay = 3.0, -- 초 (음수면 다시 재생하지 않음)
	},
}

function CinematicDirector:OnStart()
	self.Wait   = -1
	self.Paused = false
	Log.Info("컷신 시작 — 길이", self.entity:GetSequenceDuration(), "초")
end

function CinematicDirector:OnSequenceEvent_DoorOpen()
	Log.Info("시퀀스 이벤트: 문이 열립니다 (", self.entity:GetSequenceTime(), "초)")
end

function CinematicDirector:OnSequenceEvent_FoxEnter()
	Log.Info("시퀀스 이벤트: 여우 등장")
end

function CinematicDirector:OnSequenceFinished()
	Log.Info("컷신 끝 — 카메라가 기본 카메라로 돌아갑니다")
	self.Wait = self.Properties.ReplayDelay
end

function CinematicDirector:OnUpdate(dt)
	if Input.IsKeyPressed("R") then
		self.Wait = -1
		self.entity:PlaySequence()
	end
	if Input.IsKeyPressed("P") then
		if self.entity:IsSequencePlaying() then
			self.entity:PauseSequence()
		else
			self.entity:PlaySequence(nil, -1) -- 멈춘 자리에서 이어서
		end
	end
	if self.Wait >= 0 then
		self.Wait = self.Wait - dt
		if self.Wait < 0 then
			self.entity:PlaySequence()
		end
	end
end

return CinematicDirector
