// Naziv i lozinka za Wi-Fi mrežu
#define WIFI_SSID "ssid"
#define WIFI_PASS "password"

// Objedinjena biblioteka za M5Stack uredjaje
#include <M5Unified.h>

// Biblioteke za audio reprodukciju
#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <AudioOutputI2S.h>

// Konstanta za virtuelni kanal
static constexpr uint8_t m5_spk_virtual_channel = 0;

// Lista web radio stanica sa nazivima i URL-ovima strimova
static constexpr const char *radio_stations[][2] = {
    {"NES Radio", "http://188.40.62.20:8070/stream"},
    {"OKRADIO", "http://live3.okradio.net:8002"},
    {"Kontakt Radio", "http://mojstream.eu:8114/;stream.mp3"},
    {"Classic FM", "http://media-ice.musicradio.com:80/ClassicFMMP3"}};

static constexpr const size_t num_stations =
    sizeof(radio_stations) / sizeof(radio_stations[0]);

// Klasa za izlaz na M5 zvučnik
class AudioOutputM5Speaker : public AudioOutput {
public:
  AudioOutputM5Speaker(m5::Speaker_Class *m5_sound,
                       uint8_t virtual_sound_channel = 0) {
    _m5_sound = m5_sound;
    _virtual_channel = virtual_sound_channel;
  }

  virtual bool begin(void) override { return true; }

  virtual bool ConsumeSample(int16_t sample[2]) override {
    if (_tri_buffer_index < tri_buffer_size) {
      _tri_buffer[_tri_index][_tri_buffer_index++] = sample[0];
      _tri_buffer[_tri_index][_tri_buffer_index++] = sample[1];
      return true;
    }
    flush();
    return false;
  }

  virtual void flush(void) override {
    if (_tri_buffer_index) {
      _m5_sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index,
                         hertz, true, 1, _virtual_channel);
      _tri_index = (_tri_index + 1) % 3;
      _tri_buffer_index = 0;
    }
  }

  virtual bool stop(void) override {
    flush();
    _m5_sound->stop(_virtual_channel);
    for (size_t i = 0; i < 3; ++i) {
      memset(_tri_buffer[i], 0, tri_buffer_size * sizeof(int16_t));
    }
    return true;
  }

protected:
  static constexpr size_t tri_buffer_size = 640;
  m5::Speaker_Class *_m5_sound;
  uint8_t _virtual_channel;
  int16_t _tri_buffer[3][tri_buffer_size];
  size_t _tri_buffer_index = 0;
  size_t _tri_index = 0;
};

// Konstante za audio buffer i codec
static constexpr const int preallocate_buffer_size = 5 * 1024;
static constexpr const int preallocate_codec_size = 29192;
static void *preallocate_buffer = nullptr;
static void *preallocate_codec = nullptr;
static AudioOutputM5Speaker out(&M5.Speaker, m5_spk_virtual_channel);
static AudioGenerator *decoder = nullptr;
static AudioFileSourceICYStream *file = nullptr;
static AudioFileSourceBuffer *buff = nullptr;
static size_t station_index = 0;
static const char *meta_text = nullptr;
static volatile size_t play_index = ~0u;
static bool changed = false;

// Funkcija za zaustavljanje audio reprodukcije
static void stop_audio(void) {
  if (decoder) {
    decoder->stop();
    delete decoder;
    decoder = nullptr;
  }

  if (buff) {
    buff->close();
    delete buff;
    buff = nullptr;
  }
  if (file) {
    file->close();
    delete file;
    file = nullptr;
  }
  out.stop();
}

// Funkcija za pokretanje audio reprodukcije
static void play_audio(size_t index) { play_index = index; }

// Zadatak za dekodiranje audio strima
static void decode_task(void *) {
  for (;;) {
    delay(1);
    if (play_index != ~0u) {
      auto index = play_index;
      play_index = ~0u;
      stop_audio();
      changed = true;
      meta_text = radio_stations[index][0];
      file = new AudioFileSourceICYStream(radio_stations[index][1]);
      buff = new AudioFileSourceBuffer(file, preallocate_buffer,
                                       preallocate_buffer_size);
      decoder = new AudioGeneratorMP3(preallocate_codec,
                                      preallocate_codec_size);
      decoder->begin(buff, &out);
    }
    if (decoder && decoder->isRunning()) {
      if (!decoder->loop()) {
        decoder->stop();
      }
    }
  }
}

