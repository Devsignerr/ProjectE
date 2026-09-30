-- 카메라를 시작 위치 기준으로 좌우(월드 X-Y 대각선)로 천천히 왕복시킨다 (픽셀 아트 도트 스냅 확인용)
local CameraPan = {
	Properties = {
		Amplitude = 150.0,           -- cm
		Frequency = 0.08,            -- Hz
		Direction = Vector3(1, -1, 0), -- 이동 방향 (정규화하지 않아도 됨)
	},
}

function CameraPan:OnStart()
	self.Origin = self.entity:GetPosition()
	self.Time   = 0.0
end

function CameraPan:OnUpdate(dt)
	self.Time = self.Time + dt
	local P      = self.Properties
	local Offset = math.sin(self.Time * P.Frequency * 2.0 * math.pi) * P.Amplitude
	self.entity:SetPosition(self.Origin + P.Direction:Normalized() * Offset)
end

return CameraPan
