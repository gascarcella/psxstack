-- The boot check, run inside PCSX-Redux (-dofile) by `<driver> boot` (tools/replay/emulator.py): waits until the
-- game's probes say it has booted (the probes chunk's booted(): the first screen's overlay loaded, for instance).
-- Prints "boot check: OK ..." and exits 0, or "boot check: FAIL ..." and exits 1 after PSXSTACK_BOOT_FRAMES vsyncs
-- (default 3000). The probes chunk and the accessors are run.lua's (PSXSTACK_REPLAY_PROBES, PSXSTACK_REPLAY).
local ffi = require('ffi')
local max_frames = tonumber(os.getenv('PSXSTACK_BOOT_FRAMES') or '') or 3000
local frames = 0
local mem = PCSX.getMemPtr()
local function ptr(addr) return mem + bit.band(addr, 0x1FFFFF) end
PSXSTACK_REPLAY = {
    ptr = ptr,
    u8 = function(addr) return ffi.cast('uint8_t*', ptr(addr))[0] end,
    u16 = function(addr) return ffi.cast('uint16_t*', ptr(addr))[0] end,
    u32 = function(addr) return tonumber(ffi.cast('uint32_t*', ptr(addr))[0]) end,
    s32 = function(addr) return ffi.cast('int32_t*', ptr(addr))[0] end,
    s16 = function(addr) return ffi.cast('int16_t*', ptr(addr))[0] end,
    s8 = function(addr) return ffi.cast('int8_t*', ptr(addr))[0] end,
}
local game = dofile(assert(os.getenv('PSXSTACK_REPLAY_PROBES'), 'PSXSTACK_REPLAY_PROBES not set'))
assert(game.booted, 'the probes chunk has no booted()')

local function state()
    return string.format('frame=%d pc=%08x stage=%d file=%d map=0x%X', frames, PCSX.getRegisters().pc,
        game.stage(), game.file(), game.map())
end

PSXSTACK_BOOT_LISTENER = PCSX.Events.createEventListener('GPU::Vsync', function()
    frames = frames + 1
    if game.booted() then
        print('boot check: OK (booted) ' .. state())
        PCSX.quit(0)
    elseif frames % 500 == 0 then
        print('boot check: ' .. state())
    end
    if frames >= max_frames then
        print('boot check: FAIL (not booted after ' .. max_frames .. ' frames) ' .. state())
        PCSX.quit(1)
    end
end)
print('boot check: waiting for the game to boot (up to ' .. max_frames .. ' frames)')
