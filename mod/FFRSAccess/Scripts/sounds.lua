-- Raumklaenge: steuert den Mischer in ffrs_speech.dll (Pipe-Befehle "S...").
--
-- Jede Stimme hat eine ID. play() startet sie oder fuehrt sie nach (Lautstaerke, Balance, Tempo, Daempfung);
-- der Mischer laesst die Werte weich gleiten. Gesendet wird nur, was sich merklich geaendert hat.
-- Die Klaenge liegen in ..\Sounds und sind mit genny erzeugt (Sounds\klaenge.json, neu: genny render klaenge.json).

local M = {}

M.NAMES = { "wand", "ziel", "ausgang_ziel", "ausgang", "person", "tuer", "schalter", "truhe", "gegenstand", "objekt",
    "speicherpunkt" }

local ctx = nil
local voices = {}

local function send(line)
    if ctx then
        ctx.sendLine("S" .. line)
    end
end

function M.init(context)
    ctx = context
    voices = {}
    send("X")
    for _, name in ipairs(M.NAMES) do
        send("L" .. name .. "\t" .. ctx.soundsDir .. name .. ".wav")
    end
    ctx.log("Raumklaenge geladen: " .. ctx.soundsDir)
end

local function changed(old, gain, pan, rate, muffle)
    return math.abs(old.gain - gain) > 0.015 or math.abs(old.pan - pan) > 0.03 or math.abs(old.rate - rate) > 0.005
        or math.abs(old.muffle - muffle) > 0.05
end

function M.play(id, name, loop, gain, pan, rate, muffle)
    rate, muffle = rate or 1, muffle or 0
    local old = voices[id]
    if old and old.name == name and not changed(old, gain, pan, rate, muffle) then
        old.seen = true
        return
    end
    voices[id] = { name = name, gain = gain, pan = pan, rate = rate, muffle = muffle, seen = true }
    send(string.format("P%s\t%s\t%d\t%.3f\t%.3f\t%.4f\t%.2f", id, name, loop and 1 or 0, gain, pan, rate, muffle))
end

function M.stop(id)
    if voices[id] then
        voices[id] = nil
        send("S" .. id)
    end
end

-- Gesamtlautstaerke aller Raumklaenge, 1 = normal.
function M.setMaster(volume)
    send(string.format("M%.2f", volume))
end

function M.stopAll()
    voices = {}
    send("X")
end

-- Nach einem Durchlauf: Stimmen mit diesem Praefix, die nicht erneut gespielt wurden, ausblenden.
function M.beginFrame(prefix)
    for id, voice in pairs(voices) do
        if id:sub(1, #prefix) == prefix then
            voice.seen = false
        end
    end
end

function M.endFrame(prefix)
    for id, voice in pairs(voices) do
        if id:sub(1, #prefix) == prefix and not voice.seen then
            M.stop(id)
        end
    end
end

-- Richtung relativ zur Kamera: Balance -1 links bis 1 rechts, Daempfung fuer Klaenge hinter der Figur.
-- Unreal: X vorne, Y rechts, Gierwinkel im Uhrzeigersinn.
function M.spatial(listener, location)
    local dx, dy = location.X - listener.X, location.Y - listener.Y
    local distance = math.sqrt(dx * dx + dy * dy)
    if distance < 1 then
        return 0, 0, 0
    end
    local forward = (dx * listener.forwardX + dy * listener.forwardY) / distance
    local right = (dx * listener.rightX + dy * listener.rightY) / distance
    local muffle = forward < 0 and math.min(1, -forward) * 0.75 or 0
    return right * 0.9, muffle, distance
end

-- Lautstaerke nach Entfernung: voll bis near, dann weich bis far auf null.
function M.falloff(distance, near, far)
    if distance <= near then
        return 1
    end
    if distance >= far then
        return 0
    end
    local t = 1 - (distance - near) / (far - near)
    return t * t
end

return M
