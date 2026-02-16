#!/usr/bin/lua
-- myco_bridge.lua: Ubus interface for MycoFlow (fallback mode)
-- Reads /tmp/myco_state.json and serves 'myco' ubus object

require "ubus"
require "uloop"
require "nixio"
local json = require "luci.jsonc"

-- Connect to ubus
local conn = ubus.connect()
if not conn then
    error("Failed to connect to ubus")
end

uloop.init()

-- Helper to read state
local function read_state()
    local f = io.open("/tmp/myco_state.json", "r")
    if not f then return nil end
    local content = f:read("*all")
    f:close()
    return json.parse(content)
end

-- Methods
local methods = {
    myco = {
        status = {
            function(req, msg)
                local state = read_state()
                if not state then
                    conn:reply(req, { error = "state_file_missing" })
                    return
                end
                conn:reply(req, state)
            end, {}
        },
        policy_set = {
            function(req, msg)
                conn:reply(req, { status = "not_implemented_via_bridge" })
            end, { bandwidth_kbit = ubus.INT32 }
        },
        persona_list = {
            function(req, msg)
                conn:reply(req, { personas = {"interactive", "bulk", "video", "unknown"} })
            end, {}
        },
        persona_add = {
             function(req, msg)
                 conn:reply(req, { status = "ok" }) -- Stub
             end, { persona = ubus.STRING }
        },
        persona_delete = {
             function(req, msg)
                 conn:reply(req, { status = "ok" }) -- Stub
             end, {}
        }
    }
}

conn:add(methods)

-- Main loop
uloop.run()
