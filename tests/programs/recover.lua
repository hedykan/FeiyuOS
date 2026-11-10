-- @about 调用失败后接着做
local r, err = vat.exec("prog://测试-坏")
vat.write("device://local/clipboard", "错误：" .. tostring(err))
return vat.read("device://local/clipboard")
