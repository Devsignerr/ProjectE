-- 경비병 다음 순찰 지점 (비헤이비어 트리 Lua 태스크): 경비병 스크립트의 Waypoints에서 다음 점을 받아 블랙보드 PatrolPoint에 넣는다
local GuardNextWaypoint = {
	Properties = {},
}

function GuardNextWaypoint:OnExecute()
	local Guard = self.entity:GetScript()
	if Guard == nil or Guard.NextWaypoint == nil then return "Failure" end
	local Point = Guard:NextWaypoint()
	if Point == nil then return "Failure" end
	self.entity:GetBlackboard():Set("PatrolPoint", Point)
	return "Success"
end

return GuardNextWaypoint
