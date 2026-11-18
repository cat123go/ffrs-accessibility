// Sprachbruecke zwischen UE4SS-Lua und Prism.
//
// UE4SS exportiert keine Lua-C-API, daher ist ein klassisches Lua-C-Modul nicht moeglich.
// Lua laedt diese DLL per package.loadlib und schreibt danach Zeilen in eine Named Pipe:
//   <Prioritaet><UTF-8-Text>\n     Prioritaet: '1' = unterbricht, '0' = reiht sich ein
//   T<Note>[,<Note>...]\n     spielt leise Toene nacheinander
//     Note: <Hertz>:<Millisekunden>[:<Balance -1 links bis 1 rechts>[:<Lautstaerke, 1 = normal>]]
//   AP<UTF-8-Pfad>\n          spielt eine WAV-Datei (PCM 16 Bit), beendet eine laufende
//   AS\n / AH\n / AR\n         Aufnahme stoppen / anhalten / fortsetzen
//   S<Befehl>\n               Raumklaenge (Mischer), siehe mixer::Command
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>

#include <prism.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const wchar_t kPipeName[] = L"\\\\.\\pipe\\ffrs_speech";
constexpr DWORD kPipeBufferSize = 64 * 1024;

decltype(&prism_config_init) p_config_init;
decltype(&prism_init) p_init;
decltype(&prism_registry_acquire_best) p_acquire_best;
decltype(&prism_backend_initialize) p_backend_initialize;
decltype(&prism_backend_output) p_backend_output;
decltype(&prism_error_string) p_error_string;

std::wstring g_dir;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
FILE *g_log = nullptr;

void Log(const char *format, const char *a = "", const char *b = "") {
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  fprintf(g_log, format, a, b);
  fputc('\n', g_log);
  fflush(g_log);
}

std::wstring OwnDirectory() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&OwnDirectory), &self);
  wchar_t path[MAX_PATH * 4];
  DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
  std::wstring result(path, length);
  return result.substr(0, result.find_last_of(L'\\'));
}

// Lua 5.4 laedt ohne LOAD_WITH_ALTERED_SEARCH_PATH, deshalb wuerde eine feste
// Abhaengigkeit auf prism.dll im Mod-Ordner nicht gefunden.
bool LoadPrism() {
  std::wstring path = g_dir + L"\\prism.dll";
  HMODULE prism = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!prism) {
    Log("prism.dll konnte nicht geladen werden");
    return false;
  }
  bool ok = true;
  auto resolve = [&](auto &target, const char *name) {
    target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(prism, name));
    if (!target) {
      Log("Prism-Funktion fehlt: %s", name);
      ok = false;
    }
  };
  resolve(p_config_init, "prism_config_init");
  resolve(p_init, "prism_init");
  resolve(p_acquire_best, "prism_registry_acquire_best");
  resolve(p_backend_initialize, "prism_backend_initialize");
  resolve(p_backend_output, "prism_backend_output");
  resolve(p_error_string, "prism_error_string");
  return ok;
}

PrismBackend *CreateBackend() {
  if (!LoadPrism()) return nullptr;
  PrismConfig config = p_config_init();
  PrismContext *context = p_init(&config);
  if (!context) {
    Log("prism_init fehlgeschlagen");
    return nullptr;
  }
  PrismBackend *backend = p_acquire_best(context);
  if (!backend) {
    Log("Kein Prism-Backend verfuegbar");
    return nullptr;
  }
  PrismError error = p_backend_initialize(backend);
  if (error != PRISM_OK && error != PRISM_ERROR_ALREADY_INITIALIZED) {
    Log("prism_backend_initialize: %s", p_error_string(error));
    return nullptr;
  }
  Log("Prism bereit");
  return backend;
}

constexpr int kSampleRate = 44100;
constexpr double kToneVolume = 0.16;
constexpr double kAttackSeconds = 0.006;
constexpr double kDecayRate = 3.5;
constexpr double kOvertone = 0.2;

