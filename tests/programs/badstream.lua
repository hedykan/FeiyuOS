-- @about 读一个不是流的目标
local a, err = vat.stream("sensor://imu")
return a and "居然有值" or ("流错误：" .. tostring(err))
