-- @about 等 2 秒再继续
vat.write("device://local/led", "蓝")
vat.sleep(2000)
vat.write("device://local/led", "off")
vat.write("device://local/clipboard", "等待完成")
return vat.read("device://local/clipboard")
