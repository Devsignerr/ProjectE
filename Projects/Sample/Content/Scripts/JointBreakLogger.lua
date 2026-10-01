-- 관절/충돌 알림 예제 (Phase 30): 관절 컴포넌트가 있는 엔티티에 붙인다. 세게 맞으면 충격 세기를, 관절이 끊어지면 그 힘을 기록한다
local JointBreakLogger = {
	Properties = {
		MinImpulse = 2000.0, -- 이보다 센 충돌만 기록 (kg·cm/s)
	},
}

function JointBreakLogger:OnCollisionBegin(other, info)
	if info.Impulse >= self.Properties.MinImpulse then
		Log.Info(self.entity:GetName(), "충돌:", other and other:GetName() or "?", string.format("충격 %.0f kg·cm/s, 속력 %.0f cm/s", info.Impulse, info.Speed))
	end
end

function JointBreakLogger:OnJointBreak(other, force)
	Log.Info(self.entity:GetName(), string.format("관절이 끊어졌다 (%.0f N)", force))
end

return JointBreakLogger
