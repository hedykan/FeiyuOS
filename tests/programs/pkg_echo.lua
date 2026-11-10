-- @about 回声（测试用扩展包）
-- @package 测试-回声
-- @target echo://
-- @usage 测试用：read echo://文字 原样返回
-- @keywords 回声 测试
function read(target, args)
  local text = target:sub(#"echo://" + 1)
  if text == "table" then return {n = 2, list = {1, 2}} end
  if text == "fail" then error("故意出错") end
  return "回声：" .. text
end