// Sinus mit weichem Einsatz und Ausklingen, klingt wie eine leise Glocke statt wie ein Piepser.
void AppendNote(std::vector<int16_t> &samples, double hertz, double seconds, double balance, double volume) {
  const double pi = 3.14159265358979323846;
  const int count = static_cast<int>(seconds * kSampleRate);
  const double left = std::min(1.0, 1.0 - balance) * volume;
  const double right = std::min(1.0, 1.0 + balance) * volume;
  for (int i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const double attack = t < kAttackSeconds ? t / kAttackSeconds : 1.0;
    const double progress = static_cast<double>(i) / count;
    // Der letzte Abschnitt laeuft auf null aus, damit am Notenende kein Knacken entsteht.
    const double release = progress > 0.9 ? (1.0 - progress) / 0.1 : 1.0;
    const double envelope = attack * release * std::exp(-kDecayRate * progress);
    const double wave = std::sin(2 * pi * hertz * t) + kOvertone * std::sin(4 * pi * hertz * t);
    const double sample = wave * envelope * kToneVolume * 32767 / (1.0 + kOvertone);
    samples.push_back(static_cast<int16_t>(sample * left));
    samples.push_back(static_cast<int16_t>(sample * right));
  }
}

std::vector<char> BuildWave(const std::string &notes) {
  std::vector<int16_t> samples;
  size_t position = 0;
  while (position < notes.size()) {
    size_t end = notes.find(',', position);
    if (end == std::string::npos) end = notes.size();
    double hertz = 0, balance = 0, volume = 1;
    int milliseconds = 0;
    if (sscanf_s(notes.substr(position, end - position).c_str(), "%lf:%d:%lf:%lf", &hertz, &milliseconds, &balance,
                 &volume) >= 2 &&
        hertz > 0 && milliseconds > 0 && milliseconds <= 2000) {
      AppendNote(samples, hertz, milliseconds / 1000.0, std::clamp(balance, -1.0, 1.0), std::clamp(volume, 0.0, 2.0));
    }
    position = end + 1;
  }
  const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
  const uint32_t riff_size = 36 + data_size;
  const uint32_t format_size = 16;
  const uint16_t pcm = 1, channels = 2, bits = 16, block_align = 4;
  const uint32_t rate = kSampleRate, byte_rate = kSampleRate * 4;
  std::vector<char> wave;
  auto append = [&wave](const void *source, size_t size) {
    const char *bytes = static_cast<const char *>(source);
    wave.insert(wave.end(), bytes, bytes + size);
  };
  append("RIFF", 4);
  append(&riff_size, 4);
  append("WAVEfmt ", 8);
  append(&format_size, 4);
  append(&pcm, 2);
  append(&channels, 2);
  append(&rate, 4);
  append(&byte_rate, 4);
  append(&block_align, 2);
  append(&bits, 2);
  append("data", 4);
  append(&data_size, 4);
  append(samples.data(), data_size);
  return wave;
}

