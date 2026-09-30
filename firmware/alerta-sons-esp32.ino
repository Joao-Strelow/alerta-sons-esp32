#define EIDSP_QUANTIZE_FILTERBANK 0   // economiza ~10K de RAM

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_NeoPixel.h>
#include <alerta-sons_inferencing.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"
#include <strings.h>

#if EI_CLASSIFIER_FREQUENCY != 16000
#error "O modelo do Edge Impulse precisa ser de 16000 Hz."
#endif

// ===================== CONFIGURAÇÕES =====================

// ----- Nó -----
#define NODE_ID "no1"                 // "no2" no segundo ESP32

// ----- WiFi (somente 2.4 GHz) -----
const char* WIFI_SSID = "AMIGO_JULIA";
const char* WIFI_PASS = "juliabrt";

// ----- Supabase -----
const char* SUPABASE_URL = "https://<seu-projeto>.supabase.co";
const char* SUPABASE_KEY = "<chave anon/publishable>";
#define ENVIAR_SUPABASE_PADRAO false   // estado inicial (troque em tempo real com o comando "s 1" / "s 0")
#define HEARTBEAT_MS 30000             // intervalo do sinal de vida usado no status online/offline

// ----- Edge Impulse (para gravar amostras) -----
const char* EI_API_KEY = "ei_8a0a277b7f230c9d4c359136a0e114c3c5826198dfa922d7";

// ----- Detecção -----
#define CONFIANCA_MIN 0.80            // confiança mínima (0.0 a 1.0) para considerar detecção
#define COOLDOWN      3000            // ms mínimos entre dois eventos

// ----- Microfone INMP441 (I2S) -----
#define I2S_SCK 33
#define I2S_WS  25
#define I2S_SD  32
#define I2S_PORT ((i2s_port_t)0)
#define SAMPLE_RATE 16000
#define N_SAMPLES   16000             // 1 segundo (gravação para o Edge Impulse)
#define BLOCO       256
// Estes valores DEVEM ser iguais aos usados quando as amostras foram gravadas:
#define DESLOC 16
#define GANHO  1

// ----- LED WS2812B -----
#define LED_PIN 4
#define LED_COUNT 2
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

#define BOUNDARY "----esp32ei7c3f9a"

// ===================== ESTADO GLOBAL =====================

struct Evento {
  char tipo[16];
  float conf;
};

typedef struct {
  signed short *buffers[2];
  unsigned char buf_select;
  unsigned char buf_ready;
  unsigned int buf_count;
  unsigned int n_samples;
} inference_t;

static inference_t inference;
static signed short sampleBuffer[BLOCO];
static bool debug_nn = false;
static bool record_status = true;

volatile bool capturaAtiva = true;
volatile bool capturaParada = false;
volatile bool acaoOcupada = false;

QueueHandle_t filaEventos;

bool enviarSupabase = ENVIAR_SUPABASE_PADRAO;
bool logPredicoes = true;
unsigned long ultimoEvento = 0;
int aquecimento = EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW;
int print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

// buffers usados só durante a gravação de amostras
uint8_t* wav = NULL;
uint8_t* corpoHttp = NULL;
int contador = 0;
int ultimoPico = 0;
int ultimoRms = 0;

// ===================== LED =====================

void cor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

void piscar(uint8_t r, uint8_t g, uint8_t b, int vezes, int ms) {
  for (int i = 0; i < vezes; i++) {
    cor(r, g, b); delay(ms);
    cor(0, 0, 0); delay(ms);
  }
}

// ===================== WIFI =====================

bool garantirWiFi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t ini = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - ini < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

// ===================== SUPABASE =====================

int enviarEventoSupabase(const char* tipo, float conf) {
  if (!garantirWiFi(8000)) return -1;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);   // buffers TLS pequenos: sobra RAM para a inferencia
  HTTPClient https;
  https.begin(client, String(SUPABASE_URL) + "/rest/v1/eventos");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  long intensidade = (long)(conf * 100);
  String corpo = String("{\"node_id\":\"") + NODE_ID +
                 "\",\"tipo\":\"" + tipo +
                 "\",\"intensidade\":" + intensidade + "}";
  int code = https.POST(corpo);
  https.end();
  return code;
}

