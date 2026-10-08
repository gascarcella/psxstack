-- The replay runner, run inside PCSX-Redux (-dofile) by tools/replay/emulator.py: the step engine of a pad script
-- (the grammar runtime/script.c replays on the port, frame for frame) over a game's probes. The driver writes the
-- step table (a Lua chunk returning a table) and sets the environment:
--   PSXSTACK_REPLAY_SCRIPT   the step table
--   PSXSTACK_REPLAY_OUT      the output directory (result.json, one checkpoint image per checkpoint, vram_*.bin)
--   PSXSTACK_REPLAY_PROBES   the game's probes: a Lua chunk returning a table (below)
--   PSXSTACK_REPLAY_SLOT1_BASE  the first overlay slot's address (game.json memory.slots[0].base): wait_stage's word0
--   PSXSTACK_REPLAY_SPEED    PCSX-Redux's spu.Speed (0: unthrottled), PSXSTACK_REPLAY_VERBOSE
--
-- The probes chunk (GAME_CONTRACT.md "6. Tests") is loaded after the global PSXSTACK_REPLAY exists, a table of
-- memory accessors over the emulated RAM (ptr, u8, u16, u32, s8, s16, s32: PS1 addresses), so a game's chunk is
--     local R = PSXSTACK_REPLAY
--     return { image_addr = 0x80048D34, image_size = 0x275C,
--              stage = function() return R.s32(0x80055D28) end, file = function() ... end,
--              map = function() return R.u32(...) end, random_index = function() return R.s32(...) end,
--              player_pos = function() ... return x, y end,   -- optional: the walk step (pixels)
--              booted = function() ... end,                   -- optional: the boot check (boot_check.lua)
--              pad_held = function() ... end, pad_pressed = function() ... end }   -- optional: verbose state lines
-- The same probes the port's adapter implements (game_state_stage/file/map/random_index/player_pos/image).
--
-- Every GPU vsync (one game frame) this script:
--   1. records transitions of (stage, file) and of the map, with the frame number;
--   2. applies the pad buttons the current step holds (SIO0 slot 1 pad 1 override; a pressed button is a cleared
--      override bit, PCSX.CONSTS.PAD.BUTTON gives the bit numbers, the PS1 pad's own);
--   3. advances the step list: press (hold buttons for N frames, then release for `release` frames; optionally
--      `repeat`ed or repeated `until` a wait condition holds), wait_frames, wait_stage / wait_map / wait_mem (with a
--      timeout), walk (hold the d-pad toward a field position), reset (reboot the console), checkpoint (dump the
--      checkpoint image to a file, unless `image` is false, and log the frame, stage, map and random index), vram (append the whole VRAM to
--      vram_<name>.bin on each of its `frames` frames).
-- At the end it writes result.json and exits 0; a timeout or a Lua error writes what it has and exits 1.

local ffi = require('ffi')
io.stdout:setvbuf('line')  -- the driver reads the log after a crash or a timeout
local mem = PCSX.getMemPtr()
local function ptr(addr) return mem + bit.band(addr, 0x1FFFFF) end
local R = {
    ptr = ptr,
    u8 = function(addr) return ffi.cast('uint8_t*', ptr(addr))[0] end,
    u16 = function(addr) return ffi.cast('uint16_t*', ptr(addr))[0] end,
    u32 = function(addr) return tonumber(ffi.cast('uint32_t*', ptr(addr))[0]) end,
    s32 = function(addr) return ffi.cast('int32_t*', ptr(addr))[0] end,
    s16 = function(addr) return ffi.cast('int16_t*', ptr(addr))[0] end,
    s8 = function(addr) return ffi.cast('int8_t*', ptr(addr))[0] end,
}
PSXSTACK_REPLAY = R
local u8, u16, u32, s32, s16, s8 = R.u8, R.u16, R.u32, R.s32, R.s16, R.s8

local out_dir = assert(os.getenv('PSXSTACK_REPLAY_OUT'), 'PSXSTACK_REPLAY_OUT not set')
local script = dofile(assert(os.getenv('PSXSTACK_REPLAY_SCRIPT'), 'PSXSTACK_REPLAY_SCRIPT not set'))
local game = dofile(assert(os.getenv('PSXSTACK_REPLAY_PROBES'), 'PSXSTACK_REPLAY_PROBES not set'))
for _, k in ipairs({ 'image_addr', 'image_size', 'stage', 'file', 'map', 'random_index' }) do
    assert(game[k] ~= nil, 'the probes chunk lacks ' .. k)
end
local slot1_base = tonumber(os.getenv('PSXSTACK_REPLAY_SLOT1_BASE') or '')
local verbose = (os.getenv('PSXSTACK_REPLAY_VERBOSE') or '') ~= ''
local max_frames = script.max_frames or 20000
local default_timeout = script.default_timeout or 3000

-- Emulation speed (PCSX-Redux setting spu.Speed, "applied at the audio sink (the master clock)": 1 = real time,
-- 0 or less = as fast as the host allows). It paces the host only; the emulated machine is the same either way.
local speed = tonumber(os.getenv('PSXSTACK_REPLAY_SPEED') or '')
if speed then PCSX.settings.spu.Speed = speed end
print('replay: spu.Speed = ' .. tostring(PCSX.settings.spu.Speed))

local pad = PCSX.SIO0.slots[1].pads[1]
-- Buttons are the controller's physical buttons (PCSX.CONSTS.PAD.BUTTON, the PS1 pad's bit numbers: 0 SELECT, 3 START,
-- 4 UP, 5 RIGHT, 6 DOWN, 7 LEFT, 8 L2, 9 R2, 10 L1, 11 R1, 12 TRIANGLE, 13 CIRCLE, 14 CROSS, 15 SQUARE). A game may
-- remap them itself (the first game rotates the face buttons for non-Japanese languages): a script names the
-- physical button.
local BUTTON = PCSX.CONSTS.PAD.BUTTON