// Eigener Thread, damit Toene die Sprachausgabe nicht aufhalten.
DWORD WINAPI ToneThread(LPVOID parameter) {
  auto *wave = static_cast<std::vector<char> *>(parameter);
  PlaySoundW(reinterpret_cast<LPCWSTR>(wave->data()), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
  delete wave;
  return 0;
}

void PlayTones(const std::string &notes) {
  auto *wave = new std::vector<char>(BuildWave(notes));
  HANDLE thread = CreateThread(nullptr, 0, ToneThread, wave, 0, nullptr);
  if (thread) {
    CloseHandle(thread);
  } else {
    delete wave;
  }
}

// Audiodeskription: eine Aufnahme zur Zeit, ueber waveOut statt PlaySound, damit Toene sie nicht abbrechen
// und sie sich anhalten laesst. Alle Aufrufe kommen aus dem Pipe-Thread.
struct Recording {
  HWAVEOUT device = nullptr;
  WAVEHDR header{};
  std::vector<char> samples;
};
Recording g_recording;

void StopRecording() {
  if (!g_recording.device) return;
  waveOutReset(g_recording.device);
  waveOutUnprepareHeader(g_recording.device, &g_recording.header, sizeof(WAVEHDR));
  waveOutClose(g_recording.device);
  g_recording = Recording{};
}

void PlayRecording(const std::string &utf8_path) {
  StopRecording();
  const int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
  if (wide_length <= 0) return;
  std::wstring path(static_cast<size_t>(wide_length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, path.data(), wide_length);
  FILE *file = _wfopen(path.c_str(), L"rb");
  if (!file) {
    Log("Aufnahme fehlt: %s", utf8_path.c_str());
    return;
  }
  std::vector<char> bytes;
  char chunk[65536];
  size_t read;
  while ((read = fread(chunk, 1, sizeof(chunk), file)) > 0) bytes.insert(bytes.end(), chunk, chunk + read);
  fclose(file);
  // RIFF-Bloecke durchgehen: fmt liefert das Format, data die Samples.
  WAVEFORMATEX format{};
  bool have_format = false;
  size_t data_offset = 0, data_size = 0;
  if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    Log("Keine WAV-Datei: %s", utf8_path.c_str());
    return;
  }
  size_t position = 12;
  while (position + 8 <= bytes.size()) {
    uint32_t size = 0;
    memcpy(&size, bytes.data() + position + 4, 4);
    const size_t body = position + 8;
    if (memcmp(bytes.data() + position, "fmt ", 4) == 0 && size >= 16 && body + 16 <= bytes.size()) {
      memcpy(&format.wFormatTag, bytes.data() + body, 2);
      memcpy(&format.nChannels, bytes.data() + body + 2, 2);
      memcpy(&format.nSamplesPerSec, bytes.data() + body + 4, 4);
      memcpy(&format.nAvgBytesPerSec, bytes.data() + body + 8, 4);
      memcpy(&format.nBlockAlign, bytes.data() + body + 12, 2);
      memcpy(&format.wBitsPerSample, bytes.data() + body + 14, 2);
      have_format = true;
    } else if (memcmp(bytes.data() + position, "data", 4) == 0) {
      data_offset = body;
      data_size = std::min<size_t>(size, bytes.size() - body);
      break;
    }
    position = body + size + (size & 1);
  }
  if (!have_format || format.wFormatTag != WAVE_FORMAT_PCM || format.wBitsPerSample != 16 || !data_size) {
    Log("WAV-Format nicht unterstuetzt: %s", utf8_path.c_str());
    return;
  }
  g_recording.samples.assign(bytes.begin() + data_offset, bytes.begin() + data_offset + data_size);
  if (waveOutOpen(&g_recording.device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    Log("waveOutOpen fehlgeschlagen: %s", utf8_path.c_str());
    g_recording = Recording{};
    return;
  }
  g_recording.header.lpData = g_recording.samples.data();
  g_recording.header.dwBufferLength = static_cast<DWORD>(g_recording.samples.size());
  if (waveOutPrepareHeader(g_recording.device, &g_recording.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
      waveOutWrite(g_recording.device, &g_recording.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
    Log("Aufnahme nicht abspielbar: %s", utf8_path.c_str());
    StopRecording();
    return;
  }
  Log("AUFNAHME: %s", utf8_path.c_str());
}

void RecordingCommand(const std::string &command) {
  if (command.rfind("P", 0) == 0) {
    PlayRecording(command.substr(1));
  } else if (command == "S") {
    StopRecording();
  } else if (command == "H") {
    if (g_recording.device) waveOutPause(g_recording.device);
  } else if (command == "R") {
    if (g_recording.device) waveOutRestart(g_recording.device);
  }
}

// Raumklaenge: eigener Mischer ueber waveOut, beliebig viele Stimmen gleichzeitig (Waende, Ziele, Figuren).
// Lua steuert jede Stimme ueber eine ID; Lautstaerke, Balance, Tonhoehe und Daempfung gleiten weich,
// damit beim Drehen und Laufen nichts knackt.
namespace mixer {

constexpr int kRate = 44100;
constexpr int kBufferFrames = 1024;  // etwa 23 ms
constexpr int kBufferCount = 4;
constexpr double kGlide = 0.0015;     // Anteil je Sample, mit dem Werte ihrem Ziel folgen

struct Voice {
  const std::vector<float> *sample = nullptr;
  std::string sample_name;
  double position = 0;
  bool loop = true;
  bool stopping = false;
  double gain = 0, target_gain = 0;
  double pan = 0, target_pan = 0;
  double rate = 1, target_rate = 1;
  double muffle = 0, target_muffle = 0;
  double low_left = 0, low_right = 0;
};

CRITICAL_SECTION g_lock;
std::vector<std::pair<std::string, std::vector<float>>> g_samples;
std::vector<std::pair<std::string, Voice>> g_voices;
HWAVEOUT g_device = nullptr;
HANDLE g_event = nullptr;
WAVEHDR g_headers[kBufferCount]{};
std::vector<int16_t> g_buffers[kBufferCount];
double g_master = 1.0;

const std::vector<float> *FindSample(const std::string &name) {
  for (auto &entry : g_samples)
    if (entry.first == name) return &entry.second;
  return nullptr;
}

Voice *FindVoice(const std::string &id) {
  for (auto &entry : g_voices)
    if (entry.first == id) return &entry.second;
  return nullptr;
}

// WAV laden (PCM 16 Bit oder Float 32 Bit, mono oder stereo), zu mono mischen, auf 44,1 kHz bringen.
bool LoadWave(const std::wstring &path, std::vector<float> &out) {
  FILE *file = _wfopen(path.c_str(), L"rb");
  if (!file) return false;
  std::vector<char> bytes;
  char chunk[65536];
  size_t read;
  while ((read = fread(chunk, 1, sizeof(chunk), file)) > 0) bytes.insert(bytes.end(), chunk, chunk + read);
  fclose(file);
  if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    return false;
  uint16_t tag = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t data_offset = 0, data_size = 0, position = 12;
  while (position + 8 <= bytes.size()) {
    uint32_t size = 0;
    memcpy(&size, bytes.data() + position + 4, 4);
    const size_t body = position + 8;
    if (memcmp(bytes.data() + position, "fmt ", 4) == 0 && size >= 16 && body + 16 <= bytes.size()) {
      memcpy(&tag, bytes.data() + body, 2);
      memcpy(&channels, bytes.data() + body + 2, 2);
      memcpy(&rate, bytes.data() + body + 4, 4);
      memcpy(&bits, bytes.data() + body + 14, 2);
      if (tag == 0xFFFE && size >= 26) memcpy(&tag, bytes.data() + body + 24, 2);  // WAVE_FORMAT_EXTENSIBLE
    } else if (memcmp(bytes.data() + position, "data", 4) == 0) {
      data_offset = body;
      data_size = std::min<size_t>(size, bytes.size() - body);
      break;
    }
    position = body + size + (size & 1);
  }
  const bool pcm16 = tag == 1 && bits == 16, float32 = tag == 3 && bits == 32;
  if (!channels || !rate || !data_size || !(pcm16 || float32)) return false;
  const size_t frame_bytes = static_cast<size_t>(channels) * bits / 8;
  const size_t frames = data_size / frame_bytes;
  std::vector<float> mono(frames);
  for (size_t f = 0; f < frames; ++f) {
    double sum = 0;
    for (int c = 0; c < channels; ++c) {
      const char *p = bytes.data() + data_offset + f * frame_bytes + c * (bits / 8);
      if (pcm16) {
        int16_t v;
        memcpy(&v, p, 2);
        sum += v / 32768.0;
      } else {
        float v;
        memcpy(&v, p, 4);
        sum += v;
      }
    }
    mono[f] = static_cast<float>(sum / channels);
  }
  if (rate == kRate) {
    out.swap(mono);
    return true;
  }
  const double step = static_cast<double>(rate) / kRate;
  const size_t count = static_cast<size_t>(frames / step);
  out.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const double source = i * step;
    const size_t index = static_cast<size_t>(source);
    const double fraction = source - index;
    const float next = index + 1 < frames ? mono[index + 1] : mono[index];
    out[i] = static_cast<float>(mono[index] * (1 - fraction) + next * fraction);
  }
  return true;
}

void Mix(int16_t *target) {
  static std::vector<double> left, right;
  left.assign(kBufferFrames, 0);
  right.assign(kBufferFrames, 0);
  EnterCriticalSection(&g_lock);
  for (auto &entry : g_voices) {
    Voice &v = entry.second;
    const auto &s = *v.sample;
    const double length = static_cast<double>(s.size());
    if (length < 2) {
      v.stopping = true;
      v.gain = 0;
      continue;
    }
    for (int i = 0; i < kBufferFrames; ++i) {
      v.gain += (v.target_gain - v.gain) * kGlide * 2;
      v.pan += (v.target_pan - v.pan) * kGlide;
      v.rate += (v.target_rate - v.rate) * kGlide;
      v.muffle += (v.target_muffle - v.muffle) * kGlide;
      if (v.position >= length - 1) {
        if (!v.loop) {
          v.stopping = true;
          v.target_gain = v.gain = 0;
          break;
        }
        v.position = std::fmod(v.position, length - 1);
      }
      const size_t index = static_cast<size_t>(v.position);
      const double fraction = v.position - index;
      const double value = s[index] * (1 - fraction) + s[index + 1] * fraction;
      v.position += v.rate;
      // Gleichleistungs-Balance; die Daempfung (Ziel hinter der Figur) ist ein einfacher Tiefpass je Seite.
      const double angle = (v.pan + 1) * 0.78539816339744830962;
      const double coefficient = 1.0 - 0.92 * v.muffle;
      v.low_left += (value * std::cos(angle) - v.low_left) * coefficient;
      v.low_right += (value * std::sin(angle) - v.low_right) * coefficient;
      left[i] += v.low_left * v.gain;
      right[i] += v.low_right * v.gain;
    }
  }
  // Ausgeklungene Stimmen entfernen.
  g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                [](const auto &entry) {
                                  return entry.second.stopping && entry.second.gain < 0.0005;
                                }),
                 g_voices.end());
  const double master = g_master;
  LeaveCriticalSection(&g_lock);
  for (int i = 0; i < kBufferFrames; ++i) {
    // Weiche Begrenzung, falls viele Stimmen zusammenkommen.
    target[i * 2] = static_cast<int16_t>(std::tanh(left[i] * master) * 32000);
    target[i * 2 + 1] = static_cast<int16_t>(std::tanh(right[i] * master) * 32000);
  }
}

DWORD WINAPI MixThread(LPVOID) {
  for (;;) {
    WaitForSingleObject(g_event, 50);
    for (int b = 0; b < kBufferCount; ++b) {
      if (g_headers[b].dwFlags & WHDR_DONE) {
        Mix(g_buffers[b].data());
        g_headers[b].dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_device, &g_headers[b], sizeof(WAVEHDR));
      }
    }
  }
}

bool Start() {
  if (g_device) return true;
  InitializeCriticalSection(&g_lock);
  g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 2;
  format.nSamplesPerSec = kRate;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 4;
  format.nAvgBytesPerSec = kRate * 4;
  if (waveOutOpen(&g_device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(g_event), 0, CALLBACK_EVENT) !=
      MMSYSERR_NOERROR) {
    g_device = nullptr;
    Log("Mischer: waveOutOpen fehlgeschlagen");
    return false;
  }
  for (int b = 0; b < kBufferCount; ++b) {
    g_buffers[b].assign(kBufferFrames * 2, 0);
    g_headers[b].lpData = reinterpret_cast<LPSTR>(g_buffers[b].data());
    g_headers[b].dwBufferLength = kBufferFrames * 4;
    waveOutPrepareHeader(g_device, &g_headers[b], sizeof(WAVEHDR));
    waveOutWrite(g_device, &g_headers[b], sizeof(WAVEHDR));
  }
  HANDLE thread = CreateThread(nullptr, 0, MixThread, nullptr, 0, nullptr);
  if (thread) {
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(thread);
  }
  Log("Mischer bereit");
  return true;
}

std::vector<std::string> Split(const std::string &text) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t tab = text.find('\t', start);
    parts.push_back(text.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos) break;
    start = tab + 1;
  }
  return parts;
}