// Grava/atualiza o "sinal de vida" deste no na tabela nodes (usado pelo app para status online/offline)
int enviarHeartbeatSupabase() {
  if (!garantirWiFi(8000)) return -1;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);   // buffers TLS pequenos: sobra RAM para a inferencia
  HTTPClient https;
  https.begin(client, String(SUPABASE_URL) + "/rest/v1/nodes?on_conflict=node_id");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");

  String corpo = String("{\"node_id\":\"") + NODE_ID + "\",\"last_seen\":\"now()\"}";
  int code = https.POST(corpo);
  https.end();
  return code;
}

// Tarefa separada que manda o heartbeat periodicamente, sem travar a escuta do microfone
void tarefaHeartbeat(void* arg) {
  for (;;) {
    int code = enviarHeartbeatSupabase();
    Serial.printf("Heartbeat -> %d\n", code);
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
  }
}

// Tarefa que pisca o LED e envia ao Supabase sem travar a escuta do microfone
void tarefaAcoes(void* arg) {
  Evento ev;
  for (;;) {
    if (xQueueReceive(filaEventos, &ev, portMAX_DELAY) == pdTRUE) {
      acaoOcupada = true;
      bool ehPalma = (strcmp(ev.tipo, "palma") == 0);

      Serial.printf(">>> DETECTADO: %s (%.0f%%)\n", ev.tipo, ev.conf * 100);
      if (ehPalma) piscar(255, 0, 0, 3, 120);      // vermelho = palma
      else         piscar(255, 180, 0, 3, 120);    // amarelo = campainha

      if (enviarSupabase) {
        int code = enviarEventoSupabase(ev.tipo, ev.conf);
        Serial.printf("Supabase POST %s -> %d\n", ev.tipo, code);
        if (code >= 200 && code < 300) piscar(0, 200, 200, 1, 150);   // ciano = enviado
        else                           piscar(255, 0, 80, 2, 150);    // rosa = falhou
      }
      cor(0, 0, 0);
      acaoOcupada = false;
    }
  }
}

// ===================== I2S / INFERÊNCIA =====================

void i2sInit() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD,
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

static void audio_inference_callback(uint32_t n_bytes) {
  for (int i = 0; i < (int)(n_bytes >> 1); i++) {
    inference.buffers[inference.buf_select][inference.buf_count++] = sampleBuffer[i];

    if (inference.buf_count >= inference.n_samples) {
      inference.buf_select ^= 1;
      inference.buf_count = 0;
      inference.buf_ready = 1;
    }
  }
}

// Tarefa que lê o microfone o tempo todo (pode ser pausada pelos comandos)
static void capture_samples(void* arg) {
  static int32_t raw[BLOCO];
  size_t bytes_read = 0;

  while (record_status) {
    if (!capturaAtiva) {
      capturaParada = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    capturaParada = false;

    bytes_read = 0;
    i2s_read(I2S_PORT, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(100));
    if (bytes_read == 0) continue;

    int n = bytes_read / 4;
    for (int i = 0; i < n; i++) {
      int32_t v = (raw[i] >> DESLOC) * GANHO;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      sampleBuffer[i] = (int16_t)v;
    }
    audio_inference_callback(n * 2);
  }
  vTaskDelete(NULL);
}

static bool microphone_inference_start(uint32_t n_samples) {
  inference.buffers[0] = (signed short *)malloc(n_samples * sizeof(signed short));
  if (inference.buffers[0] == NULL) return false;

  inference.buffers[1] = (signed short *)malloc(n_samples * sizeof(signed short));
  if (inference.buffers[1] == NULL) {
    free(inference.buffers[0]);
    return false;
  }

  inference.buf_select = 0;
  inference.buf_count = 0;
  inference.n_samples = n_samples;
  inference.buf_ready = 0;
  return true;
}

static bool microphone_inference_record(void) {
  if (inference.buf_ready == 1) {
    Serial.println("Aviso: buffer de audio atrasado (overrun).");
  }
  while (inference.buf_ready == 0) {
    delay(1);
  }
  inference.buf_ready = 0;
  return true;
}

static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr) {
  numpy::int16_to_float(&inference.buffers[inference.buf_select ^ 1][offset], out_ptr, length);
  return 0;
}

// ----- pausar / retomar a escuta para os comandos usarem o microfone -----

void aguardarAcoes() {
  while (acaoOcupada || uxQueueMessagesWaiting(filaEventos) > 0) delay(20);
  delay(50);
}

void pausarCaptura() {
  capturaAtiva = false;
  while (!capturaParada) delay(5);
}

