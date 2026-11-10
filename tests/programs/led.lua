-- @about 亮红灯再读回颜色
vat.write("device://local/led", "红")
local c = vat.read("device://local/led")
vat.write("device://local/led", "off")
return c