// Postavke grafičkog prikaza na M5Stack ekranu
static void graphics_setup(LGFX_Device *gfx) {
  gfx->setTextSize(1.5);
  gfx->setTextDatum(BL_DATUM);
  gfx->drawString("STANICA", 0, 240);
  gfx->setTextDatum(BC_DATUM);
  gfx->drawString("SMANJI", 160, 240);
  gfx->setTextDatum(BR_DATUM);
  gfx->drawString("POJACAJ", 320, 240);
  gfx->display();
}

// Glavna petlja za grafički prikaz
void graphics_loop(LGFX_Device *gfx) {
  if (changed) {
    gfx->fillRect(0, 20, gfx->width(), 200, gfx->getBaseColor());
    gfx->setTextSize(3);
    gfx->setTextDatum(MC_DATUM);
    gfx->drawString(meta_text, 160, 120);
    gfx->display();
    changed = false;
  }

  // Crtaj traku za volumen
  if (!gfx->displayBusy()) {
    // Inicijalizuj na nevalidnu vrednost
    static int prev_x = -1;
    uint8_t volume = M5.Speaker.getVolume();

    // Mapiraj glasnoću na širinu ekrana
    int x = map(volume, 0, 255, 0, gfx->width());

    if (prev_x != x) {
      int start_x = min(prev_x, x);
      int width = abs(prev_x - x);
      gfx->fillRect(start_x, 0, width, 10,
                    start_x < x ? 0xAAFFAAu : 0u);
      gfx->display();
      prev_x = x;
    }
  }
}

void setup(void) {
  M5.begin();
  M5.Display.setTextSize(1.5);

  preallocate_buffer = malloc(preallocate_buffer_size);
  preallocate_codec = malloc(preallocate_codec_size);
  if (!preallocate_buffer || !preallocate_codec) {
    M5.Display.printf(
        "FATALNA GRESKA: Nemoguce alocirati %d bajta za aplikaciju\n",
        preallocate_buffer_size + preallocate_codec_size);
    for (;;) {
      delay(1000);
    }
  }

  {
    /// Prilagođene postavke za zvuk
    auto spk_cfg = M5.Speaker.config();

    /// Povećanje uzorkovanja će poboljšati kvalitet zvuka umesto
    /// povećanja opterećenja CPU-a. Podrazumevano: 64000 (64kHz)  npr.
    /// 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 ,
    /// 192000 , 200000
    spk_cfg.sample_rate = 96000;

    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk_cfg);
  }

  M5.Speaker.begin();

  M5.Display.println("Povezivanje na Wi-Fi mrezu");
  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

#if defined(WIFI_SSID) && defined(WIFI_PASS)
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#else
  WiFi.begin();
#endif

  // Pokušavaj beskrajno dok se ne povežeš na Wi-Fi mrežu
  while (WiFi.status() != WL_CONNECTED) {
    M5.Display.print(".");
    delay(100);
  }
  M5.Display.clear();

  graphics_setup(&M5.Display);

  play_audio(station_index);

  xTaskCreatePinnedToCore(decode_task, "decode_task", 4096, nullptr, 1,
                          nullptr, PRO_CPU_NUM);
}

void loop(void) {
  graphics_loop(&M5.Display);

  delay(8);

  M5.update();
  if (M5.BtnA.wasPressed()) {
    M5.Speaker.tone(440, 50);
  }
  if (M5.BtnA.wasDecideClickCount()) {
    switch (M5.BtnA.getClickCount()) {
    case 1:
      M5.Speaker.tone(1000, 100);
      if (++station_index >= num_stations) {
        station_index = 0;
      }
      play_audio(station_index);
      break;

    case 2:
      M5.Speaker.tone(800, 100);
      if (station_index == 0) {
        station_index = num_stations;
      }
      play_audio(--station_index);
      break;
    }
  }
  if (M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
    size_t v = M5.Speaker.getVolume();
    int add = (M5.BtnB.isPressed()) ? -1 : 1;
    v += add;
    if (v <= 255) {
      M5.Speaker.setVolume(v);
    }
  }
}