void retomarCaptura() {
  i2s_zero_dma_buffer(I2S_PORT);
  inference.buf_select = 0;
  inference.buf_count = 0;
  inference.buf_ready = 0;
  run_classifier_init();
  aquecimento = EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW;
  print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);
  capturaAtiva = true;
}

// ===================== DETECÇÃO =====================

void tratarDeteccao(const char* label, float conf) {
  if (aquecimento > 0) return;
  if (conf < CONFIANCA_MIN) return;
  if (millis() - ultimoEvento < COOLDOWN) return;

  const char* tipo = NULL;
  if (strcasecmp(label, "palma") == 0) tipo = "palma";
  else if (strcasecmp(label, "campainha") == 0) tipo = "campainha";
  else return;   // "ruido" e outros: não faz nada

  ultimoEvento = millis();
  Evento ev;
  strncpy(ev.tipo, tipo, sizeof(ev.tipo) - 1);
  ev.tipo[sizeof(ev.tipo) - 1] = 0;
  ev.conf = conf;
  xQueueSend(filaEventos, &ev, 0);
}

// ===================== TESTE DO MIC (comando "m") =====================

void monitorarMic() {
  Serial.println("Monitorando o microfone. Faca barulho perto dele.");
  Serial.println("Envie qualquer tecla (ou Enter) para parar.");
  while (Serial.available()) Serial.read();

  int32_t raw[BLOCO];
  size_t br;
  while (!Serial.available()) {
    double soma = 0, somaQ = 0;
    long n = 0;
    int pico = 0;
    int32_t primeiro = 0;
    for (int b = 0; b < 8; b++) {            // 8 blocos de 256 amostras = ~128 ms
      i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
      size_t k = br / 4;
      if (b == 0 && k > 0) primeiro = raw[0];
      for (size_t i = 0; i < k; i++) {
        int32_t v = (raw[i] >> DESLOC) * GANHO;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        int a = v < 0 ? -v : v;
        if (a > pico) pico = a;
        soma += v;
        somaQ += (double)v * v;
        n++;
      }
    }
    double media = soma / n;
    double var = somaQ / n - media * media;
    int rms = (int)sqrt(var > 0 ? var : 0);
    int barras = (int)(log10((double)rms + 1) * 10);   // escala logarítmica
    if (barras > 50) barras = 50;
    char barra[52];
    for (int i = 0; i < barras; i++) barra[i] = '#';
    barra[barras] = 0;
    Serial.printf("rms=%5d pico=%5d raw0=0x%08X %s%s\n", rms, pico, (unsigned int)primeiro,
                  barra, pico >= 32767 ? " <- ESTOURADO" : "");
  }
  while (Serial.available()) Serial.read();
  Serial.println("Monitor parado.");
}

// ===================== GRAVAR AMOSTRAS PARA O EDGE IMPULSE =====================

bool alocarBuffersCaptura() {
  wav = (uint8_t*)malloc(44 + N_SAMPLES * 2);
  corpoHttp = (uint8_t*)malloc(44 + N_SAMPLES * 2 + 512);
  if (wav == NULL || corpoHttp == NULL) {
    if (wav) free(wav);
    if (corpoHttp) free(corpoHttp);
    wav = NULL; corpoHttp = NULL;
    return false;
  }
  return true;
}

void liberarBuffersCaptura() {
  if (wav) free(wav);
  if (corpoHttp) free(corpoHttp);
  wav = NULL; corpoHttp = NULL;
}

void escreverCabecalhoWav() {
  uint32_t dataBytes = N_SAMPLES * 2;
  uint32_t chunk = 36 + dataBytes, sub1 = 16, sr = SAMPLE_RATE, br = SAMPLE_RATE * 2;
  uint16_t fmt = 1, ch = 1, ba = 2, bps = 16;
  memcpy(wav,      "RIFF", 4); memcpy(wav + 4,  &chunk, 4);
  memcpy(wav + 8,  "WAVEfmt ", 8); memcpy(wav + 16, &sub1, 4);
  memcpy(wav + 20, &fmt, 2);   memcpy(wav + 22, &ch, 2);
  memcpy(wav + 24, &sr, 4);    memcpy(wav + 28, &br, 4);
  memcpy(wav + 32, &ba, 2);    memcpy(wav + 34, &bps, 2);
  memcpy(wav + 36, "data", 4); memcpy(wav + 40, &dataBytes, 4);
}

