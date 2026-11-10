-- @about 含远端写操作，保存必须由用户确认
-- @uses exec ssh://开发机
return vat.exec("ssh://开发机", "touch", {"/tmp/vat-prog-test"})
