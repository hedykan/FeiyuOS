-- @about 查 GitHub 项目的最新版本号（最新的标签）
-- @package 发布
-- @target release://
-- @usage 查某个 GitHub 项目的最新版本用 read release://所有者/仓库（如 release://espressif/arduino-esp32）
-- @keywords GitHub 发布 版本 release tag 更新 最新
-- @uses read https://api.github.com/repos/*
-- @version 1.0
function read(target, args)
  local repo = target:sub(#"release://" + 1)
  if not repo:match("^[%w%._-]+/[%w%._-]+$") then error("要写成 release://所有者/仓库") end
  local body, err = vat.read("https://api.github.com/repos/" .. repo .. "/tags?per_page=1",
                             {headers = {Accept = "application/vnd.github+json"}})
  if not body then error(err) end
  local tags = vat.json(body)
  if not tags or not tags[1] then return repo .. " 还没有标签" end
  return {repo = repo, version = tags[1].name}
end