// Descarta ~0,5s de áudio: limpa o buffer antigo e deixa o mic estabilizar
void descartar() {
  size_t br;
  int32_t raw[BLOCO];
  for (int i = 0; i < 32; i++) i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
}

// Grava exatamente 1 segundo
void gravar() {
  int16_t* pcm = (int16_t*)(wav + 44);
  size_t br;
  int32_t raw[BLOCO];
  int pico = 0;
  double soma = 0;

  size_t total = 0;
  while (total < N_SAMPLES) {
    i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
    size_t n = br / 4;
    for (size_t i = 0; i < n && total < N_SAMPLES; i++) {
      int32_t v = (raw[i] >> DESLOC) * GANHO;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      int a = v < 0 ? -v : v;
      if (a > pico) pico = a;
      soma += (double)v * v;
      pcm[total++] = (int16_t)v;
    }
  }
  ultimoPico = pico;
  ultimoRms = (int)sqrt(soma / N_SAMPLES);
}

int enviarAmostraEI(const char* label) {
  bool teste = (contador % 5 == 4);   // 1 a cada 5 vai para teste (~20%)

  String nomeArq = String(label) + "." + String(millis()) + ".wav";
  char prefixo[300];
  int plen = snprintf(prefixo, sizeof(prefixo),
    "--%s\r\nContent-Disposition: form-data; name=\"data\"; filename=\"%s\"\r\n"
    "Content-Type: audio/wav\r\n\r\n", BOUNDARY, nomeArq.c_str());
  char sufixo[64];
  int slen = snprintf(sufixo, sizeof(sufixo), "\r\n--%s--\r\n", BOUNDARY);

  size_t wavLen = 44 + N_SAMPLES * 2;
  memcpy(corpoHttp, prefixo, plen);
  memcpy(corpoHttp + plen, wav, wavLen);
  memcpy(corpoHttp + plen + wavLen, sufixo, slen);
  size_t total = plen + wavLen + slen;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://ingestion.edgeimpulse.com/api/") +
               (teste ? "testing" : "training") + "/files";
  https.begin(client, url);
  https.addHeader("x-api-key", EI_API_KEY);
  https.addHeader("x-label", label);
  https.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);
  int code = https.POST(corpoHttp, total);
  if (code != 200) Serial.println(https.getString());
  https.end();
  contador++;
  return code;
}

void gravarParaEdgeImpulse(const char* nome, int qtd) {
  if (!alocarBuffersCaptura()) {
    Serial.println("Sem memoria para gravar. Tente de novo.");
    return;
  }
  if (!garantirWiFi(10000)) {
    Serial.println("Sem WiFi. Nao foi possivel enviar ao Edge Impulse.");
    liberarBuffersCaptura();
    return;
  }
  escreverCabecalhoWav();

  for (int i = 0; i < qtd; i++) {
    cor(140, 0, 255);                 // roxo = prepare-se (ainda NAO grava)
    descartar();
    if (qtd == 1) delay(700);
    cor(255, 255, 255);               // branco = GRAVANDO 1s, faca o som agora
    gravar();
    cor(0, 0, 255);                   // azul = enviando
    int code = enviarAmostraEI(nome);
    Serial.printf("[%d/%d] %s -> HTTP %d | pico=%d rms=%d%s\n",
                  i + 1, qtd, nome, code, ultimoPico, ultimoRms,
                  ultimoPico >= 32767 ? " <- ESTOURADO" : "");
    if (code == 200) { cor(0, 255, 0); delay(150); }     // verde = ok
    else             { cor(255, 0, 80); delay(1000); }   // rosa = falhou
    cor(0, 0, 0);
  }
  liberarBuffersCaptura();
  Serial.println("Pronto.");
}

// ===================== COMANDOS =====================

void mostrarAjuda() {
  Serial.println("-------- COMANDOS --------");
  Serial.println("s 1 / s 0  = liga / desliga envio ao Supabase (s = mostra estado)");
  Serial.println("m          = monitorar microfone (qualquer tecla para parar)");
  Serial.println("c N        = gravar N amostras de campainha para o Edge Impulse");
  Serial.println("p N        = gravar N amostras de palma para o Edge Impulse");
  Serial.println("r N        = gravar N amostras de ruido para o Edge Impulse");
  Serial.println("l 1 / l 0  = liga / desliga log das probabilidades");
  Serial.println("?          = mostra esta ajuda");
  Serial.println("--------------------------");
  Serial.printf("Envio ao Supabase: %s | Log: %s\n",
                enviarSupabase ? "LIGADO" : "DESLIGADO", logPredicoes ? "LIGADO" : "DESLIGADO");
}

