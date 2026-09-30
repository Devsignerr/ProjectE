-- 애니메이션 노티파이 예제: 여우 Walk 클립의 발소리/보폭 이벤트를 받아 로그로 남긴다 (Fox.glb.emeta)
local FootstepLogger = { Properties = {} }

function FootstepLogger:OnStart()
	self.Steps = 0
end

function FootstepLogger:OnAnimNotify_Footstep_L()
	self.Steps = self.Steps + 1
	Log.Info("왼발 (" .. self.Steps .. "걸음)")
end

function FootstepLogger:OnAnimNotify_Footstep_R()
	self.Steps = self.Steps + 1
	Log.Info("오른발 (" .. self.Steps .. "걸음)")
end

function FootstepLogger:OnAnimNotifyBegin_Stride()
	Log.Info("보폭 시작")
end

function FootstepLogger:OnAnimNotifyEnd_Stride()
	Log.Info("보폭 끝")
end

return FootstepLogger
