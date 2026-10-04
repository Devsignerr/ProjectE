-- Tests/Tilemap2D 확인 스크립트 (Phase 56 연결): 플립북 이벤트 로그, 2초에 받침 타일을 지워(entity:EraseTile) 위 상자를 떨어뜨리고,
-- 3.5초에 상자 높이·발밑 셀을 로그로 남긴다 (타일맵은 아직 그려지지 않으므로 로그로 판단)
local T = {
	Properties = {
		HoleMinX = -10,
		HoleMaxX = -8,
		HoleY = 4,
	},
}

function T:OnStart()
	self.Time = 0
	self.Sparkles = 0
	self.bErased = false
	self.bReported = false
end

function T:OnFlipbookEvent_Sparkle(frame)
	self.Sparkles = self.Sparkles + 1
	if self.Sparkles <= 3 then
		Log.Info(string.format("[Tilemap2D] 플립북 이벤트 Sparkle (프레임 %d, %d번째)", frame, self.Sparkles))
	end
end

function T:OnUpdate(dt)
	self.Time = self.Time + dt
	local Map = Scene.Find("Tilemap")
	if Map == nil then
		return
	end
	if not self.bErased and self.Time >= 2.0 then
		self.bErased = true
		local P = self.Properties
		for X = P.HoleMinX, P.HoleMaxX do
			Map:EraseTile(X, P.HoleY)
		end
		Log.Info(string.format("[Tilemap2D] 받침 타일 지움 (셀 %d~%d, %d) — 남은 셀 (%d,%d) = %s", P.HoleMinX, P.HoleMaxX, P.HoleY,
			P.HoleMaxX + 1, P.HoleY, tostring(Map:GetTile(P.HoleMaxX + 1, P.HoleY))))
	end
	if not self.bReported and self.Time >= 3.5 then
		self.bReported = true
		for _, Name in ipairs({ "BoxGround", "BoxOneWay", "BoxLedge" }) do
			local Box = Scene.Find(Name)
			if Box ~= nil then
				local Position = Box:GetWorldPosition()
				local X, Y = Map:WorldToCell(Position - Vector3(0, 0, 31))
				local Tags = Map:GetTileTags(X, Y)
				Log.Info(string.format("[Tilemap2D] %s 높이 Z = %.1f, 발밑 셀 (%d,%d) 태그 [%s]", Name, Position.Z, X, Y, table.concat(Tags, ",")))
			end
		end
		Log.Info(string.format("[Tilemap2D] 플립북 Sparkle 이벤트 %d번, 현재 프레임 %d", self.Sparkles, self.entity:GetFlipbookFrame()))
	end
end

return T