void processarComando(String linha) {
  linha.trim();
  linha.toLowerCase();
  if (linha.length() == 0) return;

  String nome = linha;
  String arg = "";
  int esp = linha.indexOf(' ');
  if (esp > 0) {
    nome = linha.substring(0, esp);
    arg = linha.substring(esp + 1);
    arg.trim();
  }

  if (nome == "?" || nome == "h") { mostrarAjuda(); return; }

  if (nome == "s") {
    if (arg == "1" || arg == "on") enviarSupabase = true;
    else if (arg == "0" || arg == "off") enviarSupabase = false;
    Serial.printf("Envio ao Supabase: %s\n", enviarSupabase ? "LIGADO" : "DESLIGADO");
    return;
  }

  if (nome == "l") {
    if (arg == "1" || arg == "on") logPredicoes = true;
    else if (arg == "0" || arg == "off") logPredicoes = false;
    else logPredicoes = !logPredicoes;
    Serial.printf("Log das probabilidades: %s\n", logPredicoes ? "LIGADO" : "DESLIGADO");
    return;
  }

  if (nome == "m") {
    aguardarAcoes();
    pausarCaptura();
    monitorarMic();
    retomarCaptura();
    return;
  }

  if (nome == "c" || nome == "p" || nome == "r") {
    int qtd = arg.toInt();
    if (qtd < 1) qtd = 1;
    const char* label = (nome == "c") ? "campainha" : (nome == "p") ? "palma" : "ruido";
    aguardarAcoes();
    pausarCaptura();
    gravarParaEdgeImpulse(label, qtd);
    retomarCaptura();
    return;
  }

  Serial.println("Comando desconhecido. Digite ? para ver a ajuda.");
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(115200);
  delay(500);

  strip.begin();
  strip.setBrightness(80);
  cor(0, 0, 0);

  cor(0, 0, 255);   // azul = conectando ao WiFi
  Serial.print("Conectando WiFi");
  if (garantirWiFi(20000)) {
    Serial.println(" ok");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" FALHOU. Continuando sem WiFi (tentarei reconectar quando precisar).");
  }
  cor(0, 0, 0);

  Serial.printf("Modelo: janela %d ms, %d classes\n",
                EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16, EI_CLASSIFIER_LABEL_COUNT);

  run_classifier_init();
  if (!microphone_inference_start(EI_CLASSIFIER_SLICE_SIZE)) {
    Serial.println("ERRO: sem memoria para o buffer de audio.");
    for (;;) { piscar(255, 0, 80, 1, 300); }
  }

  i2sInit();
  filaEventos = xQueueCreate(4, sizeof(Evento));
  xTaskCreate(capture_samples, "Captura", 8192, NULL, 10, NULL);
  xTaskCreate(tarefaAcoes, "Acoes", 16384, NULL, 1, NULL);
  xTaskCreate(tarefaHeartbeat, "Heartbeat", 8192, NULL, 1, NULL);

  piscar(0, 255, 0, 2, 150);   // verde 2x = pronto
  mostrarAjuda();
  Serial.println("Escutando...");
}

void loop() {
  if (Serial.available()) processarComando(Serial.readStringUntil('\n'));

  microphone_inference_record();

  signal_t signal;
  signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
  signal.get_data = &microphone_audio_signal_get_data;
  ei_impulse_result_t result = {0};

  EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, debug_nn);
  if (r != EI_IMPULSE_OK) {
    Serial.printf("ERRO ao classificar (%d)\n", r);
    return;
  }

  if (aquecimento > 0) aquecimento--;

  // classe com maior probabilidade nesta janela
  float melhorValor = 0;
  const char* melhorLabel = "";
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    if (result.classification[ix].value > melhorValor) {
      melhorValor = result.classification[ix].value;
      melhorLabel = result.classification[ix].label;
    }
  }
  tratarDeteccao(melhorLabel, melhorValor);

  if (++print_results >= (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)) {
    if (logPredicoes) {
      for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        Serial.printf("%s=%.2f  ", result.classification[ix].label, result.classification[ix].value);
      }
      Serial.println();
    }
    print_results = 0;
  }
}
