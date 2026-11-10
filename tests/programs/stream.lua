-- @about 取 IMU 加速度与姿态角两个流的最新值
local a = vat.stream("sensor://imu/acceleration")
local o = vat.stream("sensor://imu/orientation")
return string.format("z=%s;pitch=%s;roll=%s;", a and a.z, o and o.pitch, o and o.roll)
