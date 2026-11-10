-- @about 控制台读写（测试）
local name = io.read()
local n = io.read("n")
print("你好 " .. tostring(name))
io.write("双倍 ", n * 2, "\n")
io.stderr:write("这行是错误输出\n")
print("交互 " .. tostring(vat.interactive()), "读完 " .. tostring(io.read() == nil))