void StopAll() {
  if (!g_device) return;
  EnterCriticalSection(&g_lock);
  for (auto &entry : g_voices) {
    entry.second.stopping = true;
    entry.second.target_gain = 0;
  }
  LeaveCriticalSection(&g_lock);
}

// L<Name>\t<Pfad>                                   Klang laden (ersetzt einen gleichnamigen)
// P<ID>\t<Name>\t<Schleife 0/1>\t<Lautst.>\t<Balance>\t<Tempo>\t<Daempfung 0..1>
//                                                   Stimme starten oder, wenn schon da, nachfuehren
// S<ID>   Stimme ausblenden      X   alle ausblenden      M<Lautstaerke>   Gesamtlautstaerke
void Command(const std::string &command) {
  if (command.empty() || !Start()) return;
  const char kind = command[0];
  const auto parts = Split(command.substr(1));
  if (kind == 'L' && parts.size() >= 2) {
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, parts[1].c_str(), -1, nullptr, 0);
    std::wstring path(static_cast<size_t>(std::max(wide_length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, parts[1].c_str(), -1, path.data(), wide_length);
    std::vector<float> data;
    if (!LoadWave(path, data)) {
      Log("Klang nicht ladbar: %s", parts[1].c_str());
      return;
    }
    EnterCriticalSection(&g_lock);
    // Stimmen mit dem alten Klang zeigen auf seine Daten: erst beenden.
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                  [&](const auto &entry) { return entry.second.sample_name == parts[0]; }),
                   g_voices.end());
    bool replaced = false;
    for (auto &entry : g_samples) {
      if (entry.first == parts[0]) {
        entry.second.swap(data);
        replaced = true;
      }
    }
    if (!replaced) {
      g_samples.reserve(64);  // Zeiger der Stimmen bleiben gueltig, solange nicht umkopiert wird
      if (g_samples.size() < g_samples.capacity()) g_samples.emplace_back(parts[0], std::move(data));
    }
    LeaveCriticalSection(&g_lock);
  } else if (kind == 'P' && parts.size() >= 7) {
    EnterCriticalSection(&g_lock);
    const std::vector<float> *sample = FindSample(parts[1]);
    if (sample) {
      Voice *voice = FindVoice(parts[0]);
      const double gain = std::clamp(atof(parts[3].c_str()), 0.0, 2.0);
      const double pan = std::clamp(atof(parts[4].c_str()), -1.0, 1.0);
      const double rate = std::clamp(atof(parts[5].c_str()), 0.1, 4.0);
      const double muffle = std::clamp(atof(parts[6].c_str()), 0.0, 1.0);
      if (voice && voice->sample_name != parts[1]) {
        voice->stopping = true;
        voice->target_gain = 0;
        voice->sample_name += "\x01";  // alte Stimme klingt aus, die neue bekommt eine eigene ID
        for (auto &entry : g_voices)
          if (&entry.second == voice) entry.first += "\x01";
        voice = nullptr;
      }
      if (!voice) {
        Voice fresh;
        fresh.sample = sample;
        fresh.sample_name = parts[1];
        fresh.pan = pan;
        fresh.rate = rate;
        fresh.muffle = muffle;
        g_voices.emplace_back(parts[0], fresh);
        voice = &g_voices.back().second;
      }
      voice->loop = parts[2] == "1";
      voice->stopping = false;
      voice->target_gain = gain;
      voice->target_pan = pan;
      voice->target_rate = rate;
      voice->target_muffle = muffle;
    }
    LeaveCriticalSection(&g_lock);
  } else if (kind == 'S' && !parts.empty()) {
    EnterCriticalSection(&g_lock);
    if (Voice *voice = FindVoice(parts[0])) {
      voice->stopping = true;
      voice->target_gain = 0;
    }
    LeaveCriticalSection(&g_lock);
  } else if (kind == 'X') {
    StopAll();
  } else if (kind == 'M' && !parts.empty()) {
    EnterCriticalSection(&g_lock);
    g_master = std::clamp(atof(parts[0].c_str()), 0.0, 2.0);
    LeaveCriticalSection(&g_lock);
  }
}

}  // namespace mixer

