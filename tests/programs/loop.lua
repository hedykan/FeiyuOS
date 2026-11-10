-- @about 循环 3 次切换灯色，数一共写了几次
local n = 0
for i = 1, 3 do
  vat.write("device://local/led", "红")
  vat.write("device://local/led", "绿")
  n = n + 2
end
vat.write("device://local/led", "off")
return "写了 " .. (n + 1) .. " 次"
