-- @about 问大肥鱼再分支
local a = vat.write("ask://", "1 加 1 等于几？只回答阿拉伯数字")
if a and a:find("2") then
  vat.write("device://local/clipboard", "ask 正确")
else
  vat.write("device://local/clipboard", "ask 错误：" .. tostring(a))
end
return vat.read("device://local/clipboard")