void SpeakLine(PrismBackend *backend, const std::string &line) {
  if (line.size() < 2) return;
  if (line[0] == 'T') {
    PlayTones(line.substr(1));
    return;
  }
  if (line[0] == 'A') {
    RecordingCommand(line.substr(1));
    return;
  }
  if (line[0] == 'S') {
    mixer::Command(line.substr(1));
    return;
  }
  bool interrupt = line[0] == '1';
  std::string text = line.substr(1);
  PrismError error = p_backend_output(backend, text.c_str(), interrupt);
  if (error == PRISM_OK) {
    Log("GESPROCHEN: %s", text.c_str());
  } else {
    Log("FEHLER %s bei: %s", p_error_string(error), text.c_str());
  }
}

DWORD WINAPI PipeThread(LPVOID) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  PrismBackend *backend = CreateBackend();

  std::string pending;
  char buffer[4096];
  for (;;) {
    // ERROR_NO_DATA: Der Client hat schon wieder geschlossen, seine Daten liegen aber noch im Puffer.
    BOOL connected = ConnectNamedPipe(g_pipe, nullptr);
    DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
    if (connected || connect_error == ERROR_PIPE_CONNECTED || connect_error == ERROR_NO_DATA) {
      DWORD read = 0;
      while (ReadFile(g_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        pending.append(buffer, read);
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
          const std::string line = pending.substr(0, newline);
          // Aufnahmen und Raumklaenge brauchen kein Prism, Toene und Sprache schon.
          if (backend || (!line.empty() && (line[0] == 'A' || line[0] == 'S'))) SpeakLine(backend, line);
          pending.erase(0, newline + 1);
        }
      }
    }
    DisconnectNamedPipe(g_pipe);
    pending.clear();
    // Lua hat die Pipe geschlossen (Neuladen mit Strg+F9): Dauerklaenge sollen nicht weiterlaufen.
    mixer::StopAll();
    StopRecording();
  }
}

}  // namespace

// Wird von Lua als lua_CFunction aufgerufen; der lua_State bleibt ungenutzt, 0 = keine Rueckgabewerte.
extern "C" __declspec(dllexport) int ffrs_start(void *) {
  if (g_pipe != INVALID_HANDLE_VALUE) return 0;

  g_dir = OwnDirectory();
  g_log = _wfsopen((g_dir + L"\\ffrs_speech.log").c_str(), L"w", _SH_DENYNO);

  // Die Pipe entsteht schon hier, damit Lua sie direkt nach der Rueckkehr oeffnen kann.
  g_pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                            kPipeBufferSize, kPipeBufferSize, 0, nullptr);
  if (g_pipe == INVALID_HANDLE_VALUE) {
    Log("Pipe konnte nicht erstellt werden");
    return 0;
  }
  HANDLE thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
  if (thread) CloseHandle(thread);
  return 0;
}
