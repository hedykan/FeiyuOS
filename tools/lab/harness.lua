-- agent_lab's program runner: runs one FeiyuOS Lua program the way the
-- device does (lua_vm.cpp), with every vat.* call, print and read handed to
-- tools/agent_lab.py over this process's stdin / stdout:
--
--   lua5.4 tools/lab/harness.lua <program.lua> <name>
--   first line on stdin: {"input": {...}, "foreground": true, "limits": {...}}
--   each message out is "\30" .. JSON on one line; each answer in is JSON on one line.
--
-- Same libraries as the device (base, string, table, math, utf8, coroutine,
-- io with write / read / stderr only), no load / dofile / os / require; the
-- instruction and memory limits of the device are enforced.

local json = {}

do
  local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
                    ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
  local function encode_string(s)
    return '"' .. s:gsub('[%c"\\]', function(c)
      return escapes[c] or string.format("\\u%04x", c:byte())
    end) .. '"'
  end
  local function is_array(t)
    local n = 0
    for k in pairs(t) do
      if type(k) ~= "number" or k <= 0 or k % 1 ~= 0 then return false end
      n = math.max(n, k)
    end
    return n == #t
  end
  function json.encode(v, depth)
    depth = depth or 0
    if depth > 16 then error("表嵌套太深") end
    local t = type(v)
    if v == nil then return "null"
    elseif t == "boolean" then return tostring(v)
    elseif t == "number" then
      if v ~= v or v == math.huge or v == -math.huge then return "null" end
      if math.type(v) == "integer" then return tostring(v) end
      return string.format("%.14g", v)
    elseif t == "string" then return encode_string(v)
    elseif t == "table" then
      local out = {}
      if next(v) == nil then return "{}" end
      if is_array(v) then
        for i = 1, #v do out[#out + 1] = json.encode(v[i], depth + 1) end
        return "[" .. table.concat(out, ",") .. "]"
      end
      for k, val in pairs(v) do out[#out + 1] = encode_string(tostring(k)) .. ":" .. json.encode(val, depth + 1) end
      return "{" .. table.concat(out, ",") .. "}"
    end
    return encode_string(tostring(v))
  end

  local function decode_error(s, i, what) error(string.format("JSON 第 %d 字符：%s", i, what), 0) end
  local decode_value
  local function skip(s, i) return s:find("[^ \t\r\n]", i) or #s + 1 end
  local function decode_string(s, i)
    local out, j = {}, i + 1
    while true do
      local c = s:sub(j, j)
      if c == "" then decode_error(s, j, "字符串没有结束") end
      if c == '"' then return table.concat(out), j + 1 end
      if c == "\\" then
        local e = s:sub(j + 1, j + 1)
        local map = { b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
        if map[e] then out[#out + 1] = map[e]; j = j + 2
        elseif e == "u" then
          local code = tonumber(s:sub(j + 2, j + 5), 16) or decode_error(s, j, "\\u 不对")
          j = j + 6
          if code >= 0xD800 and code <= 0xDBFF and s:sub(j, j + 1) == "\\u" then
            local low = tonumber(s:sub(j + 2, j + 5), 16)
            if low then code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00); j = j + 6 end
          end
          out[#out + 1] = utf8.char(code)
        else decode_error(s, j, "转义不对") end
      else
        out[#out + 1] = c; j = j + 1
      end
    end
  end
  function decode_value(s, i)
    i = skip(s, i)
    local c = s:sub(i, i)
    if c == "{" then
      local t = {}
      i = skip(s, i + 1)
      if s:sub(i, i) == "}" then return t, i + 1 end
      while true do
        local k; k, i = decode_string(s, skip(s, i))
        i = skip(s, i)
        if s:sub(i, i) ~= ":" then decode_error(s, i, "缺少冒号") end
        t[k], i = decode_value(s, i + 1)
        i = skip(s, i)
        local d = s:sub(i, i)
        if d == "}" then return t, i + 1 end
        if d ~= "," then decode_error(s, i, "缺少逗号") end
        i = i + 1
      end
    elseif c == "[" then
      local t, n = {}, 0
      i = skip(s, i + 1)
      if s:sub(i, i) == "]" then return t, i + 1 end
      while true do
        n = n + 1
        t[n], i = decode_value(s, i)
        i = skip(s, i)
        local d = s:sub(i, i)
        if d == "]" then return t, i + 1 end
        if d ~= "," then decode_error(s, i, "缺少逗号") end
        i = i + 1
      end
    elseif c == '"' then return decode_string(s, i)
    elseif s:sub(i, i + 3) == "true" then return true, i + 4
    elseif s:sub(i, i + 4) == "false" then return false, i + 5
    elseif s:sub(i, i + 3) == "null" then return nil, i + 4
    else
      local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
      if not num or num == "" then decode_error(s, i, "不认识的值") end
      return math.tointeger(tonumber(num)) or tonumber(num), i + #num
    end
  end
  function json.decode(s)
    local ok, v = pcall(decode_value, s, 1)
    if not ok then return nil, v end
    return v
  end
end

local stdout, stdin = io.stdout, io.stdin
local function send(message)
  stdout:write("\30", json.encode(message), "\n")
  stdout:flush()
end
local function receive()
  local line = stdin:read("l")
  if not line then os.exit(3) end
  return json.decode(line)
end

local program_path, name = arg[1], arg[2] or "script"
local setup = receive() or {}
local limits = setup.limits or {}
local foreground = setup.foreground ~= false

-- ---------------------------------------------------------------- limits
local max_instructions = limits.instructions or (foreground and 200000000 or 20000000)
local max_kb = limits.memory_kb or 48
local DEVICE_BASE_KB = 10.6  -- an empty VM on the device (lua_vm: peak of an empty script)
local base_kb = 0            -- this harness's own memory, set just before the program runs
local peak_kb = DEVICE_BASE_KB
local thousands = 0
local function used_kb() return collectgarbage("count") - base_kb + DEVICE_BASE_KB end
debug.sethook(function()
  thousands = thousands + 1
  if thousands * 1000 > max_instructions then error("执行步数超过上限", 2) end
  local now = used_kb()
  if now > peak_kb then peak_kb = now end
  if now > max_kb then
    collectgarbage("collect")
    if used_kb() > max_kb then error("not enough memory", 2) end
  end
end, "", 1000)

-- ---------------------------------------------------------------- the program's world
-- The line of the program that made the current call (for warnings).
local function program_line()
  for level = 3, 12 do
    local info = debug.getinfo(level, "Sl")
    if not info then break end
    if info.source == "=" .. name then return info.currentline end
  end
  return nil
end

local function host_call(tool, args)
  send({ t = "call", tool = tool, args = args })
  local reply = receive() or {}
  if reply.abort then error(reply.abort, 0) end
  if reply.ok then return reply.value end
  if setup.warn and tool:sub(1, 1) ~= "_" then  -- E3: a failed call shows even if the program ignores it
    local line = program_line()
    send({ t = "err", s = string.format("[警告] %svat.%s(%s) 失败：%s\n", line and ("第 " .. line .. " 行 ") or "",
                                       tool, tostring(args.target or ""), tostring(reply.error)) })
  end
  return nil, reply.error
end

local function options(args, opts, skip_keys)
  if opts == nil then return args end
  if type(opts) ~= "table" then error("选项要是表", 3) end
  for k, v in pairs(opts) do
    if not (skip_keys and skip_keys[k]) and k ~= "target" and k ~= "confirm_required" then args[k] = v end
  end
  return args
end

local function out(text, err)
  send({ t = err and "err" or "out", s = text })
end

local function print_(...)
  local parts = {}
  for i = 1, select("#", ...) do
    local v = select(i, ...)
    if setup.print_json and type(v) == "table" then  -- E3: a table shows what it holds
      local ok, text = pcall(json.encode, v)
      parts[#parts + 1] = ok and text or tostring(v)
    else
      parts[#parts + 1] = tostring(v)
    end
  end
  out(table.concat(parts, "\t") .. "\n")
end

local function joined(from, ...)
  local parts = {}
  for i = from, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
  return table.concat(parts)
end

local function read_line(prompt, format)
  local line, why = host_call("_read", { prompt = prompt or "", foreground = foreground })
  if line == nil then return nil, why end
  format = format and format:gsub("^%*", "") or "l"
  if format:sub(1, 1) == "n" then return tonumber(line) end
  if format:sub(1, 1) == "L" then return line .. "\n" end
  return line
end

local vat = {}
function vat.read(target, opts) return host_call("read", options({ target = target }, opts)) end
function vat.write(target, content, opts)
  if type(content) == "table" then content = json.encode(content) end
  return host_call("write", options({ target = target, content = content or "", mode = "overwrite" }, opts))
end
function vat.exec(target, program, args, opts)
  return host_call("exec", options({ target = target, program = program or "run", args = args or {} }, opts))
end
function vat.observe(target, opts) return host_call("observe", options({ target = target }, opts)) end
function vat.input(target, text, opts)
  return host_call("input", options({ target = target, text = text or "", submit = false }, opts))
end
function vat.session(action, opts) return host_call("session", options({ action = action }, opts)) end
function vat.stream(target) return host_call("_stream", { target = target }) end
function vat.after(delay, run)
  if type(run) ~= "string" or run:sub(1, 7) ~= "prog://" then error("vat.after 只能预约运行程序（prog://名字）", 2) end
  return host_call("write", { target = "trigger://new", content = json.encode({ when = "time://in/" .. delay, run = run }),
                              mode = "create" })
end
function vat.json(text)
  local v, err = json.decode(text)
  if err then return nil, err end
  return v
end
function vat.tojson(v) return json.encode(v) end
function vat.urlencode(s)
  return (tostring(s):gsub("[^%w%-_%.~]", function(c) return string.format("%%%02X", c:byte()) end))
end
function vat.sleep(ms)
  if type(ms) ~= "number" or ms < 0 or ms > 60000 then error("sleep 只能 0 到 60000 毫秒；更久用 vat.after", 2) end
  host_call("_sleep", { ms = ms })
end
function vat.millis() return host_call("_millis", {}) end
vat.log = print_
if setup.log_err then
  local started = os.clock()
  function vat.log(...)
    local parts = {}
    for i = 1, select("#", ...) do
      local v = select(i, ...)
      local ok, text = pcall(json.encode, v)
      parts[#parts + 1] = (type(v) == "table" and ok) and text or tostring(v)
    end
    out(string.format("[+%.2fs] ", os.clock() - started) .. table.concat(parts, "\t") .. "\n", true)
  end
end
function vat.readline(prompt) return read_line(prompt, "l") end
function vat.key(timeout) return host_call("_key", { timeout = timeout or 0, foreground = foreground }) end
function vat.clear() send({ t = "clear" }) end
function vat.interactive() return foreground end

local io_ = {
  write = function(...) out(joined(1, ...)) end,
  read = function(format) return read_line("", type(format) == "string" and format or "l") end,
  stderr = { write = function(_, ...) out(joined(1, ...), true) end },
}

local env = {
  assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs, pcall = pcall, select = select,
  tonumber = tonumber, tostring = tostring, type = type, xpcall = xpcall, setmetatable = setmetatable,
  getmetatable = getmetatable, rawequal = rawequal, rawget = rawget, rawlen = rawlen, rawset = rawset,
  collectgarbage = collectgarbage, string = string, table = table, math = math, utf8 = utf8, coroutine = coroutine,
  print = print_, io = io_, vat = vat, input = setup.input or {}, _VERSION = _VERSION,
}
env._G = env
if setup.strict then  -- reading an undefined global is an error (a typo, usually)
  setmetatable(env, { __index = function(_, k) error("未定义的全局变量 " .. tostring(k), 2) end })
end

-- ---------------------------------------------------------------- run
local file = assert(io.open(program_path, "rb"))
local source = file:read("a")
file:close()
collectgarbage("collect")
base_kb = collectgarbage("count")
local chunk, err = load(source, "=" .. name, "t", env)
if not chunk then
  send({ t = "done", ok = false, syntax = true, error = "语法错误：" .. err, instructions = thousands * 1000,
         memory_kb = DEVICE_BASE_KB })
  os.exit(0)
end
local ok, result = xpcall(chunk, function(e) return debug.traceback(tostring(e), 2) end)
local result_text
if ok and result ~= nil then
  result_text = type(result) == "table" and json.encode(result) or tostring(result)
end
peak_kb = math.max(peak_kb, used_kb())
send({ t = "done", ok = ok, result = result_text, error = (not ok) and result or nil,
       instructions = thousands * 1000, memory_kb = peak_kb })
