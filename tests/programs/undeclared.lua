-- @about 没声明就做需要确认的事
local r, err = vat.exec("ssh://开发机", "touch", {"/tmp/vat-prog-undeclared"})
return r and "居然执行了" or err
