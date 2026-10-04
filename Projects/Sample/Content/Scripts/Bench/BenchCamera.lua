-- 엔진 비교 벤치(BenchSquare) 카메라 궤도 — Tools/DemoMap/BuildBenchSquare.py가 생성 (Godot bench_square.gd와 같은 식)
--   20초에 한 바퀴. 고정 dt(--fixed-delta 60)로 돌리면 두 엔진이 프레임마다 같은 시점을 그린다
local BenchCamera = {
	Properties = {},
}

function BenchCamera:OnStart()
	self.Time = 0.0
end

function BenchCamera:OnUpdate(dt)
	self.Time = self.Time + dt
	local A = 2.0 * math.pi * self.Time / 20.0
	local From = Vector3(1150.0 * math.cos(A), 1150.0 * math.sin(A), 420.0 + 70.0 * math.sin(2.0 * A))
	local At = Vector3(-1500.0 * math.cos(A + 0.5), -1500.0 * math.sin(A + 0.5), 380.0)
	self.entity:SetPosition(From)
	self.entity:SetRotation(Quat.LookRotation(At - From))
end

return BenchCamera