local STACK_RESET_FRAMES = 8192 -- see the end of the vsync listener
local frame = 0
local step_index = 1
local step_started = nil      -- frame the current step started
local step_phase = nil        -- for press: 'hold' or 'release'
local held = {}               -- button name -> true, applied each frame
local result = {
    script = script.name or '?',
    frames = 0,
    status = 'running',
    checkpoints = {},
    overlay_sequence = {},    -- { frame, stage, file } at every change of (stage, file)
    map_sequence = {},        -- { frame, map } at every change of the map
    inputs = {},              -- { frame, buttons } at every change of the held set (the per-frame pad trace)
}
local last_stage, last_file, last_map = nil, nil, nil
local last_input_key = ''

-- Minimal JSON writer for the result (numbers, strings, booleans, arrays, string-keyed tables).
local function is_array(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n == #t
end
local function json(v, indent)
    indent = indent or ''
    local t = type(v)
    if t == 'number' then
        if v ~= v or v == math.huge or v == -math.huge then return 'null' end
        if math.floor(v) == v then return string.format('%d', v) end
        return string.format('%.17g', v)
    elseif t == 'string' then
        return '"' .. v:gsub('[%c"\\]', function(c) return string.format('\\u%04x', c:byte()) end) .. '"'
    elseif t == 'boolean' then
        return tostring(v)
    elseif t == 'nil' then
        return 'null'
    elseif t == 'table' then
        local inner = indent .. '  '
        local parts = {}
        if is_array(v) then
            if #v == 0 then return '[]' end
            for _, x in ipairs(v) do parts[#parts + 1] = inner .. json(x, inner) end
            return '[\n' .. table.concat(parts, ',\n') .. '\n' .. indent .. ']'
        end
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = tostring(k) end
        table.sort(keys)
        for _, k in ipairs(keys) do parts[#parts + 1] = inner .. json(k) .. ': ' .. json(v[k], inner) end
        return '{\n' .. table.concat(parts, ',\n') .. '\n' .. indent .. '}'
    end
    error('cannot serialise a ' .. t)
end

local function write_result(status, message)
    result.status = status
    result.frames = frame
    if message then result.message = message end
    local f = assert(io.open(out_dir .. '/result.json', 'wb'))
    f:write(json(result), '\n')
    f:close()
end

local function state_string()
    local s = string.format('frame=%d stage=%d file=%d map=0x%X rnd=%d', frame, game.stage(), game.file(),
        game.map(), game.random_index())
    if game.pad_held then s = s .. string.format(' held=0x%04X', game.pad_held()) end
    return s
end

local function finish(status, message)
    print(string.format('replay: %s %s %s', status, message or '', state_string()))
    write_result(status, message)
    PCSX.quit(status == 'ok' and 0 or 1)
end

local function fail(message) finish('fail', message) end

-- Reads a memory operand: { addr = 0x8004xxxx, size = 1|2|4, signed = bool }.
local function read_mem(op)
    local size, signed = op.size or 4, op.signed
    if size == 1 then return signed and s8(op.addr) or u8(op.addr) end
    if size == 2 then return signed and s16(op.addr) or u16(op.addr) end
    return signed and s32(op.addr) or u32(op.addr)
end

local function apply_pad()
    for name, bitnum in pairs(BUTTON) do
        if held[name] then pad.setOverride(bitnum) else pad.clearOverride(bitnum) end
    end
    local names = {}
    for name in pairs(held) do names[#names + 1] = name end
    table.sort(names)
    local key = table.concat(names, '+')
    if key ~= last_input_key then
        result.inputs[#result.inputs + 1] = { frame = frame, buttons = names }
        last_input_key = key
    end
end

local function checkpoint(step)
    local n = #result.checkpoints + 1
    -- `image = false` on the step: no dump, no hash (the record says image = false)
    local file = nil
    if step.image ~= false then
        file = string.format('cp%02d_%s.bin', n, step.name or 'unnamed')
        local f = assert(io.open(out_dir .. '/' .. file, 'wb'))
        f:write(ffi.string(ptr(game.image_addr), game.image_size))
        f:close()
    end
    result.checkpoints[n] = {
        name = step.name or 'unnamed',
        frame = frame,
        stage = game.stage(),
        map = game.map(),
        random_index = game.random_index(),
        gamestate_file = file,
        image = step.image ~= false,
    }
    print('replay: checkpoint ' .. (step.name or '?') .. ' ' .. state_string())
end

-- The condition of a wait step (also a press step's `until`).
local function condition_met(c)
    if c.type == 'wait_stage' then
        -- the overlay is loaded when the stage says so and the slot holds its first word
        if c.word0 ~= nil then assert(slot1_base, 'wait_stage with word0 needs PSXSTACK_REPLAY_SLOT1_BASE') end
        return game.stage() == c.stage and (c.word0 == nil or u32(slot1_base) == c.word0)
    elseif c.type == 'wait_map' then
        return game.map() == c.map
    elseif c.type == 'wait_mem' then
        return read_mem(c) == c.value
    end
    error('unknown condition type ' .. tostring(c.type))
end

-- Returns true when the step is complete (the next step may start on the same frame for instant steps).
local function run_step(step)
    if step_started == nil then
        step_started = frame
        step_phase = nil
        if verbose then print('replay: step ' .. step_index .. ' ' .. (step.type or '?') .. ' ' .. state_string()) end
    end
    local t = step.type
    if t == 'checkpoint' then
        checkpoint(step)
        return true, true
    elseif t == 'reset' then
        -- Reboot the console (PCSX-Redux hardResetEmulator: RAM cleared, the memory cards stay): back to the BIOS and
        -- the title, e.g. to load a save made earlier in the same script. The frame count goes on.
        held = {}
        PCSX.hardResetEmulator()
        print('replay: reset ' .. state_string())
        return true, false
    elseif t == 'press' then
        -- One press is `frames` held + `release` released. Optional: `repeat` N presses in a row; `until` a wait
        -- condition ({type = 'wait_stage' | 'wait_map' | 'wait_mem', ...}) checked every frame, which releases the
        -- buttons and ends the step on the frame it holds (a mash through dialogue of unknown length, or a tap until
        -- the game reacts; fails after `timeout` frames). With `until`, `repeat` is a cap, not a count.
        local hold = step.frames or 2
        local release = step.release or 2
        local cycle = hold + release
        local elapsed = frame - step_started
        local phase = elapsed % cycle
        local count = math.floor(elapsed / cycle)
        if step['until'] then
            if condition_met(step['until']) then held = {}; return true, true end
            if phase == 0 and elapsed >= (step.timeout or default_timeout) then
                fail(string.format('step %d (press until %s) timed out after %d frames', step_index,
                    step['until'].type, elapsed))
            end
        end
        if phase < hold then
            if verbose and phase == hold - 1 then print('replay: pressing ' .. state_string()) end
            held = {}
            for _, b in ipairs(step.buttons) do
                assert(BUTTON[b], 'unknown button ' .. tostring(b))
                held[b] = true
            end
            return false
        end
        held = {}
        if step['until'] and not step['repeat'] then return false end
        return phase == cycle - 1 and count >= (step['repeat'] or 1) - 1, false
    elseif t == 'walk' then
        -- Hold the d-pad toward (x, y) (field pixels; RIGHT = +x, DOWN = +y, both for a diagonal) until the player is
        -- within `tol` pixels on both axes (the probes' player_pos). Fails after `timeout` frames.
        assert(game.player_pos, 'a walk step needs the probes\' player_pos')
        local x, y = game.player_pos()
        local tol = step.tol or 3
        held = {}
        if x and math.abs(step.x - x) <= tol and math.abs(step.y - y) <= tol then
            if verbose then print(string.format('replay: walked to (%.1f, %.1f) %s', x, y, state_string())) end
            return true, true
        end
        if frame - step_started >= (step.timeout or default_timeout) then
            fail(string.format('step %d (walk to %d,%d) timed out after %d frames at (%s, %s)', step_index, step.x,
                step.y, frame - step_started, tostring(x), tostring(y)))
        end
        if x then
            if step.x - x > tol then held.RIGHT = true elseif x - step.x > tol then held.LEFT = true end
            if step.y - y > tol then held.DOWN = true elseif y - step.y > tol then held.UP = true end
        end
        return false
    elseif t == 'wait_frames' then
        return frame - step_started >= step.frames - 1, false
    elseif t == 'vram' then
        -- as wait_frames, appending the whole VRAM (1024 x 512 pixels, 1 MB) to vram_<name>.bin on each of its frames
        local f = assert(io.open(out_dir .. '/vram_' .. (step.name or 'unnamed') .. '.bin',
            frame == step_started and 'wb' or 'ab'))
        f:write(tostring(PCSX.GPU.getVRAM()))
        f:close()
        return frame - step_started >= (step.frames or 1) - 1, false
    elseif t == 'wait_stage' or t == 'wait_map' or t == 'wait_mem' then
        if condition_met(step) then return true, true end
        if frame - step_started >= (step.timeout or default_timeout) then
            fail(string.format('step %d (%s) timed out after %d frames', step_index, t, frame - step_started))
        end
        return false
    end
    fail('unknown step type ' .. tostring(t))
end

-- The listener object must stay referenced: Redux removes a listener once Lua garbage-collects it (a local in
-- this chunk dies with the chunk, and the per-frame garbage triggers a collection a few hundred frames in).
PSXSTACK_REPLAY_LISTENER = PCSX.Events.createEventListener('GPU::Vsync', function()
    local ok, err = pcall(function()
        frame = frame + 1
        local stage, file, map = game.stage(), game.file(), game.map()
        if stage ~= last_stage or file ~= last_file then
            result.overlay_sequence[#result.overlay_sequence + 1] = { frame = frame, stage = stage, file = file }
            last_stage, last_file = stage, file
            if verbose then print('replay: overlay ' .. state_string()) end
        end
        if map ~= last_map then
            result.map_sequence[#result.map_sequence + 1] = { frame = frame, map = map }
            last_map = map
            if verbose then print('replay: map ' .. state_string()) end
        end
        if frame > max_frames then fail('max_frames reached') end
        -- Advance steps; instant steps (checkpoint, a wait already satisfied) chain within the frame.
        local budget = 100
        while step_index <= #script.steps do
            local done, instant = run_step(script.steps[step_index])
            if not done then break end
            step_index = step_index + 1
            step_started = nil
            budget = budget - 1
            if not instant or budget <= 0 then break end
        end
        apply_pad()
        if step_index > #script.steps then
            finish('ok', 'script complete')
        end
        if verbose and game.pad_pressed and game.pad_pressed() ~= 0 then
            print(string.format('replay: game saw pressed=0x%04X at frame %d', game.pad_pressed(), frame))
        end
        if verbose and frame % 100 == 0 then print('replay: ' .. state_string()) end
    end)
    if not ok then fail('lua error: ' .. tostring(err)) end
    -- PCSX-Redux (bf4c9ceb) leaks two Lua stack slots per event-listener call: after ~32,700 vsyncs the stack
    -- overflows (LuaJIT's 65,500 slots) inside this listener. An error raised out of a listener makes Redux print
    -- the stack and reset it, and the listener stays registered, so the runner raises one every STACK_RESET_FRAMES
    -- frames, after the frame's work is done. The emulated machine never sees it: records are unchanged.
    if frame % STACK_RESET_FRAMES == 0 then
        error(string.format('replay: Lua stack reset at frame %d (PCSX-Redux listener leak; not a failure)', frame))
    end
end)
print('replay: start ' .. (script.name or '?') .. ' (' .. #script.steps .. ' steps, max ' .. max_frames .. ' frames)')
