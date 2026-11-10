-- @about 回显参数（x 必填，y 有默认值）
-- @input x
-- @input y 默认
vat.write("device://local/clipboard", "参数 " .. input.x .. " " .. input.y)
return vat.read("device://local/clipboard")
