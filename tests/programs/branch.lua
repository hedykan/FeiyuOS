-- @about 读回灯色并分支
vat.write("device://local/led", "绿")
local c = vat.read("device://local/led") or ""
if c:find("绿") then
  vat.write("device://local/led", "蓝")
else
  vat.write("device://local/led", "红")
end
return vat.read("device://local/led")
