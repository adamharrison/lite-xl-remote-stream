-- mod-version:3 --lite-xl 3.0


local core = require "core"
local config = require "core.config"
local style = require "core.style"
local common = require "core.common"
local libremote = require "plugins.remote.libremote"

local DEFAULT_PORT = 8086

if config.plugins.remote.server then
  print("Remote server listening on " .. (config.plugins.remote.address or "localhost") .. ":" .. (config.plugins.remote.port or DEFAULT_PORT) .. ".")
  local server = libremote.server(config.plugins.remote.address, config.plugins.remote.port or DEFAULT_PORT)

  local delayed_registered_fonts = {}

  local function register_font(font, options)
    if server:is_open() then
      server:register_font(font:get_path(), io.open(font:get_path(), "rb"):read("*all"), font, font:get_size(), options and common.serialize(options) or nil)
    else
      table.insert(delayed_registered_fonts, { font, options })
    end
    return font
  end
    
  local old_renderer_font_load = renderer.font.load
  function renderer.font.load(path, size, options)
    return register_font(old_renderer_font_load(path, size, options), options and common.serialize(options) or nil)
  end

  local old_renderer_font_copy = renderer.font.copy
  function renderer.font:copy(size, options)
    return register_font(old_renderer_font_copy(self, size, options), options and common.serialize(options) or nil)
  end

  
  style.font = register_font(style.font)
  style.big_font = register_font(style.big_font)
  style.icon_font = register_font(style.icon_font, { antialiasing="grayscale", hinting="full" })
  style.icon_big_font = register_font(style.icon_big_font)
  style.code_font = register_font(style.code_font)

  function renderer.draw_rect(...) return server:draw_rect(...) end
  function renderer.draw_text(font, text, x, ...) server:draw_text(font, text, x, ...) return x + font:get_width(text) end
  function renderer.begin_frame(...) return server:begin_frame(...) end
  function renderer.end_frame(...) return server:end_frame(...) end
  function renderer.set_clip_rect(...) return server:set_clip_rect(...) end

  local old_poll_event = system.poll_event
  system.poll_event = function(...) 
    local result = { old_poll_event(...) }
    if #result > 0 then
      if result[1] == "quit" then return table.unpack(result) end
    end
    result = { server:poll_event(...) }
    return table.unpack(result)
  end

  function system.wait_event(timeout)
    return server:wait_event(timeout)
  end
  
  local status, err = pcall(function()
    local client = server:accept()
    print("Accepted client.")
    for i,v in ipairs(delayed_registered_fonts) do register_font(table.unpack(v)) end
    core.redraw = true
    core.add_thread(function()
      while server:is_open() do
        coroutine.yield(0.01)
      end
      command.perform("core:restart")
    end)
  end)
  if not status then  
    io.stderr:write(err, "\n")
    command.perform("core:restart")
  end
else
  local total_fonts = 0
  local function font_load(path, contents, idx, size, options)
    local path = "/tmp/font-" .. idx
    io.open(path, "wb"):write(contents):close()
    if options then
      options = load("return " .. options)()
    end
    return renderer.font.load(path, size, options)
  end
  
  local address, port
  for i = 1, #ARGS do
    address, port = ARGS[i]:match("remote://([^:]+):?(%d*)")
    if address then table.remove(ARGS, i) break end
  end
  if address then
    local status, client = pcall(libremote.client, address, port and port ~= "" and port or DEFAULT_PORT)
    if not status then io.stderr:write(client, "\n") os.exit(-1) end
    local old_poll_event = system.poll_event
    
    function core.step()
      -- handle events
      local did_keymap = false
      local width, height = core.window:get_size()
      -- draw
      if not client:is_open() then print("Disconnected.") core.quit(true) end
      renderer.begin_frame(core.window)
      for type, a,b,c,d in system.poll_event do
        if type == "quit" then core.quit(true) end
        local _, res = core.try(core.on_event, type, a, b, c, d)
        did_keymap = res or did_keymap
      end
      renderer.end_frame()
      return true
    end

    local old_wait_event = system.wait_event
    function system.wait_event(timeout)
      local time = system.get_time()
      while time - system.get_time() < (timeout or 0) do
        if client:has_event() then return true end
        local had_event = old_wait_event(math.min(timeout, 1.0 / config.fps))
        if had_event then return true end
      end
      return false
    end

    
    function system.poll_event(...)
      local result = { old_poll_event(...) }
      if #result > 0 then 
        if result[1] == "quit" then 
          return table.unpack(result)
        else
          client:send_event(table.unpack(result)) 
        end
      end
      return client:process_event(renderer.set_clip_rect, renderer.draw_rect, renderer.draw_text, font_load)
    end
    print("Connected!")
  end
end
