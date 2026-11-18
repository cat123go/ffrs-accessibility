-- Audiodeskription fuer Zwischensequenzen.
--
-- Texte und Aufnahmen stammen aus "Visions of Grandshelt" von buu420 (GPL-3.0,
-- https://github.com/buu420/visions-of-grandshelt), liegen unveraendert in ..\Descriptions
-- und werden in der Textsprache des Spiels gewaehlt (Englisch im Hauptordner, sonst Unterordner).
--
-- Zwei Arten von Eintraegen:
--   cues.tsv   scene, id, start, end, audio, text: laufen nach der Uhr der Sequenz
--              (CPP_TalkEventManager -> m_pScene -> m_pEventPlayer -> m_pSequencePlayer:GetCurrentTime).
--   paced.tsv  scene, event, id, audio, text, finish_on_advance, caption_sha256: haengen an einer
--              Dialogzeile (event = "table:<Stringtabelle>|<Schluessel>"); deren Text holt die Mod in der
--              eingestellten Sprache ueber TextFromStringTable und vergleicht ihn mit dem Gespraechsfenster.
-- Waehrend eine Aufnahme laeuft, wartet die Ansage der Dialogzeile, bis sie zu Ende ist.

local M = {}

M.TICK_MS = 50
M.START_WINDOW_SECONDS = 0.6 -- spaeter erreichte Startpunkte gelten als verpasst
M.FIT_MARGIN_SECONDS = 0.05
M.BACK_JUMP_SECONDS = 0.5
M.STALL_SECONDS = 0.3 -- Uhr steht so lange still: Aufnahme anhalten (Spiel wartet auf Eingabe oder Pause)
M.CUT_AFTER_END_SECONDS = 0.25
M.SPEECH_AFTER_SECONDS = 0.2

local LANGUAGE_DIRS = {
    de = "de", fr = "fr", ja = "ja", ko = "ko", es = "es-ES",
    ["zh-hans"] = "zh-Hans", ["zh-hant"] = "zh-Hant",
    ["zh-cn"] = "zh-Hans", ["zh-sg"] = "zh-Hans", ["zh-tw"] = "zh-Hant", ["zh-hk"] = "zh-Hant", zh = "zh-Hans",
}

local ctx = nil
local tables = nil -- { language, cuesByScene, pacedByScene }
local session = nil -- { key, scene, lastTime, lastWall, lastAdvanceWall, done = {}, pacedDone = {} }
local active = nil -- { cue, paced, endWall, paused, pausedAt }
local deferredSpeech = nil
local clockMethodLogged = {}
local captionCache = {}
local missLogged = {}

local function log(message)
    ctx.log("Deskription: " .. message)
end

local function send(command)
    ctx.sendLine("A" .. command)
end

local function split(line)
    local fields = {}
    for field in (line .. "\t"):gmatch("([^\t]*)\t") do
        table.insert(fields, field)
    end
    return fields
end

local function readTsv(path)
    local file = io.open(path, "rb")
    if not file then
        return nil
    end
    local rows, header = {}, nil
    for rawLine in file:lines() do
        local line = rawLine:gsub("\r$", "")
        if line ~= "" then
            local fields = split(line)
            if not header then
                header = fields
            else
                local row = {}
                for index, name in ipairs(header) do
                    row[name] = fields[index]
                end
                table.insert(rows, row)
            end
        end
    end
    file:close()
    return rows
end

-- Dauer aus dem RIFF-Kopf, ohne die ganze Datei zu lesen.
local function wavDuration(path)
    local file = io.open(path, "rb")
    if not file then
        return nil
    end
    local head = file:read(12)
    if not head or head:sub(1, 4) ~= "RIFF" or head:sub(9, 12) ~= "WAVE" then
        file:close()
        return nil
    end
    local byteRate = nil
    while true do
        local chunk = file:read(8)
        if not chunk or #chunk < 8 then
            break
        end
        local id, size = chunk:sub(1, 4), string.unpack("<I4", chunk, 5)
        if id == "fmt " then
            local body = file:read(size + size % 2)
            byteRate = body and #body >= 12 and string.unpack("<I4", body, 9) or nil
        elseif id == "data" then
            file:close()
            return byteRate and byteRate > 0 and size / byteRate or nil
        else
            file:seek("cur", size + size % 2)
        end
    end
    file:close()
    return nil
end

local function languageDir()
    local ok, language = pcall(function()
        local library = StaticFindObject("/Script/Engine.Default__KismetInternationalizationLibrary")
        local value = library:GetCurrentLanguage()
        return type(value) == "string" and value or value:ToString()
    end)
    if not ok or type(language) ~= "string" or language == "" then
        return "de", "unbekannt (" .. tostring(language) .. ")"
    end
    local lower = language:lower()
    if LANGUAGE_DIRS[lower] then
        return LANGUAGE_DIRS[lower], language
    end
    -- de-DE, zh-Hans-CN usw.: laengsten passenden Anfang nehmen.
    local best = nil
    for prefix, dir in pairs(LANGUAGE_DIRS) do
        if lower:sub(1, #prefix + 1) == prefix .. "-" and (not best or #prefix > #best) then
            best = prefix
        end
    end
    return best and LANGUAGE_DIRS[best] or "", language
end

local function loadTables(dir)
    local base = ctx.descriptionsDir
    local folder = dir == "" and base or (base .. dir .. "\\")
    local cues = readTsv(folder .. "cues.tsv")
    local paced = readTsv(folder .. "paced.tsv")
    if not cues and dir ~= "" then
        log("keine Dateien fuer Sprache " .. dir .. ", nehme Englisch")
        return loadTables("")
    end
    local result = { language = dir, cuesByScene = {}, pacedByScene = {} }
    local function audioPath(relative)
        return folder .. relative:gsub("/", "\\")
    end
    for _, row in ipairs(cues or {}) do
        local cue = {
            scene = row.scene, id = row.id, start = tonumber(row.start), finish = tonumber(row["end"]),
            audio = audioPath(row.audio or ""), text = row.text,
        }
        if cue.scene and cue.start and cue.finish then
            result.cuesByScene[cue.scene] = result.cuesByScene[cue.scene] or {}
            table.insert(result.cuesByScene[cue.scene], cue)
        end
    end
    for _, list in pairs(result.cuesByScene) do
        table.sort(list, function(a, b)
            return a.start < b.start
        end)
    end
    local pacedCount = 0
    for _, row in ipairs(paced or {}) do
        -- Englisch nennt die Zeile direkt (dialogue:<Text>), die uebrigen Sprachen ihren Tabellenschluessel.
        local tablePath, key = (row.event or ""):match("^table:(.-)|(.+)$")
        local dialogue = (row.event or ""):match("^dialogue:(.+)$")
        if row.scene and (tablePath or dialogue) then
            local cue = {
                scene = row.scene, id = row.id, tablePath = tablePath, key = key, dialogue = dialogue,
                audio = audioPath(row.audio or ""),
                text = row.text, finishOnAdvance = row.finish_on_advance == "1",
            }
            result.pacedByScene[row.scene] = result.pacedByScene[row.scene] or {}
            table.insert(result.pacedByScene[row.scene], cue)
            pacedCount = pacedCount + 1
        end
    end
    log(string.format("geladen, Sprache %s, %d Zeitmarken, %d Dialogmarken", dir == "" and "en" or dir,
        #(cues or {}), pacedCount))
    return result
end

local function ensureTables()
    local dir, raw = languageDir()
    if not tables or tables.language ~= dir then
        log("Spielsprache " .. tostring(raw))
        tables = loadTables(dir)
        captionCache = {}
    end
end

-- Liefert { key, scene, time, playing, paused } der laufenden Sequenz oder nil.
local function readClock()
    for _, manager in ipairs(FindAllOf("CPP_TalkEventManager") or {}) do
        local ok, clock = pcall(function()
            if not manager:IsValid() then
                return nil
            end
            local scene = manager.m_pScene
            if not scene or not scene:IsValid() then
                return nil
            end
            local player = scene.m_pEventPlayer
            if not player or not player:IsValid() then
                return nil
            end
            local sequencePlayer = player.m_pSequencePlayer
            if not sequencePlayer or not sequencePlayer:IsValid() then
                return nil
            end
            local sequence = sequencePlayer:GetSequence()
            if not sequence or not sequence:IsValid() then
                return nil
            end
            local result = {
                key = ctx.objectPath(scene) .. "|" .. ctx.objectPath(sequencePlayer),
                scene = "sequence:" .. ctx.objectPath(sequence),
                playing = sequencePlayer:IsPlaying(),
                paused = sequencePlayer:IsPaused(),
            }
            local timeOk, seconds = pcall(function()
                local current = sequencePlayer:GetCurrentTime()
                local frame = current.Time.FrameNumber.Value + current.Time.SubFrame
                return frame * current.Rate.Denominator / current.Rate.Numerator
            end)
            if timeOk and type(seconds) == "number" and seconds == seconds then
                result.time = seconds
                result.method = "GetCurrentTime"
            else
                result.method = "Wanduhr (" .. tostring(seconds) .. ")"
            end
            return result
        end)
        if ok and clock then
            return clock
        end
    end
    return nil
end

local function stopActive(reason)
    if active then
        send("S")
        log("Ende " .. active.cue.id .. " (" .. reason .. ")")
        active = nil
    end
end

local function play(cue, paced)
    local duration = wavDuration(cue.audio)
    send("P" .. cue.audio)
    active = { cue = cue, paced = paced, endWall = os.clock() + (duration or 8), paused = false }
    log(string.format("Start %s (%.1f s): %s", cue.id, duration or -1, cue.text or ""))
end

function M.recordingActive()
    return active ~= nil
end

local function beginSession(clock)
    session = { key = clock.key, scene = clock.scene, done = {}, pacedDone = {}, lastWall = os.clock(),
        lastAdvanceWall = os.clock(), estimated = 0 }
    stopActive("neue Szene")
    ensureTables()
    local cues = tables.cuesByScene[clock.scene]
    local paced = tables.pacedByScene[clock.scene]
    if not clockMethodLogged[clock.method] then
        clockMethodLogged[clock.method] = true
        log("Szenenuhr ueber " .. clock.method)
    end
    log(string.format("Szene %s, %d Zeitmarken, %d Dialogmarken", clock.scene, cues and #cues or 0,
        paced and #paced or 0))
end

-- Szenenzeit: aus der Sequenz, sonst selbst gezaehlt, solange sie laeuft.
local function sceneTime(clock, now)
    if clock.time then
        return clock.time
    end
    if clock.playing and not clock.paused then
        session.estimated = session.estimated + (now - session.lastWall)
    end
    return session.estimated
end

local function tick()
    local now = os.clock()
    local clock = readClock()
    if not clock then
        if session then
            stopActive("Szene vorbei")
            session = nil
        end
    else
        if not session or session.key ~= clock.key or session.scene ~= clock.scene then
            beginSession(clock)
        end
        local time = sceneTime(clock, now)
        local last = session.lastTime
        if last and time < last - M.BACK_JUMP_SECONDS then
            -- Zurueckgesprungen (Wiederholung): spaetere Eintraege wieder scharf.
            for id in pairs(session.done) do
                session.done[id] = nil
            end
            if active and not active.paced and active.cue.start > time then
                stopActive("Zeitsprung zurueck")
            end
        end
        if last and time > last + 0.005 then
            session.lastAdvanceWall = now
        end
        local stalled = not clock.playing or clock.paused or (now - session.lastAdvanceWall) > M.STALL_SECONDS
        local cues = tables.cuesByScene[clock.scene]
        -- Zeitgebundene Aufnahme: bei stehender Uhr anhalten, am Ende ihres Fensters abschneiden.
        if active and not active.paced then
            if time > active.cue.finish + M.CUT_AFTER_END_SECONDS then
                stopActive("Fenster vorbei")
            elseif stalled and not active.paused then
                send("H")
                active.paused, active.pausedAt = true, now
            elseif not stalled and active.paused then
                send("R")
                active.endWall = active.endWall + (now - active.pausedAt)
                active.paused = false
            end
        end
        if cues and not stalled then
            for _, cue in ipairs(cues) do
                if cue.start > time then
                    break
                end
                if not session.done[cue.id] then
                    session.done[cue.id] = true
                    local late = time - cue.start
                    local duration = wavDuration(cue.audio)
                    local fits = not duration or duration <= (cue.finish - time) + M.FIT_MARGIN_SECONDS
                    if late <= M.START_WINDOW_SECONDS and fits and not (active and active.paced) then
                        play(cue, false)
                    else
                        log(string.format("verpasst %s (%.2f s spaet)", cue.id, late))
                    end
                end
            end
        end
        session.lastTime = time
        session.lastWall = now
    end
    if active and not active.paused and now >= active.endWall then
        active = nil
        -- Kurze Pause, damit die Zeile nicht direkt an die Aufnahme anschliesst.
        if deferredSpeech then
            deferredSpeech.at = now + M.SPEECH_AFTER_SECONDS
        end
    end
    if deferredSpeech and not active and now >= deferredSpeech.at then
        ctx.speak(deferredSpeech.text, true)
        deferredSpeech = nil
    end
end

local function normalize(text)
    text = text:gsub('<[^>]-id="([^"]*)"[^>]*>', ""):gsub("<[^>]*>", ""):gsub("%s+", "")
    return text
end

local function caption(cue)
    if cue.dialogue then
        return normalize(cue.dialogue)
    end
    local cacheKey = cue.tablePath .. "|" .. cue.key
    if captionCache[cacheKey] == nil then
        local ok, text = pcall(function()
            local library = StaticFindObject("/Script/Engine.Default__KismetTextLibrary")
            return library:TextFromStringTable(FName(cue.tablePath), cue.key):ToString()
        end)
        if ok and type(text) == "string" and text ~= "" and text ~= cue.key and not text:find("MISSING") then
            captionCache[cacheKey] = normalize(text)
        else
            -- Tabelle evtl. noch nicht geladen: beim naechsten Mal neu versuchen.
            return nil
        end
    end
    return captionCache[cacheKey]
end

-- Aufruf bei jeder neuen Zeile im Gespraechsfenster. true = die Mod spricht die Zeile spaeter selbst.
function M.talkLine(text)
    if active and active.paced and not active.cue.finishOnAdvance then
        stopActive("Zeile weiter")
    end
    deferredSpeech = nil
    if session and tables then
        local list = tables.pacedByScene[session.scene]
        if list then
            local shown = normalize(text)
            local matched = nil
            for _, cue in ipairs(list) do
                local expected = not session.pacedDone[cue.id] and caption(cue)
                if expected and expected ~= "" and shown:find(expected, 1, true) then
                    matched = cue
                    break
                end
            end
            if matched then
                session.pacedDone[matched.id] = true
                stopActive("Dialogmarke")
                play(matched, true)
            elseif not missLogged[shown] then
                missLogged[shown] = true
                log("Zeile ohne Dialogmarke: " .. text:sub(1, 120))
            end
        end
    end
    if active then
        deferredSpeech = { text = text, at = 0 }
        return true
    end
    return false
end

function M.init(context)
    ctx = context
    LoopInGameThreadWithDelay(M.TICK_MS, function()
        local ok, err = pcall(tick)
        if not ok and not M.errorLogged then
            M.errorLogged = true
            log("Fehler: " .. tostring(err))
        end
    end)
    log("bereit, Ordner " .. ctx.descriptionsDir)
end

return M
