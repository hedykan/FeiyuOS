-- @about 高德天气写进剪贴板
local w = vat.read("place://weather")
vat.write("device://local/clipboard", "天气 " .. tostring(w))
return vat.read("device://local/clipboard")
