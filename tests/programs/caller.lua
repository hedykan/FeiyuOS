-- @about 调用测试-被调并用返回值
local r = vat.exec("prog://测试-被调?x=甲")
vat.write("device://local/clipboard", "返回：" .. tostring(r))
return vat.read("device://local/clipboard")
