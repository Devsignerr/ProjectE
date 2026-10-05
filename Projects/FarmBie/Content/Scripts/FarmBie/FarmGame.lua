-- FarmBie 게임 관리자 (씬의 "FarmGame" 엔티티). 하루 진행·농사·경제·디펜스 모듈을 단계별로 여기에 붙인다 (Plans.md FarmBie).
--   다른 스크립트는 Scene.Find("FarmGame"):GetScript()로 쓴다. 자동 검증이면 Properties.AutoPlay = 시나리오 이름 (FarmAutoPilot.lua).
local FarmGame = {
	Properties = {
		AutoPlay = "",
	},
}

function FarmGame:OnStart()
	self.Report = {}
end

-- 자동 검증 결과 (FarmAutoPilot:Finish가 부른다) — 로그 "[FarmBie] 결과: 실패 N건"
function FarmGame:ReportAutoPlay(Failures, Summary)
	Log.Info("[FarmBie] 자동 플레이 요약: " .. (Summary or ""))
	if #Failures == 0 then
		Log.Info("[FarmBie] 결과: 실패 0건")
	else
		Log.Error("[FarmBie] 결과: 실패 " .. #Failures .. "건 — " .. table.concat(Failures, ", "))
	end
end

return FarmGame
