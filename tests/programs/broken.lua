-- @about 必然失败
local r, err = vat.read("nothing://x")
if not r then error(err) end
return r
